#pragma once

#include "calendar/calendar_types.h"

#include <chrono>
#include <optional>
#include <span>

namespace desktop_stacks {
  [[nodiscard]] bool hasUpcomingEvent(std::span<const CalendarEvent> events, std::chrono::system_clock::time_point now);

  struct SmartPages {
    std::optional<std::size_t> calendar;
    std::optional<std::size_t> media;
    std::optional<std::size_t> weather;
    [[nodiscard]] bool empty() const { return !calendar && !media && !weather; }
  };

  struct SmartContext {
    bool calendarImminent = false;
    bool mediaPlaying = false;
    bool weatherAvailable = false;
    int localDay = 0; // YYYYMMDD in the desktop's timezone.
    int localHour = 0;
  };

  // Source-independent policy: wall time determines relevance; monotonic time
  // governs quiet periods so clock corrections cannot produce rapid switches.
  class SmartRotation {
  public:
    using Clock = std::chrono::steady_clock;
    struct Decision {
      std::optional<std::size_t> page;
      bool hold = false; // Suspend ordinary timed rotation while a cue or pause is active.
    };
    [[nodiscard]] Decision evaluate(
        const SmartContext& context, const SmartPages& pages, std::size_t currentPage, bool paused,
        Clock::time_point now = Clock::now()
    );
    void manualSelection(Clock::time_point now = Clock::now());
    void resume();
    [[nodiscard]] int lastWeatherDay() const { return m_lastWeatherDay; }
    void setLastWeatherDay(int day) { m_lastWeatherDay = day; }

  private:
    int m_lastWeatherDay = 0;
    std::optional<Clock::time_point> m_lastSwitch;
    std::optional<Clock::time_point> m_manualUntil;
    std::optional<Clock::time_point> m_weatherUntil;
  };
} // namespace desktop_stacks
