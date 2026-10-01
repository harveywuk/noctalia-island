#include "core/poll_timeout.h"

#include <cassert>
#include <print>

int main() {
  using namespace std::chrono;
  const steady_clock::time_point now{seconds(10)};
  assert(pollTimeoutUntil(now, now) == 0);
  assert(pollTimeoutUntil(now - nanoseconds(1), now) == 0);
  assert(pollTimeoutUntil(now + nanoseconds(1), now) == 1);
  assert(pollTimeoutUntil(now + microseconds(999), now) == 1);
  assert(pollTimeoutUntil(now + milliseconds(1), now) == 1);
  assert(pollTimeoutUntil(now + milliseconds(1) + nanoseconds(1), now) == 2);
  assert(pollTimeoutUntil(now + hours(24 * 100), now) == std::numeric_limits<int>::max());
  const system_clock::time_point wall{seconds(10)};
  assert(pollTimeoutUntil(wall + seconds(1), wall + microseconds(999900)) == 1);
  std::println("PASS: future deadlines sleep, due deadlines dispatch, large waits saturate");
}
