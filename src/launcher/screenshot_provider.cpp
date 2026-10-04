#include "launcher/screenshot_provider.h"

#include "config/config_service.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "launcher/launcher_util.h"
#include "util/file_utils.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"
#include "wayland/clipboard_service.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>

namespace {

  constexpr std::size_t kMaxResults = 100;

  [[nodiscard]] bool isImage(const std::filesystem::path& path) {
    const std::string ext = StringUtils::toLower(path.extension().string());
    return ext == ".png"
        || ext == ".jpg"
        || ext == ".jpeg"
        || ext == ".webp"
        || ext == ".gif"
        || ext == ".avif"
        || ext == ".jxl";
  }

  [[nodiscard]] std::string formatFileTime(std::filesystem::file_time_type modified) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(modified);
    const std::time_t seconds = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[32];
    const std::size_t written = std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return std::string(buffer, written);
  }

  [[nodiscard]] std::string relativeTime(std::filesystem::file_time_type modified) {
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(modified);
    const auto age = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now() - system);
    if (age < std::chrono::minutes(1)) {
      return i18n::tr("launcher.screenshots.just-now");
    }
    if (age < std::chrono::hours(1)) {
      return i18n::trp("launcher.screenshots.minutes-ago", age.count());
    }
    if (age < std::chrono::hours(24)) {
      return i18n::trp("launcher.screenshots.hours-ago", std::chrono::duration_cast<std::chrono::hours>(age).count());
    }
    return formatFileTime(modified);
  }

} // namespace

ScreenshotProvider::ScreenshotProvider(ClipboardService* clipboard, ConfigService* config)
    : m_clipboard(clipboard), m_config(config) {}

std::string ScreenshotProvider::displayName() const { return i18n::tr("launcher.providers.screenshots.title"); }

std::filesystem::path ScreenshotProvider::directory() const {
  if (m_config != nullptr) {
    const std::string& configured = m_config->config().shell.screenshot.directory;
    if (!configured.empty()) {
      return FileUtils::expandXdgBaseDir(configured);
    }
  }
  return FileUtils::defaultPicturesDirectory();
}

std::vector<ScreenshotProvider::Shot> ScreenshotProvider::scan(const std::filesystem::path& directory) {
  std::vector<Shot> shots;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    std::error_code entryEc;
    if (!entry.is_regular_file(entryEc) || !isImage(entry.path())) {
      continue;
    }
    Shot shot;
    shot.path = entry.path();
    shot.modified = entry.last_write_time(entryEc);
    shot.size = entry.file_size(entryEc);
    shots.push_back(std::move(shot));
  }
  std::ranges::sort(shots, [](const Shot& a, const Shot& b) { return a.modified > b.modified; });
  return shots;
}

std::vector<LauncherResult> ScreenshotProvider::query(std::string_view text) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  std::vector<LauncherResult> results;
  std::size_t index = 0;
  for (const Shot& shot : scan(directory())) {
    const std::string name = shot.path.filename().string();
    double score = static_cast<double>(kMaxResults) - static_cast<double>(index);
    if (!needle.empty()) {
      const double match = FuzzyMatch::score(needle, StringUtils::toLower(name));
      if (!FuzzyMatch::isMatch(match)) {
        continue;
      }
      score = match;
    }
    LauncherResult result;
    result.id = shot.path.string();
    result.title = name;
    result.subtitle = relativeTime(shot.modified);
    result.glyphName = "photo";
    result.kind = i18n::tr("launcher.kinds.screenshot");
    result.score = score;
    results.push_back(std::move(result));
    if (++index >= kMaxResults) {
      break;
    }
  }
  return results;
}

bool ScreenshotProvider::activate(const LauncherResult& result) {
  return launcher_util::openUri(launcher_util::fileUri(result.id));
}

std::string ScreenshotProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.open");
}

std::vector<LauncherAction> ScreenshotProvider::actions(const LauncherResult& /*result*/) const {
  return {
      {.id = "copy-image", .label = i18n::tr("launcher.actions.copy-image")},
      {.id = "show", .label = i18n::tr("launcher.actions.show-in-folder")},
      {.id = "copy-path", .label = i18n::tr("launcher.actions.copy-path")},
      {.id = "trash", .label = i18n::tr("launcher.actions.move-to-trash")},
  };
}

LauncherActionOutcome ScreenshotProvider::runAction(const LauncherResult& result, std::string_view actionId) {
  const std::filesystem::path path(result.id);
  const auto outcome = [](bool ok) { return ok ? LauncherActionOutcome::Done : LauncherActionOutcome::Failed; };
  if (actionId == "copy-image" && m_clipboard != nullptr) {
    if (StringUtils::toLower(path.extension().string()) == ".png") {
      std::ifstream file(path, std::ios::binary);
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      if (!bytes.empty()) {
        return outcome(m_clipboard->copyImagePng(std::move(bytes)));
      }
    }
    return outcome(m_clipboard->copyText(launcher_util::fileUri(path), "text/uri-list"));
  }
  if (actionId == "show") {
    return outcome(launcher_util::showInFolder(path));
  }
  if (actionId == "copy-path" && m_clipboard != nullptr) {
    return outcome(m_clipboard->copyText(path.string()));
  }
  if (actionId == "trash") {
    if (!process::runSync(std::vector<std::string>{"gio", "trash", path.string()})) {
      return LauncherActionOutcome::Failed;
    }
    return LauncherActionOutcome::KeepOpen;
  }
  return LauncherActionOutcome::Failed;
}

std::optional<LauncherPreview> ScreenshotProvider::preview(const LauncherResult& result) const {
  const std::filesystem::path path(result.id);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return std::nullopt;
  }
  LauncherPreview preview;
  preview.imagePath = path.string();
  preview.metadata.emplace_back(i18n::tr("launcher.preview.name"), path.filename().string());
  preview.metadata.emplace_back(
      i18n::tr("launcher.preview.size"), launcher_util::formatByteSize(std::filesystem::file_size(path, ec))
  );
  preview.metadata.emplace_back(
      i18n::tr("launcher.preview.modified"), formatFileTime(std::filesystem::last_write_time(path, ec))
  );
  preview.metadata.emplace_back(i18n::tr("launcher.preview.where"), path.parent_path().string());
  return preview;
}
