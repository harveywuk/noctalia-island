#pragma once

#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/desktop/desktop_battery_model.h"
#include "shell/desktop/desktop_widget_details_request.h"
#include "shell/desktop/desktop_widget_services.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"

#include <functional>
#include <memory>

class PopupSurface;
class WeatherTab;
class Flex;
class ScrollView;
class Label;
struct PointerEvent;
struct KeyboardEvent;

// A transient, grabbing popup owned by the desktop host. The parent widget stays
// alive until close(), including when a monitor or the desktop layout changes.
class DesktopWidgetDetailsPopup {
public:
  explicit DesktopWidgetDetailsPopup(const DesktopWidgetServices& services);
  ~DesktopWidgetDetailsPopup();
  void
  open(DesktopWidgetDetailsRequest request, PopupSurfaceParent parent, PopupAnchorRect anchor, std::uint32_t serial);
  void close();
  [[nodiscard]] bool isOpen() const { return m_surface != nullptr; }
  void setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);
  void requestUpdate();

private:
  void deferClose();
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void buildScene();
  void changeDay(int delta);
  void rebuildBatteries();
  void invalidate();

  DesktopWidgetServices m_services;
  DesktopWidgetDetailsRequest m_request;
  PopupSurfaceParent m_parent;
  std::unique_ptr<PopupSurface> m_surface;
  AnimationManager m_animations;
  InputDispatcher m_input;
  std::unique_ptr<Node> m_root;
  std::unique_ptr<WeatherTab> m_weather;
  popup_chrome::Geometry m_chrome;
  Flex* m_heading = nullptr;
  Flex* m_dayNavigation = nullptr;
  ScrollView* m_scroll = nullptr;
  Label* m_date = nullptr;
  calendar_view::EventListState m_events;
  bool m_calendarDirty = true;
  std::optional<std::vector<desktop_batteries::Device>> m_batteries;
  bool m_compact = false;
  float m_scale = 1;
  std::function<void()> m_onDismissed;
  // Renewed for each opening so a deferred dismissal cannot close a newer popup.
  std::shared_ptr<bool> m_openToken;
  std::uint64_t m_calendarCallback = 0;
};
