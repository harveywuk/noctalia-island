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

  // One slot cycles through active kinds every five seconds.
  using namespace std::chrono_literals;
  island::PrivacyRotation rotation;
  const island::PrivacyRotation::Clock::time_point t0{};
  std::vector<island::PrivacyActivity> list{{PrivacyCaptureKind::Microphone, {"Call"}},
                                            {PrivacyCaptureKind::Camera, {"Call"}}};
  TEST_CHECK(rotation.pick({}, t0) == nullptr);
  TEST_CHECK(rotation.pick(list, t0)->kind == PrivacyCaptureKind::Camera); // Newest first.
  TEST_CHECK(rotation.pick(list, t0 + 4s)->kind == PrivacyCaptureKind::Camera);
  TEST_CHECK(rotation.pick(list, t0 + 5s)->kind == PrivacyCaptureKind::Microphone);
  TEST_CHECK(rotation.pick(list, t0 + 10s)->kind == PrivacyCaptureKind::Camera);
  // Hovering holds the current icon, and the interval restarts on leaving.
  TEST_CHECK(rotation.pick(list, t0 + 16s, true)->kind == PrivacyCaptureKind::Camera);
  TEST_CHECK(rotation.pick(list, t0 + 20s)->kind == PrivacyCaptureKind::Camera);
  TEST_CHECK(rotation.pick(list, t0 + 21s)->kind == PrivacyCaptureKind::Microphone);
  // A capture that starts is shown at once; one that stops falls back to what remains.
  list.push_back({PrivacyCaptureKind::Screen, {"Recorder"}});
  TEST_CHECK(rotation.pick(list, t0 + 22s)->kind == PrivacyCaptureKind::Screen);
  list.pop_back();
  TEST_CHECK(rotation.pick(list, t0 + 23s)->kind == PrivacyCaptureKind::Microphone);
  list.erase(list.begin());
  TEST_CHECK(rotation.pick(list, t0 + 24s)->kind == PrivacyCaptureKind::Camera);
  return 0;
}
