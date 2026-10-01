#include "system/startup_apps.h"

#include "core/log.h"
#include "core/process/process.h"
#include "system/desktop_entry.h"
#include "system/desktop_entry_launch.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
  const Logger kLog("StartupApps");

  std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  const DesktopEntry* findApp(const std::string& id) {
    const auto& entries = desktopEntries();
    const auto it = std::ranges::find(entries, id, &DesktopEntry::id);
    return it == entries.end() || it->hidden || it->noDisplay ? nullptr : &*it;
  }

  bool launchApp(const StartupAppConfig& entry) {
    if (entry.kind == "command") {
      // Apps get their own unit when the shell is itself a user service, so
      // restarting Noctalia does not terminate them with the shell's cgroup.
      return process::runAsyncAsSystemdService({"/bin/sh", "-lc", entry.command}, "startup-" + entry.id);
    }
    const auto* app = findApp(entry.desktopId);
    if (!app)
      return false;
    return desktop_entry_launch::launchEntry(
        *app,
        {.runAsSystemdService = process::runningUnderSystemdUserManager(),
         .dbusActivatable = app->dbusActivatable,
         .dbusAppId = app->id}
    );
  }

  StartupAppConfig readEntry(const nlohmann::json& row) {
    StartupAppConfig entry;
    entry.id = row.at("id").get<std::string>();
    entry.kind = row.at("kind").get<std::string>();
    entry.desktopId = row.at("desktop_id").get<std::string>();
    entry.command = row.at("command").get<std::string>();
    return entry;
  }
} // namespace

StartupApps::StartupApps(Launch launch, Clock clock)
    : m_launch(launch ? std::move(launch) : launchApp), m_clock(clock ? std::move(clock) : nowMs) {}

StartupApps::~StartupApps() {
  m_timer.stop();
  if (m_lock >= 0)
    ::close(m_lock);
}

std::filesystem::path StartupApps::sessionStateFile() {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  const char* display = std::getenv("WAYLAND_DISPLAY");
  if (!runtime || !*runtime || !display || !*display)
    return {};
  struct stat directory{}, socket{};
  const auto socketPath = std::filesystem::path(runtime) / display;
  if (::stat(runtime, &directory)
      || directory.st_uid != ::getuid()
      || !S_ISDIR(directory.st_mode)
      || (directory.st_mode & 0077)
      || ::stat(socketPath.c_str(), &socket)
      || !S_ISSOCK(socket.st_mode)
      || socket.st_uid != ::getuid())
    return {};
  return std::filesystem::path(runtime)
      / std::format(
             "noctalia-startup-{}-{}-{}-{}.json", socket.st_dev, socket.st_ino, socket.st_ctim.tv_sec,
             socket.st_ctim.tv_nsec
      );
}

std::string StartupApps::problem(const StartupAppConfig& entry) {
  if (entry.kind == "app")
    return findApp(entry.desktopId) ? "" : "missing-app";
  if (entry.kind != "command")
    return "invalid-kind";
  if (entry.command.size() > 4096
      || entry.command.find('\0') != std::string::npos
      || entry.command.find_first_not_of(" \t\r\n") == std::string::npos)
    return "invalid-command";
  return {};
}

bool StartupApps::save() {
  const auto temporary = m_stateFile.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return false;
  const auto data = m_plan.dump();
  std::size_t written = 0;
  while (written < data.size()) {
    const auto n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    written += static_cast<std::size_t>(n);
  }
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  if (written == data.size() && synced && ::rename(temporary.c_str(), m_stateFile.c_str()) == 0)
    return true;
  ::unlink(temporary.c_str());
  return false;
}

bool StartupApps::start(const std::vector<StartupAppConfig>& entries, std::filesystem::path stateFile) {
  if (m_lock >= 0)
    return false;
  m_stateFile = stateFile.empty() ? sessionStateFile() : std::move(stateFile);
  if (m_stateFile.empty()) {
    kLog.warn("Cannot identify the Wayland session; startup apps skipped");
    return false;
  }
  m_lock = ::open((m_stateFile.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (m_lock < 0 || ::flock(m_lock, LOCK_EX | LOCK_NB) != 0) {
    if (m_lock >= 0)
      ::close(m_lock);
    m_lock = -1;
    kLog.warn("Startup apps are already managed or session state is unavailable");
    return false;
  }
  try {
    if (std::filesystem::exists(m_stateFile)) {
      std::ifstream input(m_stateFile);
      input >> m_plan;
      if (m_plan.at("version") != 1 || !m_plan.at("entries").is_array())
        throw std::runtime_error("invalid launch plan");
      for (const auto& row : m_plan.at("entries")) {
        (void)readEntry(row);
        (void)row.at("attempted").get<bool>();
        (void)row.at("due").get<std::int64_t>();
      }
    } else {
      m_plan = {{"version", 1}, {"entries", nlohmann::json::array()}};
      const auto now = m_clock();
      for (const auto& entry : entries) {
        if (!entry.enabled)
          continue;
        const auto issue = problem(entry);
        if (!issue.empty()) {
          kLog.warn("Skipping startup entry '{}': {}", entry.id, issue);
          continue;
        }
        m_plan["entries"].push_back(
            {{"id", entry.id},
             {"kind", entry.kind},
             {"desktop_id", entry.desktopId},
             {"command", entry.command},
             {"due", now + std::clamp(entry.delaySeconds, 0, 300) * 1000LL},
             {"attempted", false}}
        );
      }
      // Even an empty plan seals the session: editing preferences then restarting
      // the shell must not unexpectedly launch newly enabled applications.
      if (!save())
        throw std::runtime_error("could not save launch plan");
    }
    m_timer.start(std::chrono::milliseconds(0), [this] { dispatchDue(); });
    return true;
  } catch (const std::exception& e) {
    kLog.warn("Startup apps skipped: {}", e.what());
    return false;
  }
}

void StartupApps::dispatchDue() {
  const auto now = m_clock();
  std::int64_t next = std::numeric_limits<std::int64_t>::max();
  for (auto& row : m_plan["entries"]) {
    if (row["attempted"].get<bool>())
      continue;
    const auto due = row["due"].get<std::int64_t>();
    if (due > now) {
      next = std::min(next, due);
      continue;
    }
    // At most one attempt, including failures. Persist first: a crash between
    // this claim and exec can skip one launch but can never duplicate a launch.
    row["attempted"] = true;
    if (!save()) {
      kLog.warn("Cannot persist startup claim; remaining apps skipped");
      return;
    }
    const auto entry = readEntry(row);
    if (!problem(entry).empty() || !m_launch(entry))
      kLog.warn("Could not launch startup entry '{}'", entry.id);
    else
      kLog.info("Launched startup entry '{}'", entry.id);
  }
  if (next != std::numeric_limits<std::int64_t>::max())
    m_timer.start(std::chrono::milliseconds(std::max<std::int64_t>(1, next - m_clock())), [this] { dispatchDue(); });
}
