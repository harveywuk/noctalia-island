#include "launcher/system_provider.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "ipc/ipc_service.h"
#include "launcher/launcher_util.h"
#include "notification/notifications.h"
#include "system/disk_mounts.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <thread>

namespace {

  constexpr Logger kLog("launcher-system");
  // Long enough for the launcher's close animation to finish.
  constexpr auto kAfterCloseDelay = std::chrono::milliseconds(260);

  constexpr std::array<SystemProvider::Command, 17> kCommands = {{
      {.id = "toggle-dark-mode", .glyph = "moon", .ipc = "theme-mode-toggle"},
      {.id = "toggle-dnd", .glyph = "bell-off", .ipc = "notification-dnd-toggle"},
      {.id = "toggle-wifi", .glyph = "wifi", .ipc = "wifi-toggle"},
      {.id = "toggle-bluetooth", .glyph = "bluetooth", .ipc = "bluetooth-toggle"},
      {.id = "toggle-keep-awake", .glyph = "coffee", .ipc = "caffeine-toggle"},
      {.id = "toggle-night-light", .glyph = "sun-moon", .ipc = "nightlight-force-toggle"},
      {.id = "toggle-mute", .glyph = "volume-3", .ipc = "volume-mute"},
      {.id = "toggle-mic", .glyph = "microphone-off", .ipc = "mic-mute"},
      {.id = "screenshot", .glyph = "screenshot", .ipc = "screenshot-region", .afterClose = true},
      {.id = "annotate", .glyph = "pencil", .ipc = "screenshot-annotate", .afterClose = true},
      {.id = "record", .glyph = "video", .ipc = "record-region", .afterClose = true},
      {.id = "stop-recording", .glyph = "player-stop", .ipc = "record-stop"},
      {.id = "displays-off", .glyph = "device-desktop-off", .ipc = "dpms-off", .afterClose = true},
      {.id = "clear-clipboard", .glyph = "clipboard-x", .ipc = "clipboard-clear"},
      {.id = "clear-notifications", .glyph = "bell-x", .ipc = "notification-clear-history"},
      {.id = "empty-trash", .glyph = "trash", .ipc = {}},
      {.id = "eject-all", .glyph = "player-eject", .ipc = {}},
  }};

  [[nodiscard]] std::string titleFor(const SystemProvider::Command& command) {
    return i18n::tr(std::format("launcher.system.{}.title", command.id));
  }

  [[nodiscard]] std::string keywordsFor(const SystemProvider::Command& command) {
    return i18n::tr(std::format("launcher.system.{}.keywords", command.id));
  }

  void notifyLater(std::string title) {
    DeferredCall::callLater([title = std::move(title)]() { notify::info("Noctalia", title, {}); });
  }

  // Runs on a worker thread: emptying a large trash or powering off a drive can take seconds.
  void emptyTrash() {
    if (process::commandExists("gio")) {
      if (process::runSync(std::vector<std::string>{"gio", "trash", "--empty"})) {
        notifyLater(i18n::tr("launcher.system.empty-trash.done"));
        return;
      }
    }
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path trash;
    if (dataHome != nullptr && *dataHome != '\0') {
      trash = std::filesystem::path(dataHome) / "Trash";
    } else if (home != nullptr) {
      trash = std::filesystem::path(home) / ".local/share/Trash";
    } else {
      return;
    }
    for (const char* sub : {"files", "info"}) {
      std::error_code ec;
      for (const auto& item : std::filesystem::directory_iterator(trash / sub, ec)) {
        std::filesystem::remove_all(item.path(), ec);
      }
    }
    notifyLater(i18n::tr("launcher.system.empty-trash.done"));
  }

  void ejectAll() {
    std::size_t ejected = 0;
    for (const auto& mount : physicalDiskMounts()) {
      // Removable media mounts under /run/media (udisks) or /media.
      if (!mount.path.starts_with("/run/media/") && !mount.path.starts_with("/media/")) {
        continue;
      }
      if (process::runSync(
              std::vector<std::string>{"udisksctl", "unmount", "--no-user-interaction", "-b", mount.source}
          )) {
        (void)process::runSync(
            std::vector<std::string>{"udisksctl", "power-off", "--no-user-interaction", "-b", mount.source}
        );
        ++ejected;
      } else {
        kLog.warn("could not unmount {}", mount.source);
      }
    }
    notifyLater(
        ejected == 0 ? i18n::tr("launcher.system.eject-all.none")
                     : i18n::trp("launcher.system.eject-all.done", static_cast<long>(ejected))
    );
  }

} // namespace

std::span<const SystemProvider::Command> SystemProvider::commands() { return kCommands; }

std::string SystemProvider::displayName() const { return i18n::tr("launcher.providers.system.title"); }

std::vector<LauncherResult> SystemProvider::search(std::string_view text, bool listAll) const {
  const std::string needle = StringUtils::toLower(StringUtils::trim(text));
  if (needle.empty() && !listAll) {
    return {};
  }
  std::vector<LauncherResult> results;
  for (const auto& command : kCommands) {
    const std::string title = titleFor(command);
    double score = 0.0;
    if (!needle.empty()) {
      const std::string lowerTitle = StringUtils::toLower(title);
      const std::string lowerKeywords = StringUtils::toLower(keywordsFor(command));
      if (!listAll && !launcher_util::wordsMatch(needle, lowerTitle + " " + lowerKeywords)) {
        continue;
      }
      score = FuzzyMatch::score(needle, lowerTitle);
      const double keywordScore = FuzzyMatch::score(needle, lowerKeywords) - 0.5;
      score = std::max(score, keywordScore);
      if (!FuzzyMatch::isMatch(score)) {
        continue;
      }
    }
    LauncherResult result;
    result.id = std::string(command.id);
    result.title = title;
    result.glyphName = std::string(command.glyph);
    result.kind = i18n::tr("launcher.kinds.command");
    result.score = score;
    results.push_back(std::move(result));
  }
  return results;
}

std::vector<LauncherResult> SystemProvider::query(std::string_view text) const { return search(text, false); }

std::vector<LauncherResult> SystemProvider::queryPrefixed(std::string_view text) const { return search(text, true); }

void SystemProvider::run(const Command& command) {
  if (command.id == "empty-trash") {
    std::thread(emptyTrash).detach();
    return;
  }
  if (command.id == "eject-all") {
    std::thread(ejectAll).detach();
    return;
  }
  if (m_ipc == nullptr) {
    return;
  }
  const std::string reply = m_ipc->execute(std::string(command.ipc));
  if (reply.starts_with("error")) {
    kLog.warn("{}: {}", command.ipc, StringUtils::trim(reply));
  }
}

bool SystemProvider::activate(const LauncherResult& result) {
  for (const auto& command : kCommands) {
    if (command.id != result.id) {
      continue;
    }
    if (command.afterClose) {
      m_afterCloseTimer.start(kAfterCloseDelay, [this, &command]() { run(command); });
    } else {
      run(command);
    }
    return true;
  }
  return false;
}

std::string SystemProvider::primaryActionLabel(const LauncherResult& /*result*/) const {
  return i18n::tr("launcher.actions.run-command");
}
