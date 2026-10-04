#include "shell/control_center/artwork_flow_layer.h"

#include "render/animation/motion_service.h"
#include "render/core/image_file_loader.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/panel/panel_manager.h"
#include "ui/controls/image.h"

namespace control_center {

  namespace {
    // The flow moves slowly; about 30 fps is plenty.
    constexpr auto kFrameInterval = std::chrono::milliseconds(33);
    // Decoded this small, the artwork is a palette of colour regions rather than detail.
    constexpr int kSourceSize = 32;
  } // namespace

  ArtworkFlowLayer::~ArtworkFlowLayer() { m_timer.stop(); }

  void ArtworkFlowLayer::setHost(WithRenderer withRenderer, std::function<void()> requestRedraw) {
    m_withRenderer = std::move(withRenderer);
    m_requestRedraw = std::move(requestRedraw);
  }

  bool ArtworkFlowLayer::withRenderer(const std::function<void(Renderer&)>& fn) {
    return m_withRenderer ? m_withRenderer(fn) : PanelManager::instance().withRenderer(fn);
  }

  void ArtworkFlowLayer::requestRedraw() {
    if (m_requestRedraw)
      m_requestRedraw();
    else
      PanelManager::instance().requestRedraw();
  }

  void ArtworkFlowLayer::attach(Image* image) {
    m_image = image;
    if (m_image != nullptr)
      m_image->setVisible(false);
  }

  bool ArtworkFlowLayer::load(Renderer& renderer, const std::string& path) {
    auto art = loadImageFile(path, kSourceSize, true);
    if (!art || !m_flow.setArtwork(art->rgba, art->width, art->height)) {
      clear();
      return false;
    }
    upload(renderer);
    return true;
  }

  void ArtworkFlowLayer::clear() {
    m_flow.clear();
    m_timer.stop();
    if (m_image != nullptr)
      m_image->setVisible(false);
  }

  void ArtworkFlowLayer::setAnimating(bool animate) {
    if (!animate || !m_flow.hasArtwork() || !MotionService::instance().enabled() || visuals::ArtworkFlow::frozen()) {
      m_timer.stop();
      return;
    }
    if (m_timer.active())
      return;
    m_lastTick = std::chrono::steady_clock::now();
    m_timer.startRepeating(kFrameInterval, [this]() {
      const auto now = std::chrono::steady_clock::now();
      m_seconds += std::chrono::duration<float>(now - m_lastTick).count();
      m_lastTick = now;
      if (withRenderer([this](Renderer& renderer) { upload(renderer); }))
        requestRedraw();
    });
  }

  void ArtworkFlowLayer::release() {
    m_timer.stop();
    if (m_texture.id != 0
        && !withRenderer([this](Renderer& renderer) {
             renderer.textureManager().unload(m_texture);
           }))
      m_texture = {};
    m_flow.clear();
    m_image = nullptr;
  }

  void ArtworkFlowLayer::upload(Renderer& renderer) {
    if (m_image == nullptr || !m_flow.hasArtwork())
      return;
    m_flow.render(m_seconds, m_frame);
    auto& textures = renderer.textureManager();
    if (m_texture.id == 0)
      m_texture =
          textures.createEmpty(visuals::ArtworkFlow::kWidth, visuals::ArtworkFlow::kHeight, TextureDataFormat::Rgba);
    if (m_texture.id == 0
        || !textures.updateSubImage(
            m_texture, m_frame.data(), 0, 0, visuals::ArtworkFlow::kWidth, visuals::ArtworkFlow::kHeight,
            TextureDataFormat::Rgba
        ))
      return;
    m_image->setExternalTexture(renderer, m_texture);
    m_image->setVisible(true);
    m_image->markPaintDirty();
  }

} // namespace control_center
