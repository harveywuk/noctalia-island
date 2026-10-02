#include "shell/island/island_privacy.h"
#include "tests/test_check.h"

int main() {
  island::PrivacySummary summary;
  ShellConfig::PrivacyConfig config;
  PrivacyState state;
  TEST_CHECK(summary.snapshot(state, config).empty());
  state.captures = {{PrivacyCaptureKind::Microphone, 1, "Zen"},
                    {PrivacyCaptureKind::Camera, 2, "Call"},
                    {PrivacyCaptureKind::Microphone, 3, "Zen"},
                    {PrivacyCaptureKind::Screen, 4, "Recorder"},
                    {PrivacyCaptureKind::Microphone, 5, "Call"}};
  auto snapshot = summary.snapshot(state, config);
  TEST_CHECK(snapshot.size() == 3);
  TEST_CHECK(snapshot[0].appNames() == "Call, Zen");
  TEST_CHECK(snapshot[1].appNames() == "Call");
  config.micFilterRegex = "Zen";
  config.camFilterRegex = "Call";
  snapshot = summary.snapshot(state, config);
  TEST_CHECK(snapshot.size() == 2);
  TEST_CHECK(snapshot[0].appNames() == "Call");
  TEST_CHECK(snapshot[1].kind == PrivacyCaptureKind::Screen);
  config.micFilterRegex = "["; // A malformed filter must not conceal active capture.
  TEST_CHECK(summary.snapshot(state, config)[0].apps.size() == 2);
  config = {};
  state.captures.erase(state.captures.begin()); // A second stream from Zen remains.
  TEST_CHECK(summary.snapshot(state, config)[0].appNames() == "Call, Zen");
  // Binaries ride along for focusing the capturing app's window.
  state.captures = {{PrivacyCaptureKind::Camera, 0, "Discord", "discord"},
                    {PrivacyCaptureKind::Camera, 7, "Firefox", "firefox"},
                    {PrivacyCaptureKind::Camera, 8, "Firefox", "firefox"},
                    {PrivacyCaptureKind::Camera, 9, "Portal app"}};
  snapshot = summary.snapshot(state, config);
  TEST_CHECK(snapshot.size() == 1 && snapshot[0].apps.size() == 3);
  TEST_CHECK((snapshot[0].binaries == std::vector<std::string>{"discord", "firefox"}));
  state.captures.clear();
  TEST_CHECK(summary.snapshot(state, config).empty());

  // One slot cycles through active indicators every five seconds.
  using namespace std::chrono_literals;
  island::PrivacyRotation rotation;
  const island::PrivacyRotation::Clock::time_point t0{};
  std::vector<std::string> ids{"microphone", "camera"};
  const auto shown = [&](auto at, bool hold = false) { return ids[*rotation.pick(ids, t0 + at, hold)]; };
  TEST_CHECK(!rotation.pick({}, t0));
  TEST_CHECK(shown(0s) == "camera"); // Newest first.
  TEST_CHECK(shown(4s) == "camera");
  TEST_CHECK(shown(5s) == "microphone");
  TEST_CHECK(shown(10s) == "camera");
  // Hovering holds the current icon, and the interval restarts on leaving.
  TEST_CHECK(shown(16s, true) == "camera");
  TEST_CHECK(shown(20s) == "camera");
  TEST_CHECK(shown(21s) == "microphone");
  // An indicator that appears is shown at once; one that goes falls back to what remains.
  ids.push_back("notifications");
  TEST_CHECK(shown(22s) == "notifications");
  TEST_CHECK(shown(27s) == "microphone");
  ids.erase(ids.begin());
  TEST_CHECK(shown(28s) == "camera");
  return 0;
}
