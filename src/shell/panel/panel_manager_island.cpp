#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "render/render_context.h"
#include "shell/island/island.h"
#include "shell/island/island_style.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/image.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/motion.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cmath>

bool PanelManager::openIslandPanel(wl_output* output, std::string_view sourceBarName) {
  if (!m_islandHost)
    return false;
  m_islandSurface = m_islandHost->acquirePanelSurface(output, false, sourceBarName);
  if (!m_islandSurface)
    return false;

  const auto& host = *m_islandSurface;
  m_output = host.output;
  m_surface = m_layerSurface = host.surface;
  m_wlSurface = m_surface->wlSurface();
  m_activePanel->setContentScale(host.scale);
  const auto* monitor = m_platform->findOutputByWl(m_output);
  const float maxWidth = monitor ? static_cast<float>(monitor->effectiveLogicalWidth() - 32) : 800;
  const float maxHeight = monitor ? static_cast<float>(monitor->effectiveLogicalHeight() - 32) : 600;
  m_panelVisualWidth = static_cast<std::uint32_t>(std::clamp(m_activePanel->islandWidth(maxWidth), 1.0F, maxWidth));
  m_panelVisualHeight = static_cast<std::uint32_t>(std::clamp(m_activePanel->preferredHeight(), 1.0F, maxHeight));
  m_islandCollapsedWidth = host.width;
  m_islandCollapsedHeight = host.height;
  m_islandWidth = host.width;
  m_islandHeight = host.height;
  m_islandProgress = 0;
  m_islandResizing = false;
  m_islandMorph = 0;
  m_islandSurfaceHeight = monitor ? static_cast<std::uint32_t>(monitor->effectiveLogicalHeight()) : 0;
  m_panelLayer = LayerShellLayer::Overlay;
  m_layerSurface->setLayer(m_panelLayer);
  const bool keyboard = m_activePanel->keyboardMode() != LayerShellKeyboard::None;
  if (keyboard && m_activePanel->dismissOnOutsideClick())
    activateClickShield(m_panelLayer);
  m_layerSurface->setKeyboardInteractivity(keyboard ? LayerShellKeyboard::Exclusive : LayerShellKeyboard::None);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_surface)
      m_surface->requestLayout();
  });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_surface->setFrameTickCallback([this](float dt) {
    if (m_activePanel)
      m_activePanel->onFrameTick(dt);
  });
  m_surface->requestUpdate();
  if (keyboard && m_activePanel->dismissOnOutsideClick()) {
    if (auto* grabs = m_platform->focusGrabService(); grabs && grabs->available()) {
      // Resizing and raising the borrowed Island can clear a grab created in
      // the same dispatch. Keep exclusive keyboard focus while it settles,
      // then commit OnDemand before creating the outside-click grab.
      const auto generation = m_destroyGeneration;
      m_keyboardRelaxTimer.start(std::chrono::milliseconds(100), [this, generation] {
        if (m_destroyGeneration == generation && m_islandSurface && !m_closing) {
          m_layerSurface->setKeyboardInteractivity(LayerShellKeyboard::OnDemand);
          activateFocusGrab();
        }
      });
    }
  }
  if (m_panelOpenedCallback)
    m_panelOpenedCallback();
  return true;
}

