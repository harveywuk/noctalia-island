#pragma once

#include "core/timer_manager.h"
#include "render/core/texture_handle.h"
#include "ui/visuals/artwork_flow.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Image;
class Renderer;

namespace control_center {

  // Shows the artwork flow in an Image inside a panel: decodes the artwork, keeps a small texture
  // updated in place and, while animating, advances it at about 30 fps. Uploads between frames go
  // through PanelManager::withRenderer, since update and layout rebuild an Island-hosted panel.
  class ArtworkFlowLayer {
  public:
    ArtworkFlowLayer() = default;
    ~ArtworkFlowLayer();
    ArtworkFlowLayer(const ArtworkFlowLayer&) = delete;
    ArtworkFlowLayer& operator=(const ArtworkFlowLayer&) = delete;

    // The Image the flow is drawn into; hidden while there is no artwork.
    void attach(Image* image);
    // Where frames are uploaded and redrawn between updates. Unset, that is PanelManager (an
    // Island-hosted or ordinary panel); another surface, such as the lock screen, passes its own.
    using WithRenderer = std::function<bool(const std::function<void(Renderer&)>&)>;
    void setHost(WithRenderer withRenderer, std::function<void()> requestRedraw);
    // Decodes `path` and shows its first frame. False (and cleared) when it cannot be read.
    bool load(Renderer& renderer, const std::string& path);
    void clear();
    [[nodiscard]] bool hasArtwork() const noexcept { return m_flow.hasArtwork(); }
    [[nodiscard]] visuals::ArtworkFlow::Accent accent() const noexcept { return m_flow.accent(); }
    // Animates while `animate` and motion is enabled; otherwise holds the current frame.
    void setAnimating(bool animate);
    // Frees the texture; call when the panel closes.
    void release();

  private:
    void upload(Renderer& renderer);
    bool withRenderer(const std::function<void(Renderer&)>& fn);
    void requestRedraw();

    visuals::ArtworkFlow m_flow;
    Image* m_image = nullptr;
    std::vector<std::uint8_t> m_frame;
    TextureHandle m_texture{};
    float m_seconds = 0.0F;
    std::chrono::steady_clock::time_point m_lastTick;
    Timer m_timer;
    WithRenderer m_withRenderer;
    std::function<void()> m_requestRedraw;
  };

} // namespace control_center
