#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "render/render_context.h"
#include "shell/island/island.h"
#include "shell/island/island_style.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/controls/box.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/motion.h"
#include "ui/palette.h"
#include "ui/style.h"
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
    activateFocusGrab();
    if (m_focusGrab) {
      // Match ordinary panels: initially take keyboard focus, then allow
      // Hyprland's grab to detect outside clicks once the layer has settled.
      const auto generation = m_destroyGeneration;
      m_keyboardRelaxTimer.start(std::chrono::milliseconds(100), [this, generation] {
        if (m_destroyGeneration == generation && m_islandSurface && !m_closing)
          applyKeyboardRelaxation(LayerShellKeyboard::OnDemand);
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
    auto content = std::make_unique<Node>();
    m_contentNode = m_bgNode->addChild(std::move(content));
    m_activePanel->setAnimationManager(&m_animations);
    m_activePanel->setPanelCardOpacity(1.0F);
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
    const float maxHeight = std::max(1.0F, static_cast<float>(height) - 32 * m_islandSurface->scale);
    const float maxWidth = std::max(1.0F, static_cast<float>(width) - 32 * m_islandSurface->scale);
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
  static_cast<Box*>(m_bgNode)->setRadius(island::surfaceRadius(m_islandHeight, scale));
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
  // Resizing can move navigation away from a stationary pointer. Clear stale
  // hover/tooltip state using the new scene coordinates on every morph frame.
  if (m_pointerInside && !m_closing)
    m_inputDispatcher.syncPointerHover();
  m_surface->requestRedraw();
}
