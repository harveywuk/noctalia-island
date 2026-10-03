#include "launcher/file_provider.h"

#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "system/terminal_launch.h"
#include "time/time_format.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

  constexpr std::size_t kMaxEntries = 80000;
  constexpr int kMaxDepth = 6;
  constexpr std::size_t kMinQueryChars = 2;
  constexpr std::size_t kGlobalResults = 6;
  constexpr std::size_t kPrefixedResults = 50;
  constexpr auto kIndexMaxAge = std::chrono::minutes(5);
  constexpr std::string_view kResultPrefix = "file:";
  constexpr std::size_t kPreviewTextBytes = 4096;

  // Build output and dependency trees that would bury real documents.
  constexpr std::array<std::string_view, 9> kSkippedDirs = {
      "node_modules", "__pycache__", "target", "build", "dist", "venv", "site-packages", "snap", "go",
  };

  [[nodiscard]] bool skippedDir(std::string_view name) {
    return std::ranges::find(kSkippedDirs, name) != kSkippedDirs.end();
  }

  [[nodiscard]] std::string extensionLower(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
      return {};
    }
    return StringUtils::toLower(name.substr(dot + 1));
  }

  struct FileIcon {
    std::string_view iconName;
    std::string_view glyph;
  };

  [[nodiscard]] FileIcon iconFor(std::string_view nameLower, bool isDir) {
    if (isDir) {
      return {"folder", "folder"};
    }
    const std::string ext = extensionLower(nameLower);
    const auto in = [&ext](std::initializer_list<std::string_view> list) {
      return std::ranges::find(list, ext) != list.end();
    };
    if (in({"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "tif", "tiff", "heic", "avif", "jxl"})) {
      return {"image-x-generic", "photo"};
    }
    if (in({"mp3", "flac", "ogg", "opus", "wav", "m4a", "aac"})) {
      return {"audio-x-generic", "music"};
    }
    if (in({"mp4", "mkv", "webm", "mov", "avi"})) {
      return {"video-x-generic", "movie"};
    }
    if (ext == "pdf") {
      return {"application-pdf", "file-type-pdf"};
    }
    if (in({"zip", "tar", "gz", "xz", "zst", "bz2", "7z", "rar"})) {
      return {"package-x-generic", "file-zip"};
    }
    if (in({"odt", "doc", "docx", "rtf"})) {
      return {"x-office-document", "file-text"};
    }
    if (in({"ods", "xls", "xlsx", "csv"})) {
      return {"x-office-spreadsheet", "table"};
    }
    if (in({"odp", "ppt", "pptx", "key"})) {
      return {"x-office-presentation", "presentation"};
    }
    return {"text-x-generic", "file"};
  }

  [[nodiscard]] bool isPreviewableImage(std::string_view nameLower) {
    const std::string ext = extensionLower(nameLower);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" || ext == "bmp";
  }

  [[nodiscard]] bool isTextFile(std::string_view nameLower) {
    static constexpr std::array<std::string_view, 34> kText = {
        "txt", "md",   "markdown", "rst", "org",  "log", "csv", "tsv", "json", "toml", "yaml", "yml",
        "ini", "conf", "cfg",      "xml", "html", "css", "js",  "ts",  "tsx",  "jsx",  "py",   "sh",
        "zsh", "fish", "c",        "h",   "cpp",  "hpp", "rs",  "go",  "lua",  "nix",
    };
    const std::string ext = extensionLower(nameLower);
    return std::ranges::find(kText, ext) != kText.end();
  }

  [[nodiscard]] std::string homeDir() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string();
  }

  // Parent folder for the subtitle, with the home directory shortened to ~.
  [[nodiscard]] std::string displayParent(std::string_view path, std::size_t nameOffset, std::string_view home) {
    std::string_view parent = path.substr(0, nameOffset > 0 ? nameOffset - 1 : 0);
    if (!home.empty() && parent.starts_with(home)) {
      std::string out = "~";
      out += parent.substr(home.size());
      return out;
    }
    return std::string(parent);
  }

} // namespace

struct FileProvider::State {
  std::mutex mutex;
  std::shared_ptr<const std::vector<Entry>> index;
  std::chrono::steady_clock::time_point indexedAt;
  std::atomic<bool> scanning{false};
  std::atomic<bool> alive{true};
  // Touched only on the main thread.
  std::function<void()> onChanged;
};

FileProvider::FileProvider(ClipboardService* clipboard, std::filesystem::path root)
    : m_clipboard(clipboard), m_root(root.empty() ? std::filesystem::path(homeDir()) : std::move(root)),
      m_state(std::make_shared<State>()) {}

FileProvider::~FileProvider() { m_state->alive = false; }

std::string FileProvider::displayName() const { return i18n::tr("launcher.providers.files.title"); }

void FileProvider::setResultsChangedCallback(std::function<void()> callback) {
  m_state->onChanged = std::move(callback);
}

