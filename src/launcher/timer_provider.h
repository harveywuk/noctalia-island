#pragma once

#include "core/timer_manager.h"
#include "launcher/launcher_provider.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class IpcService;

// Timers in the launcher: "timer 10m", "15 min timer", "set timer 1h30 tea". A running timer
// counts down as a live activity in the Dynamic Island (a ring that empties), and a notification
// says when it is done. Running timers are listed under the prefix with a Cancel action.
class TimerProvider : public LauncherProvider {
public:
  struct Request {
    std::chrono::seconds duration{0};
    std::string label;
  };

  explicit TimerProvider(IpcService* ipc) : m_ipc(ipc) {}
  ~TimerProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "timer"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "Timer"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "hourglass"; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;
  [[nodiscard]] std::vector<LauncherAction> actions(const LauncherResult& result) const override;
  LauncherActionOutcome runAction(const LauncherResult& result, std::string_view actionId) override;

  // "10m", "1h30", "15 min tea", "timer 25 minutes focus" → duration and label. `prefixed` is true
  // after the provider's prefix, where the word "timer" is not required. Exposed for tests.
  [[nodiscard]] static std::optional<Request> parse(std::string_view text, bool prefixed);
  // "1:05:00", "4:59", "0:07".
  [[nodiscard]] static std::string formatRemaining(std::chrono::seconds remaining);
  // "10 minutes", "1 hour 30 minutes", "45 seconds".
  [[nodiscard]] static std::string formatDuration(std::chrono::seconds duration);

private:
  struct Running {
    std::string id;
    std::string label;
    std::chrono::seconds total{0};
    std::chrono::steady_clock::time_point end;
    std::unique_ptr<Timer> tick;
  };

  void start(const Request& request);
  void tick(const std::string& id);
  void finish(const std::string& id);
  void cancel(const std::string& id);
  [[nodiscard]] std::string title(const Running& running) const;
  [[nodiscard]] std::vector<LauncherResult> runningResults() const;

  IpcService* m_ipc = nullptr;
  std::vector<Running> m_running;
  int m_nextId = 1;
};