void PanelManager::buildIslandScene(std::uint32_t width, std::uint32_t height) {
  auto& renderer = m_surface->renderTarget().renderer();
  const bool first = !m_sceneRoot;
  if (first) {
    m_sceneRoot = std::make_unique<Node>();
    m_sceneRoot->setAnimationManager(&m_animations);
    m_selectPopup = std::make_unique<SelectDropdownPopup>(m_platform->wayland(), *m_renderContext);
    m_selectPopup->setShadowConfig(m_config->config().shell.shadow);
    m_selectPopup->setParent(m_layerSurface->layerSurface(), m_wlSurface, m_output);
    m_sceneRoot->setPopupContext(m_selectPopup.get());
    auto capsule = std::make_unique<Box>();
    capsule->setFill(colorSpecFromRole(ColorRole::Surface));
    capsule->setClipChildren(true);
    m_bgNode = m_sceneRoot->addChild(std::move(capsule));
    // While media plays the capsule shows the artwork gradient; keep it behind the card so opening
    // and closing don't flash a plain capsule.
    if (m_islandSurface->flow.id != 0) {
      auto flow = std::make_unique<Image>();
      flow->setFit(ImageFit::Cover);
      flow->setHitTestVisible(false);
      flow->setExternalTexture(renderer, m_islandSurface->flow);
      m_islandFlow = m_bgNode->addChild(std::move(flow));
    }
    auto content = std::make_unique<Node>();
    m_contentNode = m_bgNode->addChild(std::move(content));
    m_activePanel->setAnimationManager(&m_animations);
    const float glass = m_islandHost->capsuleOpacity();
    m_activePanel->setPanelCardOpacity(
        glass < 1.0F ? panelCardOpacityForTransparencyMode(PanelTransparencyMode::Glass, glass) : 1.0F
    );
    m_activePanel->create();
    m_activePanel->onOpen(m_pendingOpenContext);
    m_pendingOpenContext.clear();
    if (m_activePanel->root())
      m_contentNode->addChild(m_activePanel->releaseRoot());
    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    m_inputDispatcher.setTextInputContext(m_wlSurface, m_platform->wayland().textInputService());
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    m_inputDispatcher.setHoverChangeCallback([this](InputArea*, InputArea* next) {
      if (m_layerSurface)
        TooltipManager::instance().onHoverChange(next, m_layerSurface->layerSurface(), m_output);
    });
    m_inputDispatcher.setFocusChangeCallback([this](InputArea*, InputArea* next) {
      if (m_activePanel && next)
        m_activePanel->scrollFocusedInputIntoView(next);
    });
    m_surface->setSceneRoot(m_sceneRoot.get());
    if (auto* focus = m_activePanel->initialFocusArea())
      m_inputDispatcher.setFocus(focus);
  }
  m_sceneRoot->setSize(static_cast<float>(width), static_cast<float>(height));
  const float padding = Style::panelPadding * m_activePanel->contentScale();
  m_contentWidth = std::max(1.0F, static_cast<float>(m_panelVisualWidth) - 2 * padding);
  m_contentHeight = std::max(1.0F, static_cast<float>(m_panelVisualHeight) - 2 * padding);
  m_contentNode->setSize(m_contentWidth, m_contentHeight);
  m_activePanel->update(renderer);
  m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
  if (!m_closing) {
    // Bounded by the output, not the surface: the Island grows its surface to full height for a
    // panel, but the first layouts can run before that configure arrives.
    const auto* monitor = m_platform->findOutputByWl(m_output);
    const float limitWidth = monitor ? static_cast<float>(monitor->effectiveLogicalWidth()) : static_cast<float>(width);
    const float limitHeight =
        monitor ? static_cast<float>(monitor->effectiveLogicalHeight()) : static_cast<float>(height);
    const float maxHeight = std::max(1.0F, limitHeight - 32 * m_islandSurface->scale);
    const float maxWidth = std::max(1.0F, limitWidth - 32 * m_islandSurface->scale);
    const float targetWidth = std::round(std::clamp(m_activePanel->islandWidth(maxWidth), 1.0F, maxWidth));
    const float targetHeight = std::round(std::clamp(m_activePanel->islandHeight(maxHeight), 1.0F, maxHeight));
    if (first
        || std::abs(targetWidth - static_cast<float>(m_panelVisualWidth)) >= 2
        || std::abs(targetHeight - static_cast<float>(m_panelVisualHeight)) >= 2) {
      resizeIslandPanel(targetWidth, targetHeight, first);
      m_contentWidth = std::max(1.0F, targetWidth - 2 * padding);
      m_contentHeight = std::max(1.0F, targetHeight - 2 * padding);
      m_contentNode->setSize(m_contentWidth, m_contentHeight);
      m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
    }
  }
  applyPendingPanelFocus();
  applyIslandReveal(m_islandProgress);
}

void PanelManager::resizeIslandPanel(float width, float height, bool first) {
  m_animations.cancel(m_islandMorph);
  m_islandCollapsedWidth = m_islandWidth;
  m_islandCollapsedHeight = m_islandHeight;
  m_panelVisualWidth = static_cast<std::uint32_t>(width);
  m_panelVisualHeight = static_cast<std::uint32_t>(height);
  m_islandResizing = !first;
  m_islandProgress = 0;
  // The panel springs out of the Island, and between sizes, like the Island's own capsule.
  const bool growing = width > m_islandCollapsedWidth || height > m_islandCollapsedHeight;
  m_islandMorph = Motion::animateSpring(
      m_animations, 0.0F, 1.0F, first || growing ? Motion::panelOpen : Motion::panelClose,
      [this](float value) { applyIslandReveal(value); }, [this] { m_islandResizing = false; }, m_sceneRoot.get()
  );
}