bool FileProvider::isLoading() const {
  std::scoped_lock lock(m_state->mutex);
  return m_state->index == nullptr && m_state->scanning;
}

std::vector<FileProvider::Entry> FileProvider::scan(const std::filesystem::path& root) {
  std::vector<Entry> entries;
  std::error_code ec;
  if (root.empty() || !std::filesystem::is_directory(root, ec)) {
    return entries;
  }
  auto it = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, ec
  );
  for (const auto end = std::filesystem::recursive_directory_iterator(); !ec && it != end; it.increment(ec)) {
    const std::filesystem::directory_entry& entry = *it;
    const std::string name = entry.path().filename().string();
    std::error_code typeEc;
    const bool isDir = entry.is_directory(typeEc) && !entry.is_symlink(typeEc);
    // Hidden files, dotfolders and build trees are noise in a document search.
    if (name.empty() || name.front() == '.' || (isDir && skippedDir(name))) {
      if (isDir) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (isDir && it.depth() + 1 >= kMaxDepth) {
      it.disable_recursion_pending();
    }
    std::string path = entry.path().string();
    const std::size_t nameOffset = path.size() - name.size();
    entries.push_back(
        Entry{
            .path = std::move(path),
            .nameLower = StringUtils::toLower(name),
            .nameOffset = nameOffset,
            .isDir = isDir,
        }
    );
    if (entries.size() >= kMaxEntries) {
      break;
    }
  }
  return entries;
}

std::vector<LauncherResult> FileProvider::search(
    const std::vector<Entry>& entries, std::string_view text, std::size_t limit, std::string_view home
) {
  const std::string needle = StringUtils::toLower(StringUtils::trimRightView(StringUtils::trimLeftView(text)));
  if (needle.size() < kMinQueryChars) {
    return {};
  }

  struct Scored {
    const Entry* entry;
    double score;
  };
  std::vector<Scored> scored;
  for (const Entry& entry : entries) {
    // Spotlight matches names, not scattered letters: require the query as a substring,
    // then let the fuzzy scorer rank prefixes and word starts above mid-word hits.
    const auto pos = entry.nameLower.find(needle);
    if (pos == std::string::npos) {
      continue;
    }
    double score = FuzzyMatch::score(needle, entry.nameLower);
    if (!FuzzyMatch::isMatch(score)) {
      continue;
    }
    if (pos == 0) {
      score += 1.0;
    }
    // Shallower paths are usually the ones people mean.
    const auto depth = static_cast<double>(std::ranges::count(std::string_view(entry.path), '/'));
    score -= depth * 0.05;
    scored.push_back({&entry, score});
  }
  const std::size_t keep = std::min(limit, scored.size());
  std::ranges::partial_sort(
      scored, scored.begin() + static_cast<std::ptrdiff_t>(keep), std::ranges::greater{}, &Scored::score
  );
  scored.resize(keep);

  std::vector<LauncherResult> results;
  results.reserve(scored.size());
  for (const Scored& item : scored) {
    const Entry& entry = *item.entry;
    const FileIcon icon = iconFor(entry.nameLower, entry.isDir);
    LauncherResult result;
    result.id = std::string(kResultPrefix) + entry.path;
    result.title = entry.path.substr(entry.nameOffset);
    result.subtitle = displayParent(entry.path, entry.nameOffset, home);
    result.iconName = std::string(icon.iconName);
    result.glyphName = std::string(icon.glyph);
    result.kind = i18n::tr(entry.isDir ? "launcher.kinds.folder" : "launcher.kinds.file");
    // Below applications, which weight their name matches five times over.
    result.score = item.score;
    results.push_back(std::move(result));
  }
  return results;
}

void FileProvider::ensureIndex() const {
  {
    std::scoped_lock lock(m_state->mutex);
    const bool fresh =
        m_state->index != nullptr && std::chrono::steady_clock::now() - m_state->indexedAt < kIndexMaxAge;
    if (fresh || m_state->scanning) {
      return;
    }
    m_state->scanning = true;
  }
  std::thread([state = m_state, root = m_root]() {
    auto entries = std::make_shared<const std::vector<Entry>>(scan(root));
    {
      std::scoped_lock lock(state->mutex);
      state->index = std::move(entries);
      state->indexedAt = std::chrono::steady_clock::now();
      state->scanning = false;
    }
    DeferredCall::callLater([state]() {
      if (state->alive && state->onChanged) {
        state->onChanged();
      }
    });
  }).detach();
}

std::vector<LauncherResult> FileProvider::run(std::string_view text, std::size_t limit) const {
  ensureIndex();
  std::shared_ptr<const std::vector<Entry>> index;
  {
    std::scoped_lock lock(m_state->mutex);
    index = m_state->index;
  }
  if (index == nullptr) {
    return {};
  }
  return search(*index, text, limit, homeDir());
}

