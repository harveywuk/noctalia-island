#pragma once

#include "render/animation/animation_manager.h"
#include "render/core/texture_handle.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/desktop/desktop_widget_actions.h"
#include "shell/desktop/desktop_widget_details_popup.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widgets_controller.h"
#include "shell/desktop/wallpaper_mask.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/layer_surface.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ConfigService;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct WaylandOutput;
struct wl_output;
class SharedTextureCache;

class DesktopWidgetsHost {
public:
  DesktopWidgetsHost() = default;
  ~DesktopWidgetsHost();

  void initialize(const DesktopWidgetServices& services);
  void show(const DesktopWidgetsSnapshot& snapshot);
  void hide();
  void rebuild(const DesktopWidgetsSnapshot& snapshot);
  // Destroy and recreate every plugin-backed instance so its Luau runtime is re-seeded.
  // Needed because plugin-level settings live outside the widget snapshot, so syncInstances()
  // cannot see them change.
  void reloadPluginWidgets();
  void onOutputChange();
  void onSecondTick();
  void setConfigureRequestedCallback(std::function<void(const std::string&)> callback) {
    m_configureRequested = std::move(callback);
  }
  void setQuickActionCallback(std::function<void(const std::string&, desktop_widgets::QuickAction)> callback) {
    m_quickAction = std::move(callback);
  }
  void setHistoryAvailability(std::function<std::pair<bool, bool>()> callback) {
    m_historyAvailability = std::move(callback);
  }
  void requestUpdate();
  void requestLayout();
  void requestRedraw();
  void setWindowFocused(bool focused);
  void setWallpaperMasks(const OutputWallpaperMaskMap& masks);
  bool onPointerEvent(const PointerEvent& event);
  bool onKeyboardEvent(const KeyboardEvent& event);

private:
  struct DesktopWidgetInstance {
    DesktopWidgetState state;
    std::vector<DesktopWidgetState> stackCards;
    std::string effectiveOutputName;
    wl_output* output = nullptr;
    std::unique_ptr<LayerSurface> surface;
    AnimationManager animations;
    InputDispatcher inputDispatcher;
    std::unique_ptr<Node> sceneRoot;
    Node* transformNode = nullptr;
    std::unique_ptr<DesktopWidget> widget;
    bool pointerInside = false;
    float blendTarget = 1.0F;
    AnimationManager::Id blendAnimation = 0;
    float intrinsicWidth = 0.0F;
    float intrinsicHeight = 0.0F;
  };

  struct LoadedWallpaperMask {
    OutputWallpaperMask descriptor;
    TextureHandle retainedTexture;
  };

  void syncInstances();
  void openQuickMenu(DesktopWidgetInstance& instance, const PointerEvent& event);
  void openDetails(DesktopWidgetInstance& instance, DesktopWidgetDetailsRequest request, std::uint32_t serial);
  void updateBlending(DesktopWidgetInstance& instance, bool animate = true);
  void createInstance(const DesktopWidgetState& state, const WaylandOutput& output);
  void buildScene(DesktopWidgetInstance& instance);
  void prepareFrame(DesktopWidgetInstance& instance, bool needsUpdate, bool needsLayout);
  [[nodiscard]] DesktopWidgetInstance* findInstance(const std::string& id);
  void releaseWallpaperMasks();
  void updateWallpaperMask(DesktopWidgetInstance& instance);

  std::function<void(const std::string&)> m_configureRequested;
  std::function<void(const std::string&, desktop_widgets::QuickAction)> m_quickAction;
  std::function<std::pair<bool, bool>()> m_historyAvailability;
  MprisService* m_mpris = nullptr;
  WaylandConnection* m_wayland = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  SharedTextureCache* m_textureCache = nullptr;
  std::unique_ptr<DesktopWidgetFactory> m_factory;
  DesktopWidgetsSnapshot m_snapshot;
  bool m_visible = false;
  bool m_windowFocused = false;
  std::vector<std::unique_ptr<DesktopWidgetInstance>> m_instances;
  std::unique_ptr<ContextMenuPopup> m_quickMenu;
  std::string m_menuWidgetId;
  std::unique_ptr<DesktopWidgetDetailsPopup> m_details;
  std::string m_detailsWidgetId;
  std::optional<std::pair<std::string, DesktopWidgetDetailsRequest>> m_pendingDetails;
  std::unordered_map<std::string, LoadedWallpaperMask> m_wallpaperMasks;
};
