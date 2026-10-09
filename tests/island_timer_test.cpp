#include "shell/island/island_timer.h"
#include "tests/test_check.h"

#include <chrono>
#include <limits>
#include <unordered_set>
#include <vector>

int main() {
  using nlohmann::json;
  auto timer = island::timerSnapshot("RUNNING", 75, 100);
  TEST_CHECK(timer && timer->active && timer->running);
  TEST_CHECK(timer->time() == "1:15" && timer->fraction() == .75F);
  TEST_CHECK(timer->toggleCommand() == "PAUSE");
  timer = island::timerSnapshot("PAUSED", 75, 100);
  TEST_CHECK(timer && timer->active && !timer->running && timer->toggleCommand() == "START");
  TEST_CHECK(!island::timerSnapshot("IDLE", 100, 0)->active);
  TEST_CHECK(island::timerSnapshot("NOTIFY", 0, 100)->finished);
  TEST_CHECK(!island::timerSnapshot("RUNNING", -1, 100));
  TEST_CHECK(!island::timerSnapshot("RUNNING", "75", 100));
  TEST_CHECK(!island::timerSnapshot("INVALID", 75, 100));
  TEST_CHECK(!island::timerSnapshot("RUNNING", 1e100, 100));
  TEST_CHECK(island::timerSnapshot("RUNNING", 3601, 4000)->time() == "1:00:01");
  {
    const auto displayed = *island::timerSnapshot("PAUSED", 75, 100);
    TEST_CHECK(island::acceptsCountdownCommand(displayed, displayed, "START"));
    TEST_CHECK(island::acceptsCountdownCommand(displayed, displayed, "RESET"));
    TEST_CHECK(!island::acceptsCountdownCommand(displayed, *island::timerSnapshot("IDLE", 100, 100), "START"));
    TEST_CHECK(!island::acceptsCountdownCommand(displayed, *island::timerSnapshot("NOTIFY", 0, 100), "START"));
    TEST_CHECK(!island::acceptsCountdownCommand(displayed, *island::timerSnapshot("RUNNING", 74, 100), "START"));
    TEST_CHECK(!island::acceptsCountdownCommand(displayed, *island::timerSnapshot("PAUSED", 75, 200), "START"));
    TEST_CHECK(!island::acceptsCountdownCommand(displayed, displayed, "PAUSE"));
    TEST_CHECK(island::acceptsCountdownCommand(displayed, *island::timerSnapshot("NOTIFY", 0, 100), "RESET"));
  }
  json state = {
      {"isRunning", true}, {"isDirty", true}, {"secondsLeft", 1250}, {"sessionPtr", {{"session", 1}, {"stage", 1}}}
  };
  json sessions = {{1500, 300}, {1500, 900}};
  auto pomo = island::pomodoroSnapshot(state, sessions);
  TEST_CHECK(pomo && pomo->active && pomo->running && !pomo->onBreak && pomo->duration == 1500);
  TEST_CHECK(pomo->toggleCommand() == "toggle" && pomo->cancelCommand() == "resetAll");
  state["sessionPtr"] = {{"session", 2}, {"stage", 2}};
  state["secondsLeft"] = 900;
  state["isRunning"] = false;
  pomo = island::pomodoroSnapshot(state, sessions);
  TEST_CHECK(pomo && pomo->active && pomo->onBreak && pomo->duration == 900 && !pomo->running);
  state["isDirty"] = false;
  TEST_CHECK(!island::pomodoroSnapshot(state, sessions)->active);
  state["sessionPtr"]["session"] = 999999;
  TEST_CHECK(!island::pomodoroSnapshot(state, sessions));
  TEST_CHECK(!island::pomodoroSnapshot(json::array(), sessions));

  using namespace std::chrono_literals;
  const auto now = std::chrono::system_clock::time_point{} + 1000h;
  const auto event = [&](std::string id, auto start, auto length, bool allDay = false) {
    return CalendarEvent{
        .id = std::move(id),
        .title = "Standup",
        .url = "https://meet.example/x",
        .start = now + start,
        .end = now + start + length,
        .allDay = allDay
    };
  };
  std::unordered_set<std::string> dismissed;
  std::vector<CalendarEvent> events{
      event("later", 20min, 30min), event("soon", 4min, 30min), event("holiday", -1h, 24h, true)
  };
  auto next = island::upNextSnapshot(events, now, 10, dismissed);
  TEST_CHECK(next && next->event && next->active && next->running && next->title == "Standup");
  TEST_CHECK(next->remaining == 240 && next->duration == 600 && next->time() == "4:00");
  TEST_CHECK(next->url == "https://meet.example/x" && next->panel == "calendar");
  TEST_CHECK(!island::upNextSnapshot(events, now, 0, dismissed));                        // 0 turns it off
  TEST_CHECK(!island::upNextSnapshot(events, now, 3, dismissed));                        // nothing that close
  TEST_CHECK(island::upNextSnapshot(events, now + 6min, 10, dismissed)->remaining == 0); // started
  TEST_CHECK(!island::upNextSnapshot(events, now + 9min, 3, dismissed));                 // gone 5 minutes in
  dismissed.insert(next->plugin);
  TEST_CHECK(!island::upNextSnapshot(events, now, 10, dismissed));
  TEST_CHECK(island::upNextSnapshot(events, now + 15min, 10, dismissed)->remaining == 300); // next one
  return 0;
}
