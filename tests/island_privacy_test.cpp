#include "shell/island/island_privacy.h"
#include "tests/test_check.h"

int main() {
  island::PrivacySummary summary;
  ShellConfig::PrivacyConfig config;
  PrivacyState state;
  TEST_CHECK(summary.snapshot(state, config).empty());
  state.captures = {
      {PrivacyCaptureKind::Microphone, 1, "Zen"},
      {PrivacyCaptureKind::Camera, 2, "Call"},
      {PrivacyCaptureKind::Microphone, 3, "Zen"},
      {PrivacyCaptureKind::Screen, 4, "Recorder"},
      {PrivacyCaptureKind::Microphone, 5, "Call"}
  };
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
  state.captures = {
      {PrivacyCaptureKind::Camera, 0, "Discord", "discord"},
      {PrivacyCaptureKind::Camera, 7, "Firefox", "firefox"},
      {PrivacyCaptureKind::Camera, 8, "Firefox", "firefox"},
      {PrivacyCaptureKind::Camera, 9, "Portal app"}
  };
  snapshot = summary.snapshot(state, config);
  TEST_CHECK(snapshot.size() == 1 && snapshot[0].apps.size() == 3);
  TEST_CHECK((snapshot[0].binaries == std::vector<std::string>{"discord", "firefox"}));
  state.captures.clear();
  TEST_CHECK(summary.snapshot(state, config).empty());
  state.captures = {
      {PrivacyCaptureKind::Microphone, 1, "Call", "call", {40, 41}},
      {PrivacyCaptureKind::Microphone, 2, "Recorder", "recorder", {40}},
      {PrivacyCaptureKind::Microphone, 3, "Ignored", "ignored", {99}}
  };
  config.micFilterRegex = "Ignored";
  snapshot = summary.snapshot(state, config);
  TEST_CHECK(snapshot.size() == 1);
  TEST_CHECK((snapshot[0].sourceIds == std::vector<std::uint32_t>{40, 41}));
  TEST_CHECK(snapshot[0].appNames() == "Call, Recorder");
  state.captures.erase(state.captures.begin());
  TEST_CHECK((summary.snapshot(state, config)[0].sourceIds == std::vector<std::uint32_t>{40}));

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
  island::CaptureSessions sessions;
  sessions.update({"Browser", "Browser"}, t0);
  TEST_CHECK(sessions.sessions().size() == 1);
  TEST_CHECK(sessions.elapsed("Browser", t0 + 12s) == 12s);
  sessions.update({"Call", "Browser"}, t0 + 15s);
  TEST_CHECK(sessions.elapsed("Browser", t0 + 20s) == 20s);
  TEST_CHECK(sessions.elapsed("Call", t0 + 20s) == 5s);
  // Repeated snapshots, metadata order and overlapping app sessions preserve elapsed time.
  sessions.update({"Browser", "Call", "Browser"}, t0 + 30s);
  TEST_CHECK(sessions.elapsed("Browser", t0 + 3601s) == 3601s);
  sessions.update({"Call"}, t0 + 31s);
  TEST_CHECK(!sessions.elapsed("Browser", t0 + 32s));
  sessions.update({"Browser", "Call"}, t0 + 40s);
  TEST_CHECK(sessions.elapsed("Browser", t0 + 41s) == 1s);
  TEST_CHECK(sessions.elapsed("Call", t0 + 41s) == 26s);
  sessions.update({}, t0 + 45s);
  TEST_CHECK(sessions.sessions().empty());
  // One app's camera and screen activity have independent lifetimes. Applying a
  // camera filter removes only that activity, and unfiltering starts a new observation.
  island::CaptureSessions camera, screen;
  config = {};
  state.captures = {
      {PrivacyCaptureKind::Camera, 1, "Call"},
      {PrivacyCaptureKind::Camera, 2, "Call"},
      {PrivacyCaptureKind::Screen, 3, "Call"}
  };
  const auto update = [&](auto at) {
    auto groups = summary.snapshot(state, config);
    for (auto kind : {PrivacyCaptureKind::Camera, PrivacyCaptureKind::Screen}) {
      auto found = std::ranges::find(groups, kind, &island::PrivacyActivity::kind);
      (kind == PrivacyCaptureKind::Camera ? camera : screen)
          .update(found == groups.end() ? std::vector<std::string>{} : found->apps, at);
    }
  };
  update(t0);
  state.captures.erase(state.captures.begin());
  update(t0 + 10s);
  TEST_CHECK(camera.elapsed("Call", t0 + 12s) == 12s);
  config.camFilterRegex = "Call";
  update(t0 + 15s);
  TEST_CHECK(camera.sessions().empty() && screen.elapsed("Call", t0 + 20s) == 20s);
  config.camFilterRegex.clear();
  update(t0 + 25s);
  TEST_CHECK(camera.elapsed("Call", t0 + 30s) == 5s);
  TEST_CHECK(screen.elapsed("Call", t0 + 30s) == 30s);
  state.captures.erase(state.captures.begin());
  update(t0 + 35s);
  TEST_CHECK(camera.sessions().empty() && screen.elapsed("Call", t0 + 40s) == 40s);
  return 0;
}
