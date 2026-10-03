#pragma once

#include "calendar/calendar_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace island {
  struct Countdown {
    std::string plugin, panel, titleKey, icon;
    int remaining = 0, duration = 0, session = 0;
    bool running = false, active = false, finished = false, pomodoro = false, onBreak = false, event = false;
    // Up-next calendar events carry their own title and an optional meeting link.
    std::string title, url;
    float fraction() const { return duration > 0 ? std::clamp(static_cast<float>(remaining) / static_cast<float>(duration), 0.0F, 1.0F) : 0; }
    std::string time() const {
      return remaining >= 3600 ? std::format("{}:{:02}:{:02}", remaining / 3600, remaining / 60 % 60, remaining % 60)
                               : std::format("{}:{:02}", remaining / 60, remaining % 60);
    }
    std::string commandKey() const { return pomodoro ? "pomodoro.nextCommand" : "timer.cmd"; }
    std::string toggleCommand() const { return pomodoro ? "toggle" : running ? "PAUSE" : "START"; }
    std::string cancelCommand() const { return pomodoro ? "resetAll" : "RESET"; }
  };

  inline std::optional<int> countdownSeconds(const nlohmann::json& value) {
    if (!value.is_number()) return std::nullopt;
    const auto seconds = value.get<double>();
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 31536000) return std::nullopt;
    return static_cast<int>(seconds);
  }

  // These are adapters for the plugins' published state, not another timer engine.
  inline std::optional<Countdown> timerSnapshot(const nlohmann::json& state, const nlohmann::json& remaining,
                                               const nlohmann::json& duration) {
    if (!state.is_string()) return std::nullopt;
    const std::string status = state.get<std::string>();
    if (status != "IDLE" && status != "RUNNING" && status != "PAUSED" && status != "NOTIFY") return std::nullopt;
    const auto seconds = countdownSeconds(remaining), total = countdownSeconds(duration);
    if (!seconds || !total) return std::nullopt;
    return Countdown{"noctalia/timer", "noctalia/timer:panel", "island.timer.title", "hourglass",
        *seconds, *total, 0, status == "RUNNING", status != "IDLE", status == "NOTIFY"};
  }

  inline std::optional<Countdown> pomodoroSnapshot(const nlohmann::json& state, const nlohmann::json& sessions) {
    if (!state.is_object() || !sessions.is_array() || !state.contains("sessionPtr")
        || !state["sessionPtr"].is_object() || !state.contains("secondsLeft")
        || !state.contains("isRunning") || !state["isRunning"].is_boolean()
        || !state.contains("isDirty") || !state["isDirty"].is_boolean()) return std::nullopt;
    const auto& ptr = state["sessionPtr"];
    if (!ptr.contains("session") || !ptr.contains("stage")) return std::nullopt;
    const auto session = countdownSeconds(ptr["session"]), stage = countdownSeconds(ptr["stage"]);
    const auto remaining = countdownSeconds(state["secondsLeft"]);
    if (!session || *session < 1 || static_cast<std::size_t>(*session) > sessions.size()
        || !stage || (*stage != 1 && *stage != 2) || !remaining) return std::nullopt;
    const auto& cycle = sessions[static_cast<std::size_t>(*session - 1)];
    if (!cycle.is_array() || cycle.size() < 2) return std::nullopt;
    const auto duration = countdownSeconds(cycle[static_cast<std::size_t>(*stage - 1)]);
    if (!duration || *duration == 0) return std::nullopt;
    const bool running = state["isRunning"].get<bool>();
    return Countdown{"thepunkoff/pomodoro", "thepunkoff/pomodoro:panel",
        *stage == 1 ? "island.timer.focus" : "island.timer.break", *stage == 1 ? "brain" : "coffee",
        *remaining, *duration, *session, running, running || state["isDirty"].get<bool>(), false, true, *stage == 2};
  }

  // The next timed calendar event starting within `minutes`. It reads "now" for its first five
  // minutes, then gives way; `dismissed` holds the ids of events the user closed.
  inline std::optional<Countdown> upNextSnapshot(
      const std::vector<CalendarEvent>& events, std::chrono::system_clock::time_point now, int minutes,
      const std::unordered_set<std::string>& dismissed
  ) {
    using namespace std::chrono;
    if (minutes <= 0)
      return std::nullopt;
    const CalendarEvent* next = nullptr;
    std::string nextId;
    for (const auto& event : events) {
      if (event.allDay
          || event.start > now + std::chrono::minutes(minutes)
          || now >= event.start + 5min
          || (event.end > event.start && now >= event.end))
        continue;
      auto id = std::format("calendar:{}@{}", event.id, duration_cast<seconds>(event.start.time_since_epoch()).count());
      if (dismissed.contains(id) || (next != nullptr && next->start <= event.start))
        continue;
      next = &event;
      nextId = std::move(id);
    }
    if (next == nullptr)
      return std::nullopt;
    const auto remaining = std::max<long long>(0, ceil<seconds>(next->start - now).count());
    Countdown countdown;
    countdown.plugin = std::move(nextId);
    countdown.panel = "calendar";
    countdown.titleKey = "island.up-next.untitled";
    countdown.icon = "calendar-event";
    countdown.remaining = static_cast<int>(remaining);
    countdown.duration = minutes * 60;
    countdown.running = countdown.active = countdown.event = true;
    countdown.title = next->title;
    countdown.url = next->url;
    return countdown;
  }
} // namespace island
