#include "shell/desktop/desktop_stack_rotation.h"

#include <algorithm>

namespace desktop_stacks {
  using namespace std::chrono_literals;

  bool hasUpcomingEvent(std::span<const CalendarEvent> events, std::chrono::system_clock::time_point now) {
    return std::ranges::any_of(events, [now](const auto& event) {
      return !event.allDay
          && event.end >= event.start
          && event.end > now
          && event.start <= now + 15min
          && event.start > now - 5min;
    });
  }

  SmartRotation::Decision SmartRotation::evaluate(
      const SmartContext& context, const SmartPages& pages, std::size_t currentPage, bool paused, Clock::time_point now
  ) {
    if (paused || (m_manualUntil && now < *m_manualUntil))
      return {.hold = true};

    std::optional<std::size_t> target;
    bool morning = false;
    if (pages.calendar && context.calendarImminent) {
      target = pages.calendar;
    } else if (pages.media && context.mediaPlaying) {
      target = pages.media;
    } else if (
        pages.weather
        && context.weatherAvailable
        && context.localDay > 0
        && context.localHour >= 6
        && context.localHour < 10
        && (context.localDay != m_lastWeatherDay || (m_weatherUntil && now < *m_weatherUntil))
    ) {
      target = pages.weather;
      morning = true;
    }
    if (!target)
      return {};
    if (*target != currentPage && m_lastSwitch && now < *m_lastSwitch + 10s)
      return {.hold = true};

    if (!morning)
      m_weatherUntil.reset();
    if (morning && context.localDay != m_lastWeatherDay) {
      m_lastWeatherDay = context.localDay;
      m_weatherUntil = now + 90s;
    }
    if (*target == currentPage)
      return {.hold = true};
    m_lastSwitch = now;
    return {.page = target, .hold = true};
  }

  void SmartRotation::manualSelection(Clock::time_point now) {
    m_manualUntil = now + 5min;
    m_weatherUntil.reset();
  }

  void SmartRotation::resume() {
    m_manualUntil.reset();
    m_lastSwitch.reset();
  }
} // namespace desktop_stacks
