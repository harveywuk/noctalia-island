#include "system/steam_activity.h"

#include <cassert>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/prctl.h>
#include <unistd.h>

int main() {
  namespace fs = std::filesystem;
  char path[] = "/tmp/noctalia-steam-test-XXXXXX";
  assert(mkdtemp(path));
  const fs::path home(path), root = home / ".steam/steam", log = root / "logs/content_log.txt";
  fs::create_directories(root / "logs");
  fs::create_directories(root / "steamapps");
  std::ofstream(home / ".steam/steam.pid") << getpid();
  // Only this isolated test process is named Steam; no real client is touched.
  assert(prctl(PR_SET_NAME, "steam", 0, 0, 0) == 0);
  std::ofstream(root / "steamapps/appmanifest_42.acf") << "\"AppState\" { \"name\" \"Test game\" }";
  const auto external = home / "External Library";
  fs::create_directories(external / "steamapps");
  std::ofstream(root / "steamapps/libraryfolders.vdf") << "\"libraryfolders\" { \"1\" { \"path\" \"" << external.string() << "\" } }";
  const auto write = [&](const std::string& state, bool append = true, bool old = false) {
    const auto now = std::time(nullptr) - (old ? 3600 : 0);
    std::tm tm{}; localtime_r(&now, &tm);
    char timestamp[32]; std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", &tm);
    std::ofstream file(log, append ? std::ios::app : std::ios::trunc);
    file << timestamp << " AppID 42 App update changed : " << state << "\n";
  };
  SteamActivity activity(home);
  write("Running Update,Downloading,", false, true);
  assert(activity.read().empty()); // Never resurrect another client session.
  write("Running Update,Downloading,Staging,");
  auto state = activity.read();
  assert(state.size() == 1 && state[0].phase == "downloading" && state[0].name == "Steam · Test game");
  assert(activity.read() == state); // An idle log doesn't duplicate the entry.
  fs::rename(root / "steamapps/appmanifest_42.acf", external / "steamapps/appmanifest_42.acf");
  assert(activity.read() == state); // Resolve names from external libraries too.
  SteamActivity restarted(home);
  assert(restarted.read() == state); // Shell restart during a transfer.
  write("Running Update,Staging,");
  assert(activity.read()[0].phase == "installing");
  write("Running Update,Stopping,");
  assert(activity.read().empty());
  write("Running Update,Preallocating,", false); // Truncation/rotation.
  assert(activity.read()[0].phase == "preparing");
  write("Running Update,Validating,");
  assert(activity.read()[0].phase == "verifying");
  write("None");
  assert(activity.read().empty());
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  std::ofstream(home / ".steam/steam.pid", std::ios::trunc) << "2147483647";
  assert(activity.read().empty()); // A crashed/exited client leaves no activity.
  fs::remove_all(home);
}
