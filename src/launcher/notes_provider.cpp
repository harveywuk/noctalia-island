#include "launcher/notes_provider.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

namespace {

  constexpr std::string_view kAddId = "add";
  constexpr std::string_view kOpenId = "open";
  constexpr std::string_view kNotePrefix = "note:";
  constexpr std::size_t kMaxListed = 50;
  constexpr double kAddScore = 8500.0;

  [[nodiscard]] std::string captureText(std::string_view text, bool prefixed) {
    std::string trimmed = StringUtils::trim(text);
    if (prefixed) {
      return trimmed;
    }
    const std::string lower = StringUtils::toLower(trimmed);
    for (const std::string_view lead : {"note ", "note: ", "quick note "}) {
      if (lower.starts_with(lead)) {
        return StringUtils::trim(std::string_view(trimmed).substr(lead.size()));
      }
    }
    return {};
  }

  [[nodiscard]] std::string nowStamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buffer[32];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return std::string(buffer, written);
  }

} // namespace

NotesProvider::NotesProvider(ClipboardService* clipboard, ConfigService* config, std::filesystem::path file)
    : m_clipboard(clipboard), m_config(config), m_file(std::move(file)) {}

std::string NotesProvider::displayName() const { return i18n::tr("launcher.providers.notes.title"); }

std::filesystem::path NotesProvider::file() const {
  if (!m_file.empty()) {
    return m_file;
  }
  if (m_config != nullptr) {
    const std::string& configured = m_config->config().shell.launcher.notesFile;
    if (!configured.empty()) {
      return FileUtils::expandXdgBaseDir(configured);
    }
  }
  const char* homeEnv = std::getenv("HOME");
  const std::filesystem::path home(homeEnv != nullptr && homeEnv[0] != '\0' ? homeEnv : ".");
  std::error_code ec;
  if (std::filesystem::is_directory(home / "Documents", ec)) {
    return home / "Documents" / "Notes.md";
  }
  return home / "Notes.md";
}

bool NotesProvider::append(const std::filesystem::path& file, std::string_view text) {
  std::string line = StringUtils::trim(text);
  if (line.empty()) {
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::ofstream out(file, std::ios::app);
  if (!out.is_open()) {
    return false;
  }
  out << "- [" << nowStamp() << "] " << line << '\n';
  return static_cast<bool>(out);
}

std::vector<NotesProvider::Note> NotesProvider::read(const std::filesystem::path& file) {
  std::vector<Note> notes;
  std::ifstream in(file);
  if (!in.is_open()) {
    return notes;
  }
  std::string line;
  std::size_t index = 0;
  for (; std::getline(in, line); ++index) {
    const std::string trimmed = StringUtils::trim(line);
    if (!trimmed.starts_with("- ")) {
      continue;
    }
    Note note;
    note.line = index;
    std::string body = StringUtils::trim(std::string_view(trimmed).substr(2));
    if (body.starts_with('[')) {
      const auto close = body.find(']');
      if (close != std::string::npos) {
        note.stamp = body.substr(1, close - 1);
        body = StringUtils::trim(std::string_view(body).substr(close + 1));
      }
    }
    note.text = body;
    if (!note.text.empty()) {
      notes.push_back(std::move(note));
    }
  }
  std::ranges::reverse(notes);
  return notes;
}

bool NotesProvider::remove(const std::filesystem::path& file, std::size_t line) {
  std::ifstream in(file);
  if (!in.is_open()) {
    return false;
  }
  std::vector<std::string> lines;
  std::string current;
  while (std::getline(in, current)) {
    lines.push_back(current);
  }
  in.close();
  if (line >= lines.size()) {
    return false;
  }
  lines.erase(lines.begin() + static_cast<long>(line));
  std::ofstream out(file, std::ios::trunc);
  for (const auto& text : lines) {
    out << text << '\n';
  }
  return static_cast<bool>(out);
}

std::vector<LauncherResult> NotesProvider::noteResults(std::string_view filter) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(filter));
  std::vector<LauncherResult> results;
  std::size_t shown = 0;
  for (const Note& note : read(file())) {
    double score = static_cast<double>(kMaxListed) - static_cast<double>(shown);
    if (!needle.empty()) {
      const double match = FuzzyMatch::score(needle, StringUtils::toLower(note.text));
      if (!FuzzyMatch::isMatch(match)) {
        continue;
      }
      score = match;
    }
    LauncherResult result;
    result.id = std::string(kNotePrefix) + std::to_string(note.line);
    result.title = note.text;
    result.subtitle = note.stamp;
    result.glyphName = "note";
    result.kind = i18n::tr("launcher.kinds.note");
    result.score = score;
    results.push_back(std::move(result));
    if (++shown >= kMaxListed) {
      break;
    }
  }
  return results;
}

