#pragma once

#include "config/config_types.h"
#include "shell/island/island_state.h"

#include <array>
#include <chrono>
#include <optional>

namespace island {
  using ActivityOrder = std::array<Activity, 3>;

  constexpr ActivityOrder activityOrder(IslandActivityPriority priority) {
    using enum Activity;
    switch (priority) {
    case IslandActivityPriority::TimersMediaDownloads:
      return {Timers, Media, Downloads};
    case IslandActivityPriority::DownloadsTimersMedia:
      return {Downloads, Timers, Media};
    case IslandActivityPriority::DownloadsMediaTimers:
      return {Downloads, Media, Timers};
    case IslandActivityPriority::MediaTimersDownloads:
      return {Media, Timers, Downloads};
    case IslandActivityPriority::MediaDownloadsTimers:
      return {Media, Downloads, Timers};
    default:
      return {Timers, Downloads, Media};
    }
  }

  constexpr Activity preferredActivity(Activities available, ActivityOrder order, Activity retained = Activity::None) {
    if (available.contains(retained))
      return retained;
    for (const auto activity : order)
      if (available.contains(activity))
        return activity;
    return Activity::None;
  }

  // The split Island's bubble: the first other running activity in priority order.
  constexpr Activity secondaryActivity(Activities available, ActivityOrder order, Activity primary) {
    for (const auto activity : order)
      if (activity != primary && available.contains(activity))
        return activity;
    return Activity::None;
  }

  class CompactActivity {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Activity selected() const { return m_selected; }
    std::optional<TimePoint> nextExpiry() const { return m_paused ? std::nullopt : m_next; }

    void pause(TimePoint now) {
      if (!m_paused)
        m_paused = now;
    }

    // Brings an activity into the capsule (a click on the split bubble) until it ends.
    void promote(Activity activity) {
      m_promoted = activity;
      m_selected = activity;
      m_next.reset();
      m_paused.reset();
    }

    void
    update(Activities available, IslandActivityPriority priority, bool cycle, int seconds, bool paused, TimePoint now) {
      const auto order = activityOrder(priority);
      const auto interval = std::chrono::seconds(std::clamp(seconds, 1, 30));
      const bool changed = priority != m_priority || cycle != m_cycle || interval != m_interval;
      m_priority = priority;
      m_cycle = cycle;
      m_interval = interval;
      if (!available.contains(m_promoted))
        m_promoted = Activity::None;
      if (changed || !cycle || !available.contains(m_selected)) {
        m_selected = preferredActivity(available, order, m_promoted);
        m_next.reset();
        m_paused.reset();
      }
      if (!cycle || available.count() < 2) {
        m_next.reset();
        m_paused.reset();
        return;
      }
      if (!m_next) {
        m_next = now + interval;
        m_paused.reset();
      }
      if (paused) {
        pause(now);
        return;
      }
      if (m_paused) {
        *m_next += now - *m_paused;
        m_paused.reset();
      }
      if (now >= *m_next) {
        const auto current = std::ranges::find(order, m_selected) - order.begin();
        for (std::size_t offset = 1; offset <= order.size(); ++offset) {
          const auto next = order[(current + offset) % order.size()];
          if (available.contains(next)) {
            m_selected = next;
            break;
          }
        }
        m_next = now + interval;
      }
    }

  private:
    Activity m_selected = Activity::None;
    Activity m_promoted = Activity::None;
    IslandActivityPriority m_priority = IslandActivityPriority::TimersDownloadsMedia;
    bool m_cycle = false;
    std::chrono::seconds m_interval{5};
    std::optional<TimePoint> m_next;
    std::optional<TimePoint> m_paused;
  };
} // namespace island
