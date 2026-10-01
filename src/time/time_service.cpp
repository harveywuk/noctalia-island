#include "time/time_service.h"

#include "core/poll_timeout.h"

#include <chrono>
#include <utility>

TimeService::TimeService() {
  using namespace std::chrono;
  m_now = system_clock::now();
  m_nowSeconds = floor<seconds>(m_now);
}

void TimeService::setTickSecondCallback(TickCallback callback) { m_secondCallback = std::move(callback); }

int TimeService::pollTimeoutMs() const {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto nextSecond = floor<seconds>(now) + seconds{1};
  return pollTimeoutUntil(time_point_cast<system_clock::duration>(nextSecond), now);
}

void TimeService::tick() {
  using namespace std::chrono;
  m_now = system_clock::now();
  const auto floored = floor<seconds>(m_now);

  if (floored != m_nowSeconds) {
    m_nowSeconds = floored;
    if (m_secondCallback) {
      m_secondCallback();
    }
  }
}
