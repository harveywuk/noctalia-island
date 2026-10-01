#pragma once

#include "shell/island/island_preview_target.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace island {
  // Event timestamps are shared; each bar/monitor applies its own durations.
  // Refreshes and config reloads never restart the elapsed time.
  class MediaActivity {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void update(std::string_view track, std::string_view status, TimePoint now, std::string_view focusedOutput = {}) {
      const bool playing = !track.empty() && status == "Playing";
      if (track != m_track) {
        m_target.output = focusedOutput;
        m_preview = playing ? std::optional(now) : std::nullopt;
        m_pause.reset();
      } else if (m_playing && status == "Paused") {
        m_pause = now;
      }
      if (!playing)
        m_preview.reset();
      if (track.empty() || status != "Paused")
        m_pause.reset();
      m_track = track;
      m_playing = playing;
    }

    bool announcing(TimePoint now, int seconds = 5) const {
      return m_preview && now < *m_preview + std::chrono::seconds(seconds);
    }
    void reconcileOutputs(const std::vector<std::string>& available, std::string_view focused) {
      m_target.reconcile(available, focused);
    }
    bool targets(std::string_view setting, std::string_view output) const { return m_target.matches(setting, output); }
    bool compact(TimePoint now, int seconds = 3) const {
      return m_playing || (m_pause && now < *m_pause + std::chrono::seconds(seconds));
    }
    std::optional<TimePoint> nextExpiry(TimePoint now, int previewSeconds = 5, int pauseSeconds = 3) const {
      if (announcing(now, previewSeconds))
        return *m_preview + std::chrono::seconds(previewSeconds);
      if (m_pause && compact(now, pauseSeconds))
        return *m_pause + std::chrono::seconds(pauseSeconds);
      return std::nullopt;
    }

  private:
    PreviewTarget m_target;
    std::string m_track;
    bool m_playing = false;
    std::optional<TimePoint> m_preview;
    std::optional<TimePoint> m_pause;
  };
} // namespace island
