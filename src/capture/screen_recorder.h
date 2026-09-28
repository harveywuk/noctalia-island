#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

// Main-loop owned recorder. Only the child launched here can be stopped.
class ScreenRecorder {
public:
  static ScreenRecorder& instance();
  std::string start(const std::string& output, const std::string& geometry = {});
  void stop();
  void shutdown();
  bool active() const { return m_pid > 0; }
  std::string label() const;
  std::function<void(bool, const std::string&)> completed;

private:
  void poll();
  int m_pid = -1;
  bool m_stopping = false;
  Timer m_poll;
  std::chrono::steady_clock::time_point m_started, m_stopRequested;
  std::filesystem::path m_path, m_log;
};
