#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

// poll(2) accepts whole milliseconds. Rounding a future deadline down to zero
// makes the event loop spin until the remaining fractional millisecond elapses.
template <typename Clock, typename Duration>
int pollTimeoutUntil(std::chrono::time_point<Clock, Duration> deadline, std::chrono::time_point<Clock, Duration> now) {
  if (deadline <= now)
    return 0;
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<int>(std::min<std::int64_t>(remaining, std::numeric_limits<int>::max()));
}
