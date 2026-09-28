#include "shell/island/island_timer.h"
#include "tests/test_check.h"
#include <limits>

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
  json state = {{"isRunning", true}, {"isDirty", true}, {"secondsLeft", 1250},
                {"sessionPtr", {{"session", 1}, {"stage", 1}}}};
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
  return 0;
}
