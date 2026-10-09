#pragma once

#include "capture/capture_options.h"
#include "core/timer_manager.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

// Main-loop owned recorder. Only the child launched here can be stopped.
class ScreenRecorder {
public:
  struct Result {
    bool success = false;
    std::filesystem::path path;
    std::chrono::seconds duration{};
    std::uintmax_t bytes = 0;
    std::string error;
  };
  static ScreenRecorder& instance();
  std::string start(
      const std::string& output, const std::string& geometry = {},
      capture::RecordingAudio audio = capture::RecordingAudio::Desktop
  );
  void stop();
  void shutdown();
  bool active() const { return m_pid > 0; }
  bool stopping() const { return active() && m_stopping; }
  std::uint64_t sessionId() const { return m_sessionId; }
  std::chrono::steady_clock::time_point startedAt() const { return m_started; }
  std::chrono::seconds elapsed() const;
  std::string label() const;
  std::function<void(const Result&)> completed;
  std::function<void(bool)> activeChanged;

private:
  void poll();
  int m_pid = -1;
  bool m_stopping = false;
  std::uint64_t m_sessionId = 0;
  Timer m_poll;
  std::chrono::steady_clock::time_point m_started, m_stopRequested;
  std::filesystem::path m_path, m_log;
};