std::vector<LauncherResult> FileProvider::query(std::string_view text) const { return run(text, kGlobalResults); }

std::vector<LauncherResult> FileProvider::queryPrefixed(std::string_view text) const {
  return run(text, kPrefixedResults);
}

std::string FileProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.open");
}

std::vector<LauncherAction> FileProvider::actions(const LauncherResult& result) const {
  if (!result.id.starts_with(kResultPrefix)) {
    return {};
  }
  const std::filesystem::path path = result.id.substr(kResultPrefix.size());
  std::error_code ec;
  const bool isDir = std::filesystem::is_directory(path, ec);
  std::vector<LauncherAction> actions;
  actions.push_back({.id = "show", .label = i18n::tr("launcher.actions.show-in-folder")});
  if (isDir) {
    actions.push_back({.id = "terminal", .label = i18n::tr("launcher.actions.open-in-terminal")});
  }
  if (m_clipboard != nullptr) {
    actions.push_back({.id = "copy-path", .label = i18n::tr("launcher.actions.copy-path")});
    actions.push_back({.id = "copy-file", .label = i18n::tr("launcher.actions.copy-file")});
  }
  if (process::commandExists("gio")) {
    actions.push_back({.id = "trash", .label = i18n::tr("launcher.actions.move-to-trash")});
  }
  return actions;
}

LauncherActionOutcome FileProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (!result.id.starts_with(kResultPrefix)) {
    return LauncherActionOutcome::Failed;
  }
  const std::filesystem::path path = result.id.substr(kResultPrefix.size());
  const auto outcome = [](bool ok) { return ok ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed; };
  if (actionId == "show") {
    return outcome(launcher_util::showInFolder(path));
  }
  if (actionId == "terminal") {
    const auto terminal = terminal_launch::prepareOpen();
    return outcome(terminal.has_value() && process::runAsync(*terminal, {}, path.string()));
  }
  if (actionId == "copy-path" && m_clipboard != nullptr) {
    return outcome(m_clipboard->copyText(path.string()));
  }
  if (actionId == "copy-file" && m_clipboard != nullptr) {
    // A file manager pastes a text/uri-list selection as the file itself.
    return outcome(m_clipboard->copyText(launcher_util::fileUri(path), "text/uri-list"));
  }
  if (actionId == "trash") {
    if (!process::runSync(std::vector<std::string>{"gio", "trash", path.string()})) {
      return LauncherActionOutcome::Failed;
    }
    // Drop it from the index so it leaves the results straight away.
    {
      std::scoped_lock lock(m_state->mutex);
      if (m_state->index != nullptr) {
        auto pruned = std::make_shared<std::vector<Entry>>(*m_state->index);
        std::erase_if(*pruned, [&path](const Entry& entry) {
          return entry.path == path.string() || entry.path.starts_with(path.string() + "/");
        });
        m_state->index = std::move(pruned);
      }
    }
    return LauncherActionOutcome::KeepOpen;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> FileProvider::preview(const LauncherResult& result) const {
  if (!result.id.starts_with(kResultPrefix)) {
    return std::nullopt;
  }
  const std::filesystem::path path = result.id.substr(kResultPrefix.size());
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec) {
    return std::nullopt;
  }
  const bool isDir = std::filesystem::is_directory(status);
  const std::string nameLower = StringUtils::toLower(path.filename().string());

  LauncherPreview preview;
  preview.title = path.filename().string();
  if (!isDir && isPreviewableImage(nameLower)) {
    preview.imagePath = path.string();
  } else if (!isDir && isTextFile(nameLower)) {
    std::ifstream file(path, std::ios::binary);
    std::string text(kPreviewTextBytes, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (!text.contains('\0')) {
      preview.body = std::move(text);
    }
  }

  preview.metadata.emplace_back(i18n::tr("launcher.preview.kind"), result.kind);
  if (isDir) {
    std::size_t items = 0;
    for (auto it = std::filesystem::directory_iterator(path, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      ++items;
    }
    preview.metadata.emplace_back(i18n::tr("launcher.preview.items"), std::to_string(items));
  } else {
    preview.metadata.emplace_back(
        i18n::tr("launcher.preview.size"), launcher_util::formatByteSize(std::filesystem::file_size(path, ec))
    );
  }
  const auto modified = std::filesystem::last_write_time(path, ec);
  if (!ec) {
    preview.metadata.emplace_back(i18n::tr("launcher.preview.modified"), formatFileTime(modified));
  }
  preview.metadata.emplace_back(i18n::tr("launcher.preview.where"), result.subtitle);
  return preview;
}

bool FileProvider::activate(const LauncherResult& result) {
  if (!result.id.starts_with(kResultPrefix)) {
    return false;
  }
  const std::string path = result.id.substr(kResultPrefix.size());
  return launcher_util::openUri(path);
}
