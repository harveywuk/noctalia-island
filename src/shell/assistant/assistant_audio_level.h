#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// An envelope for this voice session only, independent of desktop/media audio.
class AssistantAudioLevel {
public:
  void capture(std::span<const float> samples);
  bool loadPlayback(const std::filesystem::path& file);
  void playbackEvent(std::string_view chunk);
  [[nodiscard]] float level() const;

private:
  using Clock = std::chrono::steady_clock;
  static float amplitude(std::span<const float> samples);
  std::atomic<float> m_inputLevel{0};
  std::atomic<Clock::duration::rep> m_inputAt{0};
  std::vector<float> m_envelope;
  mutable std::mutex m_mutex;
  std::string m_pending;
  Clock::time_point m_started{};
  std::chrono::duration<double> m_elapsed{};
  bool m_running = false;
};
