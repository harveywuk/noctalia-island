#include "idle/idle_inhibitor.h"
#include "tests/test_check.h"

#include <thread>

using namespace std::chrono_literals;

int main() {
  IdleInhibitor idle;
  int changes = 0;
  idle.setChangeCallback([&] { ++changes; });
  idle.setEnabledFor(25ms);
  TEST_CHECK(idle.enabled() && idle.remaining());
  std::this_thread::sleep_for(40ms);
  TimerManager::instance().tick();
  TEST_CHECK(!idle.enabled() && !idle.remaining() && changes == 2);
  idle.setEnabledFor(25ms);
  idle.setEnabled(true); // Choosing indefinite cancels the old deadline.
  std::this_thread::sleep_for(40ms);
  TimerManager::instance().tick();
  TEST_CHECK(idle.enabled() && !idle.remaining());
  idle.setEnabledFor(25ms);
  idle.setEnabledFor(1h); // Replacing a timer cannot be stopped by its old expiry.
  std::this_thread::sleep_for(40ms);
  TimerManager::instance().tick();
  TEST_CHECK(idle.enabled() && idle.remaining()->count() > 3590);
  idle.toggle();
  TEST_CHECK(!idle.enabled() && !idle.remaining());
  idle.setEnabledFor(0ms);
  TEST_CHECK(!idle.enabled());
  TEST_CHECK(!idle.extendTimed(15min));
  idle.setEnabled(true);
  TEST_CHECK(!idle.extendTimed(15min) && !idle.remaining());
  idle.setEnabledFor(25ms);
  std::this_thread::sleep_for(40ms);
  TEST_CHECK(!idle.extendTimed(15min)); // A late click cannot revive an expired timer.
  TimerManager::instance().tick();
  TEST_CHECK(!idle.enabled());
  idle.setEnabledFor(25ms);
  TEST_CHECK(idle.extendTimed(1h));
  std::this_thread::sleep_for(40ms);
  TimerManager::instance().tick();
  TEST_CHECK(idle.enabled() && idle.remaining()->count() >= 3599);
  TEST_CHECK(!idle.extendTimed(0ms) && !idle.extendTimed(-1ms));
  TEST_CHECK(idle.extendTimed(15min));
  TEST_CHECK(idle.remaining()->count() >= 4499); // Adds to the deadline, not to now.
  idle.setEnabled(false);
  TEST_CHECK(!idle.extendTimed(15min));
  return 0;
}
