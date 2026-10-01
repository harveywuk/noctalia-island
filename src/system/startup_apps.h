#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A login's launch plan is captured once and kept in XDG_RUNTIME_DIR. Claims are
// persisted before spawning, so restarts cannot repeat an already attempted launch.
class StartupApps {
public:
  using Launch = std::function<bool(const StartupAppConfig&)>;
  using Clock = std::function<std::int64_t()>;
  explicit StartupApps(Launch launch = {}, Clock clock = {});
  ~StartupApps();
  StartupApps(const StartupApps&) = delete;
  StartupApps& operator=(const StartupApps&) = delete;

  // stateFile is injectable for isolated tests; production derives it from the
  // current Wayland socket's identity, which changes for each compositor session.
  bool start(const std::vector<StartupAppConfig>& entries, std::filesystem::path stateFile = {});
  static std::filesystem::path sessionStateFile();
  static std::string problem(const StartupAppConfig& entry);

private:
  bool save();
  void dispatchDue();
  Launch m_launch;
  Clock m_clock;
  Timer m_timer;
  int m_lock = -1;
  std::filesystem::path m_stateFile;
  nlohmann::json m_plan;
};
