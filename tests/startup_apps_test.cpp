#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "system/startup_apps.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <print>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("noctalia-startup-test-" + std::to_string(::getpid()));
  std::filesystem::create_directory(root);
  ::chmod(root.c_str(), 0700);
  const auto state = root / "session.json";
  std::int64_t now = 10000;
  std::vector<std::string> launches;
  auto clock = [&] { return now; };
  auto launch = [&](const StartupAppConfig& entry) {
    launches.push_back(entry.id);
    return entry.id != "failed";
  };
  StartupAppConfig immediate{.id = "immediate", .enabled = true, .kind = "command", .command = "true"};
  auto delayed = immediate;
  delayed.id = "delayed";
  delayed.delaySeconds = 30;
  auto disabled = immediate;
  disabled.id = "disabled";
  disabled.enabled = false;
  auto invalid = immediate;
  invalid.id = "invalid";
  invalid.command = " \n\t";
  auto failed = immediate;
  failed.id = "failed";
  {
    StartupApps manager(launch, clock);
    assert(manager.start({immediate, delayed, disabled, invalid, failed}, state));
    StartupApps duplicate(launch, clock);
    assert(!duplicate.start({immediate}, state));
    TimerManager::instance().tick();
    assert((launches == std::vector<std::string>{"immediate", "failed"}));
    struct stat st{};
    assert(::stat(state.c_str(), &st) == 0);
    assert((st.st_mode & 0077) == 0);
  }
  now += 15000;
  {
    StartupApps restarted(launch, clock);
    assert(restarted.start({}, state)); // edits do not replace this session's captured plan
    TimerManager::instance().tick();
    assert(launches.size() == 2);
  }
  now += 16000;
  {
    StartupApps restarted(launch, clock);
    assert(restarted.start({immediate}, state));
    TimerManager::instance().tick();
    assert((launches == std::vector<std::string>{"immediate", "failed", "delayed"}));
  }
  {
    StartupApps restarted(launch, clock);
    assert(restarted.start({immediate, delayed, failed}, state));
    TimerManager::instance().tick();
    assert(launches.size() == 3);
    assert(TimerManager::instance().pollTimeoutMs() == -1); // no idle timer or polling
  }
  {
    StartupApps empty(launch, clock);
    assert(empty.start({}, root / "empty.json"));
    TimerManager::instance().tick();
  }
  {
    StartupApps edited(launch, clock);
    assert(edited.start({immediate}, root / "empty.json"));
    TimerManager::instance().tick();
    assert(launches.size() == 3);
  }
  {
    StartupApps newSession(launch, clock);
    assert(newSession.start({immediate}, root / "next-login.json"));
    TimerManager::instance().tick();
    assert(launches.size() == 4);
  }
  {
    std::ofstream(root / "broken.json") << "{";
    StartupApps broken(launch, clock);
    assert(!broken.start({immediate}, root / "broken.json"));
    TimerManager::instance().tick();
    assert(launches.size() == 4);
    StartupApps unwritable(launch, clock);
    assert(!unwritable.start({immediate}, root / "missing" / "file"));
  }
  assert(StartupApps::problem(invalid) == "invalid-command");
  invalid.command = std::string(4097, 'x');
  assert(StartupApps::problem(invalid) == "invalid-command");
  invalid.command = std::string("a\0b", 3);
  assert(StartupApps::problem(invalid) == "invalid-command");
  invalid.kind = "bad";
  assert(StartupApps::problem(invalid) == "invalid-kind");
  invalid.kind = "app";
  invalid.desktopId = "noctalia-nonexistent-test-app";
  assert(StartupApps::problem(invalid) == "missing-app");

  // A distinct compositor socket gets a distinct plan, even at the same display name.
  ::setenv("XDG_RUNTIME_DIR", root.c_str(), 1);
  ::setenv("WAYLAND_DISPLAY", "wayland-test", 1);
  assert(StartupApps::sessionStateFile().empty());
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto socketPath = (root / "wayland-test").string();
  std::copy(socketPath.begin(), socketPath.end(), address.sun_path);
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(fd >= 0);
  assert(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  const auto first = StartupApps::sessionStateFile();
  assert(!first.empty());
  assert(StartupApps::sessionStateFile() == first);
  ::close(fd);
  ::unlink(socketPath.c_str());
  fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(fd >= 0);
  assert(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  assert(StartupApps::sessionStateFile() != first);
  ::close(fd);
  ::chmod(root.c_str(), 0755);
  assert(StartupApps::sessionStateFile().empty());

  ShellConfig config;
  config.session.startupApps = {immediate, delayed, disabled};
  using namespace noctalia::config::schema;
  std::ranges::sort(config.session.startupApps, {}, &StartupAppConfig::id);
  const auto table = writeTable(config, shellSchema());
  ShellConfig restored;
  Diagnostics diagnostics;
  readInto(table, restored, shellSchema(), "shell", diagnostics);
  assert(restored.session.startupApps == config.session.startupApps);
  readInto(
      toml::parse("[session.startup_apps.clamped]\ndelay_seconds=9999"), restored, shellSchema(), "shell", diagnostics
  );
  const auto clamped = std::ranges::find(restored.session.startupApps, "clamped", &StartupAppConfig::id);
  assert(clamped != restored.session.startupApps.end() && clamped->delaySeconds == 300);
  std::filesystem::remove_all(root);
  std::println(
      "PASS: login isolation, restart recovery, delays, disabled/invalid entries, failures, atomic claims, schema and "
      "no idle polling"
  );
}
