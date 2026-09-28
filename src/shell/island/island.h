#pragma once

#include "core/timer_manager.h"
#include "notification/notification.h"
#include "shell/island/island_panel_surface.h"
#include "shell/island/island_state.h"
#include "shell/island/island_privacy.h"
#include "shell/osd/osd_overlay.h"

#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

class MprisService;
class SessionBus;
class DownloadProgressService;
class UPowerService;
class BluetoothService;
namespace island { struct Battery; }
namespace island { struct Countdown; }
class HttpClient;
class NotificationManager;
enum class NotificationEvent;
struct PointerEvent;
struct KeyboardEvent;
struct wl_output;

class Island {
public:
  Island();
  ~Island();
  void initialize(WaylandConnection&, ConfigService*, RenderContext*, MprisService*, NotificationManager*, HttpClient*, SessionBus*, UPowerService*, BluetoothService*, PipeWireService*);
  void onOutputChange();
  void onConfigReload();
  void refresh();
  bool showOsd(const OsdContent&);
  bool onNotification(const Notification&, NotificationEvent);
  bool onPointerEvent(const PointerEvent&);
  bool focusKeyboard();
  bool onKeyboardEvent(const KeyboardEvent&);
  void hideDndSuppressed();
  [[nodiscard]] bool enabled() const;
  [[nodiscard]] bool osdVisible() const;
  std::function<void(wl_output*, const std::string&)> openPanel;
  std::function<void()> closeHostedPanel;
  std::optional<IslandPanelSurface> acquirePanelSurface(wl_output* output, bool exactOutput = false);
  [[nodiscard]] island::Size panelReturnSize() const;
  void releasePanelSurface(wl_output* output, float width, float height);

private:
  struct Instance;
  void prepare(Instance&);
  void geometry(Instance&);
  void releaseKeyboard(Instance&);
  void dismissNotification();
  void destroySurfaces();
  std::vector<island::Battery> batteries() const;
  std::vector<island::PrivacyActivity> privacy() const;
  std::vector<island::Countdown> countdowns() const;
  void timerCommand(const island::Countdown&, const std::string& command);
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  MprisService* m_mpris = nullptr;
  NotificationManager* m_notifications = nullptr;
  HttpClient* m_http = nullptr;
  UPowerService* m_upower = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  PipeWireService* m_pipewire = nullptr;
  mutable island::PrivacySummary m_privacySummary;
  std::unique_ptr<DownloadProgressService> m_downloads;
  std::vector<std::unique_ptr<Instance>> m_instances;
  std::optional<Notification> m_notification;
  std::optional<OsdContent> m_osd;
  Timer m_tick;
  Timer m_osdTimeout;
  Timer m_announcementTimeout;
  std::string m_trackSignature;
  std::string m_announcement;
  std::unordered_set<std::string> m_pendingArtwork;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
};
