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
  std::ofstream(root / "steamapps/libraryfolders.vdf")
      << "\"libraryfolders\" { \"1\" { \"path\" \""
      << external.string()
      << "\" } }";
  const auto write = [&](const std::string& state, bool append = true, bool old = false, int appId = 42) {
    const auto now = std::time(nullptr) - (old ? 3600 : 0);
    std::tm tm{};
    localtime_r(&now, &tm);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "[%Y-%m-%d %H:%M:%S]", &tm);
    std::ofstream file(log, append ? std::ios::app : std::ios::trunc);
    const bool event = state.starts_with("update ")
        || state.starts_with("scheduler finished : ")
        || state.starts_with("Shader update changed : ");
    file << timestamp << " AppID " << appId << " " << (event ? "" : "App update changed : ") << state << "\n";
  };
  SteamActivity activity(home);
  write("Running Update,Downloading,", false, true);
  assert(activity.read().empty()); // Never resurrect another client session.
  write("Running Update,Downloading,Staging,");
  auto state = activity.read();
  assert(state.size() == 1 && state[0].phase == "downloading" && state[0].name == "Steam · Test game");
  assert(activity.read() == state); // An idle log doesn't duplicate the entry.
  assert(!activity.takeFailure());  // Slow/stalled progress is not a reported error.
  fs::rename(root / "steamapps/appmanifest_42.acf", external / "steamapps/appmanifest_42.acf");
  assert(activity.read() == state); // Resolve names from external libraries too.
  SteamActivity restarted(home);
  assert(restarted.read() == state); // Shell restart during a transfer.
  write("Running Update,Staging,");
  assert(activity.read()[0].phase == "installing");
  write("Running Update,Stopping,");
  assert(activity.read().empty());
  assert(!activity.takeCompletion());
  assert(!activity.takeFailure());
  write("Running Update,Preallocating,", false); // Truncation/rotation.
  assert(activity.read()[0].phase == "preparing");
  write("Running Update,Validating,");
  assert(activity.read()[0].phase == "verifying");
  write("None");
  assert(activity.read().empty());
  assert(!activity.takeCompletion());
  // Steam can report None before its explicit finished event. Only that event
  // confirms success; replaying it after shell restart must not announce again.
  write("update finished : No Error");
  assert(activity.read().empty());
  auto completion = activity.takeCompletion();
  assert(completion && completion->appId == "42" && completion->name == "Test game");
  assert(!activity.takeCompletion());
  write("update finished : No Error");
  assert(activity.read().empty() && !activity.takeCompletion());
  SteamActivity afterFinish(home);
  assert(afterFinish.read().empty() && !afterFinish.takeCompletion());
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update canceled : User canceled");
  assert(activity.read().empty() && !activity.takeCompletion());
  assert(!activity.takeFailure());
  write("update finished : No Error");
  assert(activity.read().empty() && !activity.takeCompletion());
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update finished : Disk write failure \t\r");
  assert(activity.read().empty() && !activity.takeCompletion());
  auto failure = activity.takeFailure();
  assert(failure && failure->appId == "42" && failure->name == "Test game" && failure->reason == "Disk write failure");
  assert(!activity.takeFailure());
  write("update finished : Disk write failure");
  assert(activity.read().empty() && !activity.takeFailure()); // Duplicate errors do not prolong the notice.
  SteamActivity afterFailure(home);
  assert(afterFailure.read().empty() && !afterFailure.takeFailure());

  // Steam follows its explicit suspension with Stopping/None. Preserve the
  // paused row through those messages, but allow explicit resumption.
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update canceled : Priority (Suspended) \t\r"); // Steam's real log uses CRLF.
  write("Running Update,Stopping,");
  write("None");
  state = activity.read();
  assert(state.size() == 1 && state[0].phase == "paused");
  assert(!activity.takeCompletion() && !activity.takeFailure());
  assert(activity.read() == state);
  SteamActivity afterPause(home);
  assert(afterPause.read() == state); // Reconstruct a pause without announcing old events.
  assert(!afterPause.takeCompletion() && !afterPause.takeFailure());
  write("Running Update,Downloading,");
  assert(activity.read()[0].phase == "downloading");
  write("update canceled : Failed updating depot 42 while starting download (No connection to content servers)\r");
  write("None");
  assert(activity.read().empty());
  failure = activity.takeFailure();
  assert(failure && failure->appId == "42" && failure->name == "Test game");
  assert(failure->reason == "No connection to content servers");
  assert(!activity.takeCompletion() && !activity.takeFailure());

  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update canceled : Unfamiliar reason");
  assert(activity.read().empty() && !activity.takeFailure() && !activity.takeCompletion());
  write("update canceled : Disabled (Suspended)");
  assert(activity.read().empty()); // No phantom pause without observed work.
  write("update canceled : Failed updating depot 42 (No connection to content servers)");
  assert(activity.read().empty() && !activity.takeFailure());

  // Log rotation replays state but never old failure notices.
  write("Running Update,Downloading,", false);
  write("update finished : Disk read failure");
  assert(activity.read().empty() && !activity.takeFailure());
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update canceled : Shader Priority (Suspended)");
  assert(activity.read()[0].phase == "paused");

  // Current Steam logs use scheduler completion rather than update finished.
  // None ends the visible phase, but only the explicit success announces it.
  write("Running Update,Downloading,");
  assert(activity.read()[0].phase == "downloading");
  write("None");
  write("scheduler finished : removed from schedule (result No Error, state 0xc) \r");
  assert(activity.read().empty() && !activity.takeFailure());
  completion = activity.takeCompletion();
  assert(completion && completion->appId == "42" && completion->name == "Test game");
  write("scheduler finished : removed from schedule (result No Error, state 0xc)");
  assert(activity.read().empty() && !activity.takeCompletion());
  SteamActivity afterSchedulerFinish(home);
  assert(afterSchedulerFinish.read().empty() && !afterSchedulerFinish.takeCompletion());

  // Shader cache downloads use the same scheduler, with a different phase marker.
  write("Shader update changed : Running Update,Preallocating,");
  assert(activity.read()[0].phase == "preparing");
  write("Shader update changed : Running Update,Downloading,Staging,");
  assert(activity.read()[0].phase == "downloading");
  write("update canceled : Shader Priority (Suspended)");
  write("Shader update changed : None");
  write("scheduler finished : staying in schedule (result Suspended, state 0x40e) ");
  assert(activity.read()[0].phase == "paused");
  assert(!activity.takeCompletion() && !activity.takeFailure());
  SteamActivity afterShaderPause(home);
  assert(afterShaderPause.read()[0].phase == "paused" && !afterShaderPause.takeCompletion());
  write("Shader update changed : Running Update,Committing,");
  assert(activity.read()[0].phase == "installing");
  write("Shader update changed : None");
  write("scheduler finished : removed from schedule (result No Error, state 0xc)\t");
  assert(activity.read().empty() && activity.takeCompletion() && !activity.takeFailure());

  // A scheduler event without observed work, or after cancellation, stays quiet.
  write("scheduler finished : removed from schedule (result No Error, state 0xc)");
  assert(activity.read().empty() && !activity.takeCompletion());
  write("Shader update changed : Running Update,Downloading,");
  assert(!activity.read().empty());
  write("update canceled : User canceled");
  write("scheduler finished : removed from schedule (result No Error, state 0xc)");
  assert(activity.read().empty() && !activity.takeCompletion() && !activity.takeFailure());

  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("scheduler finished : staying in schedule (result No Error, state 0xc)");
  assert(!activity.read().empty() && !activity.takeCompletion());
  write("None");
  write("scheduler finished : removed from schedule (result Disk write failure, state 0x6)");
  assert(activity.read().empty() && !activity.takeCompletion());
  failure = activity.takeFailure();
  assert(failure && failure->appId == "42" && failure->name == "Test game" && failure->reason == "Disk write failure");
  SteamActivity afterSchedulerFailure(home);
  assert(afterSchedulerFailure.read().empty() && !afterSchedulerFailure.takeFailure());
  write("Running Update,Downloading,");
  assert(!activity.read().empty());
  write("scheduler finished : removed from schedule (result Unfamiliar reason, state 0x6)");
  assert(activity.read().empty() && !activity.takeFailure() && !activity.takeCompletion());

  // Resolve even when the whole transfer fits between two polls, and preserve
  // the title as metadata rather than stripping a display prefix from it.
  std::ofstream(external / "steamapps/appmanifest_42.acf") << R"("AppState" { "name" "Steam · Café \"Deluxe\"" })";
  write("Running Update,Downloading,");
  write("None");
  write("scheduler finished : removed from schedule (result No Error, state 0xc)");
  assert(activity.read().empty());
  completion = activity.takeCompletion();
  assert(completion && completion->appId == "42" && completion->name == "Steam · Café \"Deluxe\"");

  // With several completions in one poll, the last log event wins, even if a
  // different game's transfer remains active. Repeated success is ignored.
  std::ofstream(root / "steamapps/appmanifest_43.acf") << R"("AppState" { "name" "Second game" })";
  write("Running Update,Downloading,");
  write("Running Update,Downloading,", true, false, 43);
  write("Running Update,Downloading,", true, false, 44);
  write("update finished : No Error", true, false, 43);
  write("update finished : No Error");
  write("update finished : No Error", true, false, 43);
  state = activity.read();
  assert(state.size() == 1 && state[0].appId == "44");
  completion = activity.takeCompletion();
  assert(completion && completion->appId == "42" && completion->name == "Steam · Café \"Deluxe\"");
  assert(!activity.takeCompletion());

  // A missing or empty manifest remains a real completion with no invented
  // game title. It must not inherit the previous game's name.
  write("update finished : No Error", true, false, 44);
  assert(activity.read().empty());
  completion = activity.takeCompletion();
  assert(completion && completion->appId == "44" && completion->name.empty());
  std::ofstream(root / "steamapps/appmanifest_44.acf") << R"("AppState" { "name" "" })";
  write("Running Update,Downloading,", true, false, 44);
  write("update finished : No Error", true, false, 44);
  assert(activity.read().empty());
  completion = activity.takeCompletion();
  assert(completion && completion->name.empty());

  // Quick failures retain both the game and Steam's reason even when no active
  // row survives the poll. Known errors omit the verbose depot wrapper.
  for (const std::string reason :
       {"Disk read failure", "Missing file privileges", "Not enough disk space", "Content servers unreachable",
        "Corrupt update files"}) {
    write("Running Update,Downloading,", true, false, 43);
    write("None", true, false, 43);
    write("update canceled : Failed updating depot 123 while starting download (" + reason + ")", true, false, 43);
    assert(activity.read().empty() && !activity.takeCompletion());
    failure = activity.takeFailure();
    assert(failure && failure->appId == "43" && failure->name == "Second game" && failure->reason == reason);
    assert(!activity.takeFailure());
  }

  // Explicit but unfamiliar failures keep Steam's own report, without guessing
  // a cause. When a later game has no name, no previous title can leak into it.
  const std::string unfamiliar = "Failed updating depot 456 (Unexpected server response)";
  write("Running Update,Downloading,", true, false, 43);
  write("Running Update,Downloading,", true, false, 44);
  write("update finished : Disk write failure", true, false, 43);
  write("update canceled : " + unfamiliar, true, false, 44);
  write("update finished : Disk write failure", true, false, 43); // Duplicate, not a new last failure.
  assert(activity.read().empty());
  failure = activity.takeFailure();
  assert(failure && failure->appId == "44" && failure->name.empty() && failure->reason == unfamiliar);
  assert(!activity.takeFailure());
  SteamActivity afterNamedFailures(home);
  assert(afterNamedFailures.read().empty() && !afterNamedFailures.takeFailure());

  // Failure and success retain separate identities when reported together.
  // The service keeps its existing rule that a reported error takes priority.
  write("Running Update,Downloading,", true, false, 43);
  write("Running Update,Downloading,");
  write("scheduler finished : removed from schedule (result Not enough disk space, state 0x6)", true, false, 43);
  write("update finished : No Error");
  assert(activity.read().empty());
  completion = activity.takeCompletion();
  failure = activity.takeFailure();
  assert(completion && completion->appId == "42");
  assert(
      failure && failure->appId == "43" && failure->name == "Second game" && failure->reason == "Not enough disk space"
  );

  write("Running Update,Downloading,");
  write("update canceled : Failed updating depot 42 (Suspended) \r");
  assert(activity.read()[0].phase == "paused" && !activity.takeFailure());

  // Multiple terminal events in one poll remain individually available to
  // history, in log order. A duplicate does not create another result.
  write("Running Update,Downloading,");
  write("Running Update,Downloading,", true, false, 43);
  write("Running Update,Downloading,", true, false, 44);
  write("update finished : No Error", true, false, 43);
  write("update finished : Disk write failure");
  write("update finished : No Error", true, false, 44);
  write("update finished : No Error", true, false, 43);
  assert(activity.read().empty());
  const auto results = activity.takeResults();
  assert(results.size() == 3);
  assert(results[0].appId == "43" && results[0].name == "Second game" && !results[0].failed());
  assert(results[1].appId == "42" && results[1].reason == "Disk write failure");
  assert(results[2].appId == "44" && results[2].name.empty() && !results[2].failed());
  assert(activity.takeResults().empty());

  std::ofstream(home / ".steam/steam.pid", std::ios::trunc) << "2147483647";
  assert(activity.read().empty()); // A crashed/exited client leaves no activity.
  assert(!activity.takeCompletion());
  assert(!activity.takeFailure());
  fs::remove_all(home);
}