void PanelManager::applyIslandReveal(float progress) {
  if (!m_islandSurface || !m_bgNode || !m_contentNode)
    return;
  m_islandProgress = progress;
  const float scale = m_islandSurface->scale;
  const float fromWidth = m_closing ? m_islandSurface->width : static_cast<float>(m_panelVisualWidth);
  const float fromHeight = m_closing ? m_islandSurface->height : static_cast<float>(m_panelVisualHeight);
  m_islandWidth = m_islandCollapsedWidth + (fromWidth - m_islandCollapsedWidth) * progress;
  m_islandHeight = m_islandCollapsedHeight + (fromHeight - m_islandCollapsedHeight) * progress;
  const float x = (static_cast<float>(m_surface->width()) - m_islandWidth) / 2;
  const float y = 8 * scale;
  m_bgNode->setPosition(x, y);
  m_bgNode->setSize(m_islandWidth, m_islandHeight);
  auto* capsule = static_cast<Box*>(m_bgNode);
  const float radius = island::surfaceRadius(m_islandHeight, scale);
  capsule->setRadius(radius);
  if (auto* flow = static_cast<Image*>(m_islandFlow)) {
    flow->setPosition(0, 0);
    flow->setSize(m_islandWidth, m_islandHeight);
    flow->setRadius(radius);
  }
  // A glass Island stays glass while it hosts a panel: the same tint, and the morphing card's
  // shape as the blur region (hyprglass glasses exactly this).
  const float glass = m_islandHost->capsuleOpacity();
  const auto glassFill = [glass](Color color) {
    color.a *= glass;
    return color;
  };
  // The card keeps the Island's own colour while it is still pill-sized and takes the panel
  // surface as it grows, so opening and closing read as one capsule changing shape rather than
  // a light card snapping onto a black pill. A resize between two panel sizes stays on the surface.
  if (m_islandResizing) {
    capsule->setFill(colorSpecFromRole(ColorRole::Surface, glass));
  } else {
    // Closing returns to the capsule colour by mid-travel, so the pill never lands light.
    const float blend = m_closing ? std::clamp((progress - 0.45F) / 0.4F, 0.0F, 1.0F)
                                  : std::clamp((progress - 0.2F) / 0.55F, 0.0F, 1.0F);
    capsule->setFill(glassFill(
        lerpColor(m_islandHost->capsuleColor(), colorForRole(ColorRole::Surface), blend * blend * (3.0F - 2.0F * blend))
    ));
    // The gradient fades as the card takes the panel's colour, and returns as it closes to a pill.
    if (m_islandFlow)
      m_islandFlow->setOpacity(1.0F - blend * blend * (3.0F - 2.0F * blend));
  }
  if (m_islandFlow && m_islandResizing)
    m_islandFlow->setOpacity(0.0F);
  if (glass < 1.0F)
    m_surface->setBlurRegion(
        Surface::tessellateRoundedRect(
            static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
            static_cast<int>(std::lround(m_islandWidth)), static_cast<int>(std::lround(m_islandHeight)), radius
        )
    );
  const float padding = Style::panelPadding * m_activePanel->contentScale();
  m_contentNode->setPosition((m_islandWidth - static_cast<float>(m_panelVisualWidth)) / 2 + padding, padding);
  if (!m_closing)
    m_contentNode->setOpacity(m_islandResizing ? 1.0F : std::clamp((progress - 0.65F) / 0.35F, 0.0F, 1.0F));
  m_surface->setInputRegion({InputRect{
      static_cast<int>(std::floor(x)), 0, static_cast<int>(std::ceil(m_islandWidth)),
      static_cast<int>(std::ceil(y + m_islandHeight))
  }});
  if (const auto* output = m_platform->findOutputByWl(m_output)) {
    m_panelOutputInputRect = InputRect{
        static_cast<int>((static_cast<float>(output->effectiveLogicalWidth()) - m_islandWidth) / 2), 0,
        static_cast<int>(std::ceil(m_islandWidth)), static_cast<int>(std::ceil(y + m_islandHeight))
    };
    m_clickShield.setPanelInputRect(m_output, *m_panelOutputInputRect);
  }
  fitIslandSurface();
  // Resizing can move navigation away from a stationary pointer. Clear stale
  // hover/tooltip state using the new scene coordinates on every morph frame.
  if (m_pointerInside && !m_closing)
    m_inputDispatcher.syncPointerHover();
  m_surface->requestRedraw();
}

void PanelManager::fitIslandSurface() {
  // The Island lends its surface at the output's full height. Trim it to the panel (or the
  // capsule, while that is still taller): compositor effects such as hyprglass's layer glass cost
  // per pixel of the layer. Grow at once; shrink only once the panel has settled open.
  if (!m_islandSurface || !m_layerSurface || m_closing)
    return;
  const auto* monitor = m_platform->findOutputByWl(m_output);
  if (!monitor)
    return;
  const float scale = m_islandSurface->scale;
  const float tall = std::max({static_cast<float>(m_panelVisualHeight), m_islandHeight, m_islandCollapsedHeight});
  const auto max = static_cast<std::uint32_t>(monitor->effectiveLogicalHeight());
  const auto want = std::min(max, static_cast<std::uint32_t>(std::ceil((32 * scale + tall) / 64.0F)) * 64U);
  const bool settled = !m_islandResizing && m_islandProgress >= 0.999F;
  if (want > m_islandSurfaceHeight || (settled && want < m_islandSurfaceHeight)) {
    m_islandSurfaceHeight = want;
    m_layerSurface->requestSize(static_cast<std::uint32_t>(m_surface->width()), want);
  }
}