std::vector<LauncherResult> NotesProvider::query(std::string_view text) const {
  const std::string capture = captureText(text, false);
  if (capture.empty()) {
    return {};
  }
  LauncherResult add;
  add.id = std::string(kAddId);
  add.title = i18n::tr("launcher.notes.add", "text", capture);
  add.subtitle = file().filename().string();
  add.glyphName = "note";
  add.kind = i18n::tr("launcher.kinds.command");
  add.query = capture;
  add.score = kAddScore;
  return {std::move(add)};
}

std::vector<LauncherResult> NotesProvider::queryPrefixed(std::string_view text) const {
  const std::string capture = captureText(text, true);
  std::vector<LauncherResult> results = noteResults(capture);
  if (!capture.empty()) {
    LauncherResult add;
    add.id = std::string(kAddId);
    add.title = i18n::tr("launcher.notes.add", "text", capture);
    add.subtitle = file().filename().string();
    add.glyphName = "note";
    add.kind = i18n::tr("launcher.kinds.command");
    add.query = capture;
    add.score = kAddScore;
    results.insert(results.begin(), std::move(add));
  } else {
    LauncherResult open;
    open.id = std::string(kOpenId);
    open.title = i18n::tr("launcher.notes.open");
    open.subtitle = file().string();
    open.glyphName = "file-text";
    open.kind = i18n::tr("launcher.kinds.command");
    open.score = -1.0;
    results.push_back(std::move(open));
  }
  return results;
}

bool NotesProvider::activate(const LauncherResult& result) {
  if (result.id == kAddId) {
    return append(file(), result.query.value_or(std::string()));
  }
  if (result.id == kOpenId) {
    return launcher_util::openUri(launcher_util::fileUri(file()));
  }
  if (result.id.starts_with(kNotePrefix) && m_clipboard != nullptr) {
    return m_clipboard->copyText(result.title);
  }
  return false;
}

std::string NotesProvider::primaryActionLabel(const LauncherResult& result) const {
  if (result.id == kAddId) {
    return i18n::tr("launcher.actions.save-note");
  }
  if (result.id == kOpenId) {
    return i18n::tr("launcher.actions.open");
  }
  return i18n::tr("launcher.actions.copy-note");
}

std::vector<LauncherAction> NotesProvider::actions(const LauncherResult& result) const {
  if (!result.id.starts_with(kNotePrefix)) {
    return {};
  }
  return {
      {.id = "delete", .label = i18n::tr("launcher.actions.delete-note")},
      {.id = "open-file", .label = i18n::tr("launcher.notes.open")},
  };
}

LauncherActionOutcome NotesProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  if (!result.id.starts_with(kNotePrefix)) {
    return LauncherActionOutcome::Failed;
  }
  if (actionId == "delete") {
    const std::size_t line = static_cast<std::size_t>(std::stoul(result.id.substr(kNotePrefix.size())));
    return remove(file(), line) ? LauncherActionOutcome::KeepOpen : LauncherActionOutcome::Failed;
  }
  if (actionId == "open-file") {
    return launcher_util::openUri(launcher_util::fileUri(file())) ? LauncherActionOutcome::Done
                                                                  : LauncherActionOutcome::Failed;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> NotesProvider::preview(const LauncherResult& result) const {
  LauncherPreview preview;
  if (result.id.starts_with(kNotePrefix)) {
    preview.body = result.title;
    if (!result.subtitle.empty()) {
      preview.metadata.emplace_back(i18n::tr("launcher.preview.written"), result.subtitle);
    }
    preview.metadata.emplace_back(i18n::tr("launcher.preview.where"), file().string());
    return preview;
  }
  if (result.id == kAddId) {
    preview.title = i18n::tr("launcher.notes.preview-title");
    preview.body = result.query.value_or(std::string());
    preview.metadata.emplace_back(i18n::tr("launcher.preview.where"), file().string());
    return preview;
  }
  return std::nullopt;
}
