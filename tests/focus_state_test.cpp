#include "notification/focus_state.h"
#include "notification/notification_manager.h"
#include "tests/test_check.h"

namespace {
  std::tm at(int day, int hour, int minute = 0) {
    std::tm value{};
    value.tm_wday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    return value;
  }
} // namespace

int main() {
  FocusConfig config;
  focus::State state;
  state.configure(config);
  state.update(at(1, 10));
  TEST_CHECK(state.active().empty()); // No schedule is enabled by default.
  config.work.scheduleEnabled = true;
  config.sleep.scheduleEnabled = true;
  config.sleep.days = 1; // Only nights starting on Monday.
  state.configure(config);
  state.update(at(1, 8, 59));
  TEST_CHECK(state.active().empty());
  state.update(at(1, 9));
  TEST_CHECK(state.active() == "work");
  TEST_CHECK(state.select("off", at(1, 10)));
  state.update(at(1, 12));
  TEST_CHECK(state.active().empty());
  state.update(at(1, 17));
  TEST_CHECK(state.active().empty() && state.automatic());
  state.update(at(1, 22));
  TEST_CHECK(state.active() == "sleep");
  state.update(at(2, 6, 59));
  TEST_CHECK(state.active() == "sleep");
  state.update(at(2, 7));
  TEST_CHECK(state.active().empty());
  state.update(at(2, 22));
  TEST_CHECK(state.active().empty());
  state.update(at(6, 10));
  TEST_CHECK(state.active().empty());
  state.select("gaming", at(6, 10));
  state.update(at(0, 10));
  TEST_CHECK(state.active() == "gaming");
  TEST_CHECK(!state.select("unknown", at(0, 10)) && state.active() == "gaming");
  state.select("auto", at(1, 10));
  TEST_CHECK(state.active() == "work");
  config.gaming.scheduleEnabled = true;
  config.gaming.startMinute = 600;
  config.gaming.endMinute = 1380;
  state.configure(config);
  state.update(at(1, 10));
  TEST_CHECK(state.active() == "gaming");
  state.update(at(1, 22));
  TEST_CHECK(state.active() == "sleep");
  config.sleep.startMinute = config.sleep.endMinute;
  TEST_CHECK(!focus::scheduled(config.sleep, at(1, 22))); // Equal times never mean all day.

  NotificationManager manager;
  config = {};
  config.work.allowedApps = {" Firefox ", "org.example.Chat.desktop"};
  manager.configureFocus(config);
  TEST_CHECK(manager.selectFocus("work") && manager.doNotDisturb());
  Notification notification{.appName = "Firefox", .urgency = Urgency::Normal};
  TEST_CHECK(!manager.dndSuppresses(notification));
  notification.appName = "Firefox Helper";
  TEST_CHECK(manager.dndSuppresses(notification)); // Exact identities, not substrings.
  notification.desktopEntry = "org.example.Chat";
  TEST_CHECK(!manager.dndSuppresses(notification));
  notification.desktopEntry.reset();
  notification.urgency = Urgency::Critical;
  TEST_CHECK(manager.dndSuppresses(notification));
  config.work.allowCritical = true;
  manager.configureFocus(config);
  TEST_CHECK(!manager.dndSuppresses(notification));
  manager.selectFocus("gaming");
  TEST_CHECK(manager.dndSuppresses(notification));
  notification.dndPolicy = NotificationDndPolicy::Bypass;
  TEST_CHECK(!manager.dndSuppresses(notification));
  notification.dndPolicy = NotificationDndPolicy::Respect;
  manager.setDoNotDisturb(false);
  TEST_CHECK(manager.focusId().empty() && !manager.dndSuppresses(notification));
  manager.setDoNotDisturb(true);
  notification.appName = "Firefox";
  TEST_CHECK(manager.dndSuppresses(notification)); // Plain DND has no preset exceptions.
  manager.selectFocus("work");
  config.work.allowedApps.clear();
  config.work.allowCritical = false;
  manager.configureFocus(config);
  TEST_CHECK(manager.dndSuppresses(notification)); // Editing exceptions applies to existing banners.

  // Recording is a temporary policy, never a replacement for the user's Focus.
  config = {};
  manager.configureFocus(config);
  manager.selectFocus("off");
  manager.setRecordingActive(true);
  TEST_CHECK(manager.focusId().empty() && !manager.doNotDisturb());
  manager.setRecordingActive(false);
  config.whileRecording = true;
  manager.configureFocus(config);
  manager.selectFocus("work");
  manager.setRecordingActive(true);
  TEST_CHECK(manager.focusId() == "recording" && manager.focusAutomatic() && manager.doNotDisturb());
  notification.urgency = Urgency::Normal;
  config.work.allowedApps = {"Firefox"};
  manager.configureFocus(config);
  TEST_CHECK(manager.dndSuppresses(notification)); // The recording policy also silences a preset's allowed apps.
  notification.urgency = Urgency::Critical;
  TEST_CHECK(!manager.dndSuppresses(notification));
  notification.urgency = Urgency::Normal;
  notification.dndPolicy = NotificationDndPolicy::Bypass;
  TEST_CHECK(!manager.dndSuppresses(notification));
  notification.dndPolicy = NotificationDndPolicy::Respect;
  TEST_CHECK(!manager.selectFocus("invalid") && manager.focusId() == "recording");
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId() == "work" && !manager.focusAutomatic());
  TEST_CHECK(!manager.dndSuppresses(notification));

  manager.setDoNotDisturb(true);
  manager.setRecordingActive(true);
  TEST_CHECK(manager.focusId() == "recording");
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId().empty() && manager.doNotDisturb()); // Preserve plain DND as well as presets.
  manager.setDoNotDisturb(false);
  manager.selectFocus("auto");
  manager.setRecordingActive(true);
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId().empty() && manager.focusAutomatic() && !manager.doNotDisturb());

  manager.setRecordingActive(true);
  manager.selectFocus("sleep");
  manager.setRecordingActive(true); // A repeated active update cannot steal the manual choice.
  manager.configureFocus(config);
  TEST_CHECK(manager.focusId() == "sleep" && !manager.focusAutomatic());
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId() == "sleep");
  manager.setRecordingActive(true);
  TEST_CHECK(manager.focusId() == "recording");
  manager.setDoNotDisturb(false);
  config.whileRecording = false;
  manager.configureFocus(config);
  config.whileRecording = true;
  manager.configureFocus(config);
  TEST_CHECK(manager.focusId().empty() && !manager.doNotDisturb()); // Reload/toggle does not undo a user override.
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId().empty() && !manager.doNotDisturb());

  manager.selectFocus("gaming");
  manager.setRecordingActive(true);
  config.whileRecording = false;
  manager.configureFocus(config);
  TEST_CHECK(manager.focusId() == "gaming");
  config.whileRecording = true;
  manager.configureFocus(config);
  TEST_CHECK(manager.focusId() == "recording");
  manager.selectFocus("auto");
  TEST_CHECK(manager.focusId().empty() && manager.focusAutomatic());
  manager.setRecordingActive(false);
  TEST_CHECK(manager.focusId().empty() && manager.focusAutomatic());

  // Scheduling is independent of the temporary recording policy. Ending a
  // recording follows the current schedule rather than restoring a stale profile.
  focus::RecordingOverride recording;
  recording.configure(true);
  state.configure(FocusConfig{.work = {.scheduleEnabled = true}});
  state.select("auto", at(1, 10));
  recording.setRecording(true);
  state.update(at(1, 18));
  TEST_CHECK(recording.active() && state.active().empty() && state.automatic());
  recording.setRecording(false);
  TEST_CHECK(!recording.active() && state.active().empty());
  return 0;
}
