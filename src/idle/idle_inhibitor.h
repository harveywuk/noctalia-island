#pragma once

#include "core/timer_manager.h"

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

class IpcService;
class LogindService;
class WaylandConnection;
struct wl_surface;
struct zwp_idle_inhibit_manager_v1;
struct zwp_idle_inhibitor_v1;

class IdleInhibitor {
public:
  using ChangeCallback = std::function<void()>;
  using StateFeedbackCallback = std::function<void(bool enabled)>;
  using AnchorSurfacesProvider = std::function<std::vector<wl_surface*>()>;

  IdleInhibitor();
  ~IdleInhibitor();

  IdleInhibitor(const IdleInhibitor&) = delete;
  IdleInhibitor& operator=(const IdleInhibitor&) = delete;

  bool initialize(WaylandConnection& wayland);
  void setLogindService(LogindService* logind);
  void setAnchorSurfacesProvider(AnchorSurfacesProvider provider);
  void toggle();
  void setEnabled(bool enabled);
  void setEnabledFor(std::chrono::milliseconds duration);
  // Adds to a running deadline without restarting an ended or indefinite session.
  bool extendTimed(std::chrono::milliseconds duration);
  [[nodiscard]] std::optional<std::chrono::seconds> remaining() const;
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool available() const noexcept;
  void setChangeCallback(ChangeCallback callback);

  void registerIpc(IpcService& ipc, StateFeedbackCallback stateFeedback = {});
  /// Re-resolve Wayland inhibitor anchors when output topology changes.
  void onOutputChange();
  /// Re-sync inhibitors (Wayland fallback anchors only when logind is unavailable).
  void resyncAnchorSurfaces();

private:
  void syncInhibitor(bool logTransitions = true);
  void syncWaylandInhibitors(bool logTransitions);
  void syncLogindInhibit(bool logTransitions);
  void destroyWaylandInhibitors(bool logDisable = false);
  void releaseLogindInhibit();
  void notifyChanged();

  WaylandConnection* m_wayland = nullptr;
  LogindService* m_logind = nullptr;
  zwp_idle_inhibit_manager_v1* m_manager = nullptr;
  std::unordered_map<wl_surface*, zwp_idle_inhibitor_v1*> m_inhibitors;
  AnchorSurfacesProvider m_anchorSurfacesProvider;
  ChangeCallback m_changeCallback;
  bool m_enabled = false;
  Timer m_expiryTimer;
  std::optional<std::chrono::steady_clock::time_point> m_deadline;
  bool m_loggedWaylandEnable = false;
  bool m_loggedLogindEnable = false;
};
