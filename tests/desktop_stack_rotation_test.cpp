#include "shell/desktop/desktop_stack_rotation.h"
#include "shell/desktop/desktop_widget_settings_registry.h"

#include <cstdlib>
#include <print>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using namespace std::chrono_literals;
  using namespace desktop_stacks;
  const auto wall = std::chrono::system_clock::time_point{1000000s};
  CalendarEvent event{.start = wall + 15min, .end = wall + 45min};
  expect(hasUpcomingEvent({&event, 1}, wall), "calendar becomes relevant fifteen minutes before an event");
  expect(!hasUpcomingEvent({&event, 1}, wall - 1s), "distant event does not take over");
  expect(hasUpcomingEvent({&event, 1}, wall + 19min + 59s), "calendar stays relevant during the first five minutes");
  expect(!hasUpcomingEvent({&event, 1}, wall + 20min), "past event releases the stack");
  event.allDay = true;
  expect(!hasUpcomingEvent({&event, 1}, wall), "all-day entries do not dominate the stack");
  event.allDay = false;
  event.end = event.start + 1min;
  expect(!hasUpcomingEvent({&event, 1}, event.end), "finished short event is no longer relevant");
  event.end = event.start;
  expect(hasUpcomingEvent({&event, 1}, wall), "instantaneous timed entries are relevant before their start");
  event.end = event.start - 1s;
  expect(!hasUpcomingEvent({&event, 1}, wall), "invalid event range is ignored");
  expect(!hasUpcomingEvent({}, wall), "missing calendar data is quiet");

  const SmartPages pages{.calendar = 0, .media = 1, .weather = 2};
  const SmartContext all{
      .calendarImminent = true, .mediaPlaying = true, .weatherAvailable = true, .localDay = 20261006, .localHour = 7
  };
  const auto epoch = SmartRotation::Clock::time_point{};
  SmartRotation rotation;
  auto choice = rotation.evaluate(all, pages, 3, false, epoch);
  expect(choice.page == 0 && choice.hold, "calendar outranks playback and weather");
  expect(rotation.lastWeatherDay() == 0, "deferred weather cue is not consumed");
  choice = rotation.evaluate(all, pages, 0, false, epoch + 1s);
  expect(!choice.page && choice.hold, "current relevant page holds ordinary rotation without reselecting itself");
  auto context = all;
  context.calendarImminent = false;
  choice = rotation.evaluate(context, pages, 0, false, epoch + 2s);
  expect(!choice.page && choice.hold, "cooldown avoids a rapid switch when priorities change");
  choice = rotation.evaluate(context, pages, 0, false, epoch + 10s);
  expect(choice.page == 1 && choice.hold, "active playback takes over when the event ends");
  context.mediaPlaying = false;
  choice = rotation.evaluate(context, pages, 1, false, epoch + 20s);
  expect(
      choice.page == 2 && choice.hold && rotation.lastWeatherDay() == 20261006,
      "morning weather surfaces after higher priorities clear"
  );
  choice = rotation.evaluate(context, pages, 2, false, epoch + 109s);
  expect(!choice.page && choice.hold, "weather holds its morning slot for ninety seconds");
  choice = rotation.evaluate(context, pages, 2, false, epoch + 110s);
  expect(!choice.page && !choice.hold, "ordinary rotation resumes after the morning slot");
  expect(
      !rotation.evaluate(context, pages, 3, false, epoch + 500s).page,
      "weather is not repeatedly suggested that morning"
  );
  context.localDay = 20261007;
  expect(
      rotation.evaluate(context, pages, 3, false, epoch + 1000s).page == 2, "next local date gets a fresh morning slot"
  );
  context.localHour = 10;
  expect(!rotation.evaluate(context, pages, 2, false, epoch + 1001s).hold, "morning slot never extends past ten");

  SmartRotation persisted;
  persisted.setLastWeatherDay(20261006);
  context = all;
  context.calendarImminent = context.mediaPlaying = false;
  expect(!persisted.evaluate(context, pages, 3, false, epoch).page, "persisted morning day survives reconstruction");
  context.localDay = 20261007;
  context.localHour = 5;
  expect(!persisted.evaluate(context, pages, 3, false, epoch).page, "weather waits until six local time");
  context.localHour = 6;
  context.weatherAvailable = false;
  expect(!persisted.evaluate(context, pages, 3, false, epoch).page, "missing weather does not surface an empty card");
  context.weatherAvailable = true;
  expect(
      persisted.evaluate(context, pages, 2, false, epoch).hold && persisted.lastWeatherDay() == 20261007,
      "weather already visible still counts as shown"
  );

  SmartRotation interruptedMorning;
  context = all;
  context.calendarImminent = context.mediaPlaying = false;
  expect(interruptedMorning.evaluate(context, pages, 3, false, epoch).page == 2, "weather starts its morning slot");
  context.mediaPlaying = true;
  expect(
      interruptedMorning.evaluate(context, pages, 2, false, epoch + 10s).page == 1,
      "playback can interrupt the morning slot"
  );
  context.mediaPlaying = false;
  choice = interruptedMorning.evaluate(context, pages, 1, false, epoch + 11s);
  expect(!choice.page && !choice.hold, "an interrupted morning slot does not repeatedly take over");

  SmartRotation manual;
  manual.manualSelection(epoch);
  choice = manual.evaluate(all, pages, 3, false, epoch + 299s);
  expect(!choice.page && choice.hold, "manual navigation protects its page and holds timed rotation for five minutes");
  choice = manual.evaluate(all, pages, 3, false, epoch + 300s);
  expect(choice.page == 0, "smart rotation resumes when the manual quiet period expires");
  manual.manualSelection(epoch + 301s);
  manual.resume();
  expect(manual.evaluate(all, pages, 3, false, epoch + 302s).page == 0, "explicit unpin resumes immediately");
  SmartRotation paused;
  choice = paused.evaluate(all, pages, 3, true, epoch);
  expect(
      !choice.page && choice.hold && paused.lastWeatherDay() == 0,
      "hover, focus, pins and previews cannot switch or consume cues"
  );
  context = {};
  choice = paused.evaluate(context, pages, 3, false, epoch + 60s);
  expect(!choice.page && !choice.hold, "expired cues are not queued during interaction");
  expect(
      paused.evaluate(all, pages, 3, false, epoch + 61s).page == 0, "still-relevant context is considered on resume"
  );
  SmartRotation absent;
  choice = absent.evaluate(all, {}, 0, false, epoch);
  expect(!choice.page && !choice.hold, "only cards present in the stack can be suggested");
  choice = absent.evaluate(all, {.media = 3}, 0, false, epoch);
  expect(choice.page == 3, "missing calendar page allows playback to take priority");

  auto defaults = std::unordered_map<std::string, WidgetSettingValue>{};
  desktop_settings::applyAllDesktopWidgetDefaultSettings(defaults, "stack");
  expect(!std::get<bool>(defaults.at("smart_rotate")), "Smart Rotate is opt-in and resets to off");
  std::println("PASS: Smart Rotate priorities, time windows, persistence, cooldowns and interaction guards");
}
