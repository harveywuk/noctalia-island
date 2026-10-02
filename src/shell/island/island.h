#pragma once

#include "core/timer_manager.h"
#include "notification/notification.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/island/island_media.h"
#include "shell/island/island_panel_surface.h"
#include "shell/island/island_privacy.h"
#include "shell/island/island_state.h"
#include "shell/osd/osd_overlay.h"
#include "ui/visuals/artwork_flow.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

class MprisService;
class CompositorPlatform;
class WidgetFactory;
class IpcService;
struct BarServices;
class PipeWireSpectrum;
class SessionBus;
class DownloadProgressService;
class UPowerService;
class BluetoothService;
namespace island {
  struct Battery;
  class BatteryConnections;
} // namespace island
namespace island {
  struct Countdown;
}
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
  void initialize(
      WaylandConnection&, ConfigService*, RenderContext*, MprisService*, NotificationManager*, HttpClient*, SessionBus*,
      UPowerService*, BluetoothService*, PipeWireService*, PipeWireSpectrum*
  );
  void initializeWidgets(const BarServices&, IpcService*);
  void onOutputChange();
  void onConfigReload();
  void refresh();
  void onWorkspaceChanged();
  bool showOsd(const OsdContent&);
  bool onNotification(const Notification&, NotificationEvent);
  bool onPointerEvent(const PointerEvent&);
  bool focusKeyboard();
  bool onKeyboardEvent(const KeyboardEvent&);
  void hideDndSuppressed();
  [[nodiscard]] bool enabled() const;
  [[nodiscard]] bool osdVisible() const;
  std::function<void(wl_output*, const std::string&)> openPanel;
  // Raises a window of the first app found among lower-case executable names; false if none.
  std::function<bool(const std::vector<std::string>&)> focusApp;
  // True while a menu opened from the Island is showing; the Island stays expanded meanwhile.
  std::function<bool()> holdExpanded;
  std::function<void()> closeHostedPanel;
  std::optional<IslandPanelSurface>
  acquirePanelSurface(wl_output* output, bool exactOutput = false, std::string_view barName = {});
  [[nodiscard]] island::Size panelReturnSize() const;
  void releasePanelSurface(wl_output* output, float width, float height);

private:
  struct Instance;
  void prepare(Instance&);
  void geometry(Instance&);
  void updateVisibility(Instance&);
  bool trackPreview(const IslandConfig&, wl_output*) const;
  void releaseKeyboard(Instance&);
  // The flowing artwork gradient behind the capsule while media plays (Cupertino look).
  void showFlow(Instance&, bool show);
  void tickFlow();
  void releaseFlow(Instance&);
  void collapseAfterLeave(Instance&, std::chrono::milliseconds delay);
  void dismissNotification();
  void updateNotificationPreview();
  void destroySurfaces();
  std::vector<island::Battery> batteries(const IslandConfig&, wl_output*) const;
  std::vector<island::PrivacyActivity> privacy() const;
  std::vector<island::Countdown> countdowns() const;
  void timerCommand(const island::Countdown&, const std::string& command);
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  CompositorPlatform* m_platform = nullptr;
  RenderContext* m_renderContext = nullptr;
  MprisService* m_mpris = nullptr;
  NotificationManager* m_notifications = nullptr;
  HttpClient* m_http = nullptr;
  UPowerService* m_upower = nullptr;
  BluetoothService* m_bluetooth = nullptr;
  PipeWireService* m_pipewire = nullptr;
  PipeWireSpectrum* m_spectrum = nullptr;
  mutable island::PrivacySummary m_privacySummary;
  std::unique_ptr<DownloadProgressService> m_downloads;
  std::unique_ptr<WidgetFactory> m_widgetFactory;
  noctalia::bar::WidgetActionDispatcher m_widgetActions;
  std::vector<std::unique_ptr<Instance>> m_instances;
  std::optional<Notification> m_notification;
  std::vector<Notification> m_urgentNotifications;
  std::optional<TimePoint> m_notificationDeadline;
  Timer m_notificationPreviewTimer;
  std::optional<OsdContent> m_osd;
  Timer m_tick;
  visuals::ArtworkFlow m_flow;
  std::string m_flowArt;
  std::vector<std::uint8_t> m_flowFrame;
  Timer m_flowTimer;
  std::chrono::steady_clock::time_point m_flowStart = std::chrono::steady_clock::now();
  std::unique_ptr<island::BatteryConnections> m_batteryConnections;
  Timer m_batteryTimeout;
  Timer m_osdTimeout;
  island::MediaActivity m_mediaActivity;
  Timer m_mediaTimeout;
  std::string m_trackSignature;
  std::unordered_set<std::string> m_pendingArtwork;
  std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
};
