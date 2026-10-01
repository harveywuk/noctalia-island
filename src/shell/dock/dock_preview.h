#pragma once

#include "capture/toplevel_thumbnail_capture.h"
#include "shell/dock/dock_context_menu.h"
#include "wayland/wayland_toplevels.h"

#include <optional>
#include <string>

class Image;

namespace shell::dock {

  struct DockPreviewCallbacks {
    std::function<void(const ToplevelInfo&)> activateWindow;
    std::function<void(const ToplevelInfo&)> closeWindow;
    std::function<void()> dismiss;
  };

  // A non-grabbing hover popup. Only the visible page owns live capture sessions.
  class DockPreview : public DockPopup {
  public:
    DockPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext);
    ~DockPreview();
    bool initialize(
        zwlr_layer_surface_v1* parent, wl_output* output, const DesktopEntry& entry, std::vector<ToplevelInfo> windows,
        const DockPreviewCallbacks& callbacks
    );
    void refreshWindows(std::vector<ToplevelInfo> current);

  private:
    void buildScene();
    void setPage(std::size_t page);
    void startCaptures();
    void updateImages();

    CompositorPlatform& m_platform;
    ConfigService& m_config;
    RenderContext& m_renderContext;
    std::vector<std::unique_ptr<ToplevelThumbnailCapture>> m_captures;
    DockPreviewCallbacks m_callbacks;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::string m_appName;
    std::string m_iconPath;
    std::size_t m_columns = 1;
    std::size_t m_pageSize = 1;
    std::size_t m_page = 0;
    float m_cardWidth = 220;
    float m_cardHeight = 158;
    float m_uiScale = 1;
    bool m_dirty = true;
    std::vector<std::optional<ScreencopyImage>> m_images;
    std::vector<bool> m_imageDirty;
    std::vector<Image*> m_imageNodes;
    std::vector<Node*> m_fallbackNodes;
  };

  [[nodiscard]] bool samePreviewWindow(const ToplevelInfo& a, const ToplevelInfo& b);

} // namespace shell::dock
