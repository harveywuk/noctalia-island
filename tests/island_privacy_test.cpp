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
  return 0;
}
