#include "shell/dock/dock_preview.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/core/texture_manager.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "system/desktop_entry.h"
#include "system/icon_resolver.h"
#include "ui/builders.h"
#include "ui/motion.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace shell::dock {

  bool samePreviewWindow(const ToplevelInfo& a, const ToplevelInfo& b) {
    // wlr identifiers are app-id + title, so two different windows can share
    // them. Use live handle generations before compositor-assigned identity.
    if (a.handle && b.handle)
      return a.handle == b.handle && a.order == b.order;
    if (a.extHandle && b.extHandle)
      return a.extHandle == b.extHandle && a.identifier == b.identifier;
    return a.exactIdentity && b.exactIdentity && !a.identifier.empty() && a.identifier == b.identifier;
  }

  DockPreview::DockPreview(CompositorPlatform& platform, ConfigService& config, RenderContext& renderContext)
      : m_platform(platform), m_config(config), m_renderContext(renderContext) {}

  DockPreview::~DockPreview() {
    *m_alive = false;
    m_captures.clear();
  }

  bool DockPreview::initialize(
      zwlr_layer_surface_v1* parent, wl_output* output, const DesktopEntry& entry, std::vector<ToplevelInfo> current,
      const DockPreviewCallbacks& callbacks
  ) {
    if (!parent || current.empty())
      return false;
    windows = std::move(current);
    m_callbacks = callbacks;
    m_appName = entry.name.empty() ? entry.id : entry.name;
    IconResolver resolver;
    m_iconPath = resolver.resolve(entry.icon, 64);
    m_uiScale = m_config.config().accessibility.uiScale;
    const auto* monitor = m_platform.wayland().findOutputByWl(output);
    const float availableWidth = monitor ? static_cast<float>(monitor->effectiveLogicalWidth()) : 1280;
    const float availableHeight = monitor ? static_cast<float>(monitor->effectiveLogicalHeight()) : 720;
    const float pad = 12 * m_uiScale;
    const float gap = 8 * m_uiScale;
    m_cardWidth = std::min(220 * m_uiScale, std::max(100.0F, availableWidth - 96 - 2 * pad));
    m_cardHeight = std::min(158 * m_uiScale, std::max(90.0F, availableHeight - 200 * m_uiScale));
    m_columns = std::clamp(
        static_cast<std::size_t>(
            std::max(1.0F, std::floor((availableWidth - 96 - 2 * pad + gap) / (m_cardWidth + gap)))
        ),
        std::size_t{1}, std::size_t{3}
    );
    m_columns = std::min(m_columns, windows.size());
    const auto rows = static_cast<std::size_t>(
        std::clamp(std::floor((availableHeight - 200 * m_uiScale) / (m_cardHeight + gap)), 1.0F, 2.0F)
    );
    m_pageSize = m_columns * rows;
    const auto shownRows = std::min(rows, (windows.size() + m_columns - 1) / m_columns);
    const float width = 2 * pad + static_cast<float>(m_columns) * (m_cardWidth + gap) - gap;
    const float height = 2 * pad + 32 * m_uiScale + static_cast<float>(shownRows) * (m_cardHeight + gap) - gap;
    chrome = popup_chrome::computeGeometry(width, height, m_config.config().shell.shadow, Style::popupShadowsEnabled());
    const auto edge = m_config.config().dock.position;
    const bool bottom = edge == DockEdge::Bottom;
    const bool top = edge == DockEdge::Top;
    const bool right = edge == DockEdge::Right;
    const int half = std::max(1, m_config.config().dock.iconSize / 2);
    PopupSurfaceConfig popupConfig{
        .anchorX = static_cast<int>(m_platform.lastPointerX()) - half,
        .anchorY = static_cast<int>(m_platform.lastPointerY()) - half,
        .anchorWidth = 2 * half,
        .anchorHeight = 2 * half,
        .width = chrome.surfaceWidth,
        .height = chrome.surfaceHeight,
        .anchor = bottom ? XDG_POSITIONER_ANCHOR_TOP
            : top        ? XDG_POSITIONER_ANCHOR_BOTTOM
            : right      ? XDG_POSITIONER_ANCHOR_LEFT
                         : XDG_POSITIONER_ANCHOR_RIGHT,
        .gravity = bottom ? XDG_POSITIONER_GRAVITY_TOP
            : top         ? XDG_POSITIONER_GRAVITY_BOTTOM
            : right       ? XDG_POSITIONER_GRAVITY_LEFT
                          : XDG_POSITIONER_GRAVITY_RIGHT,
        .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
        .offsetX = right      ? -8
            : (bottom || top) ? 0
                              : 8,
        .offsetY = bottom ? -8
            : top         ? 8
                          : 0,
        .grab = false,
    };
    popup_chrome::applyToConfig(
        popupConfig, chrome,
        {
            .horizontal = (bottom || top) ? popup_chrome::HorizontalAttachment::Center
                : right                   ? popup_chrome::HorizontalAttachment::Right
                                          : popup_chrome::HorizontalAttachment::Left,
            .vertical = bottom ? popup_chrome::VerticalAttachment::Bottom
                : top          ? popup_chrome::VerticalAttachment::Top
                               : popup_chrome::VerticalAttachment::Center,
        }
    );
    surface = std::make_unique<PopupSurface>(m_platform.wayland());
    surface->setRenderContext(&m_renderContext);
    surface->setAnimationManager(&animations);
    surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) {
      m_dirty = true;
      surface->requestLayout();
    });
    surface->setPrepareFrameCallback([this](bool, bool needsLayout) {
      if (!sceneRoot || (m_dirty && !inputDispatcher.pointerCaptured())) {
        buildScene();
      }
      updateImages();
      if (needsLayout && sceneRoot) {
        // Hover/press styling can request layout. Keep the existing controls so
        // a button retains its pointer capture through press and release.
        m_renderContext.makeCurrent(surface->renderTarget());
        UiPhaseScope layoutPhase(UiPhase::Layout);
        sceneRoot->layout(surface->renderTarget().renderer());
      }
    });
    surface->setDismissedCallback([alive = m_alive, dismiss = callbacks.dismiss] {
      DeferredCall::callLater([alive, dismiss] {
        if (*alive && dismiss)
          dismiss();
      });
    });
    if (!surface->initialize(parent, output, popupConfig))
      return false;
    wlSurface = surface->wlSurface();
    popup_chrome::setContentInputRegion(*surface, chrome);
    setPage(0);
    return true;
  }

  void DockPreview::refreshWindows(std::vector<ToplevelInfo> current) {
    const bool unchanged = current.size() == windows.size()
        && std::equal(current.begin(), current.end(), windows.begin(), [](const auto& a, const auto& b) {
                             return samePreviewWindow(a, b) && a.title == b.title && a.extHandle == b.extHandle;
                           });
    if (unchanged)
      return;
    const bool sameWindows = current.size() == windows.size()
        && std::equal(current.begin(), current.end(), windows.begin(), [](const auto& a, const auto& b) {
                               return samePreviewWindow(a, b) && a.extHandle == b.extHandle;
                             });
    if (sameWindows) {
      // Title changes do not interrupt live capture.
      windows = std::move(current);
      m_dirty = true;
      surface->requestUpdateOnly();
      return;
    }
    windows = std::move(current);
    setPage(m_page);
  }

  void DockPreview::setPage(std::size_t page) {
    m_captures.clear();
    m_page = std::min(page, windows.empty() ? std::size_t{0} : (windows.size() - 1) / m_pageSize);
    m_images.clear();
    m_images.resize(std::min(m_pageSize, windows.size() - m_page * m_pageSize));
    m_imageDirty.assign(m_images.size(), false);
    m_imageNodes.clear();
    m_fallbackNodes.clear();
    const float pad = 12 * m_uiScale;
    const float gap = 8 * m_uiScale;
    const auto rows = std::max(std::size_t{1}, (std::min(m_pageSize, windows.size()) + m_columns - 1) / m_columns);
    chrome = popup_chrome::computeGeometry(
        2 * pad + static_cast<float>(m_columns) * (m_cardWidth + gap) - gap,
        2 * pad + 32 * m_uiScale + static_cast<float>(rows) * (m_cardHeight + gap) - gap,
        m_config.config().shell.shadow, Style::popupShadowsEnabled()
    );
    surface->resize(chrome.surfaceWidth, chrome.surfaceHeight);
    popup_chrome::setContentInputRegion(*surface, chrome);
    m_dirty = true;
    surface->requestUpdateOnly();
    startCaptures();
  }

  void DockPreview::startCaptures() {
    for (std::size_t slot = 0; slot < m_images.size(); ++slot) {
      const auto& window = windows[m_page * m_pageSize + slot];
      auto* handle = window.extHandle;
      if (!handle) {
        // Taskbar activation may use the wlr handle while capture requires the
        // ext handle. Prefer compositor identity; never guess between twins.
        const auto windowId = m_platform.compositorWindowIdForToplevelInfo(window);
        const auto app = StringUtils::toLower(window.appId);
        const auto candidates = m_platform.wayland().extWindowsForApp(app, app);
        for (const auto& candidate : candidates) {
          if (!candidate.extHandle)
            continue;
          const auto candidateId = m_platform.compositorWindowIdForToplevelInfo(candidate);
          const bool matches = windowId && candidateId
              ? *windowId == *candidateId
              : !windowId && !window.title.empty() && window.title == candidate.title;
          if (!matches)
            continue;
          if (handle) {
            handle = nullptr;
            break;
          }
          handle = candidate.extHandle;
        }
      }
      bool live = false;
      m_platform.wayland().visitExtToplevelHandles([&](auto* candidate) { live |= handle && candidate == handle; });
      if (!live)
        continue;
      auto capture = std::make_unique<ToplevelThumbnailCapture>(m_platform.wayland());
      auto* stream = capture.get();
      m_captures.push_back(std::move(capture));
      stream->stream(
          handle, 480, 300, std::chrono::milliseconds{100},
          [this, slot](std::optional<ScreencopyImage> image, std::string) {
            // A stopped/unavailable source keeps its last frame (or app icon).
            if (!image)
              return;
            // Some compositors supply frames even when the source is static.
            // Avoid uploading identical pixels and redrawing the popup.
            const auto& previous = m_images[slot];
            if (previous
                && previous->width == image->width
                && previous->height == image->height
                && previous->rgba == image->rgba)
              return;
            m_images[slot] = std::move(image);
            m_imageDirty[slot] = true;
            surface->requestUpdateOnly();
          }
      );
    }
  }

  void DockPreview::updateImages() {
    if (m_imageNodes.empty())
      return;
    m_renderContext.makeCurrent(surface->renderTarget());
    auto& renderer = surface->renderTarget().renderer();
    for (std::size_t slot = 0; slot < m_imageNodes.size(); ++slot) {
      if (!m_imageDirty[slot] || !m_images[slot])
        continue;
      const auto& image = *m_images[slot];
      if (m_imageNodes[slot]->setSourceRaw(
              renderer, image.rgba.data(), image.rgba.size(), image.width, image.height, image.width * 4,
              PixmapFormat::RGBA, true
          )) {
        m_imageNodes[slot]->setVisible(true);
        m_fallbackNodes[slot]->setVisible(false);
      }
      m_imageDirty[slot] = false;
    }
  }

  void DockPreview::buildScene() {
    if (!surface->width() || !surface->height())
      return;
    m_renderContext.makeCurrent(surface->renderTarget());
    UiPhaseScope layoutPhase(UiPhase::Layout);
    auto& renderer = surface->renderTarget().renderer();
    const bool first = !sceneRoot;
    inputDispatcher.setSceneRoot(nullptr);
    surface->setSceneRoot(nullptr);
    m_imageNodes.clear();
    m_fallbackNodes.clear();
    sceneRoot = ui::node(
        {.width = static_cast<float>(surface->width()),
         .height = static_cast<float>(surface->height()),
         .animationManager = &animations}
    );
    if (first) {
      sceneRoot->setOpacity(0);
      auto* root = sceneRoot.get();
      animations.animate(
          0, 1, Motion::feedbackMs, Motion::reveal, [root](float value) { root->setOpacity(value); }, {}, root
      );
    }
    if (Style::popupShadowsEnabled())
      (void)popup_chrome::addShadow(*sceneRoot, chrome, m_config.config().shell.shadow, Style::scaledRadiusLg());
    (void)popup_chrome::addCardBackground(*sceneRoot, chrome, 1);
    const float pad = 12 * m_uiScale;
    const float gap = 8 * m_uiScale;
    const float x = chrome.contentX() + pad;
    const float y = chrome.contentY() + pad;
    const auto pages = std::max(std::size_t{1}, (windows.size() + m_pageSize - 1) / m_pageSize);
    auto title = ui::label(
        {.text = std::format("{} · {}", m_appName, windows.size()),
         .fontSize = Style::fontSizeCaption * m_uiScale,
         .fontWeight = FontWeight::Bold,
         .width = chrome.contentWidth - 2 * pad - (pages > 1 ? 118 * m_uiScale : 0),
         .height = 24 * m_uiScale,
         .maxWidth = chrome.contentWidth - 2 * pad - (pages > 1 ? 118 * m_uiScale : 0),
         .maxLines = 1,
         .ellipsize = TextEllipsize::End}
    );
    title->setPosition(x, y);
    sceneRoot->addChild(std::move(title));
    if (pages > 1) {
      auto pageLabel = ui::label(
          {.text = std::format("{}/{}", m_page + 1, pages),
           .fontSize = Style::fontSizeMini * m_uiScale,
           .width = 50 * m_uiScale,
           .height = 24 * m_uiScale,
           .minWidth = 50 * m_uiScale,
           .maxWidth = 50 * m_uiScale,
           .maxLines = 1,
           .textAlign = TextAlign::Center}
      );
      pageLabel->setPosition(chrome.contentX() + chrome.contentWidth - pad - 80 * m_uiScale, y);
      sceneRoot->addChild(std::move(pageLabel));
      for (bool next : {false, true}) {
        auto button = ui::button(
            {.glyph = next ? "chevron-right" : "chevron-left",
             .controlHeight = 24 * m_uiScale,
             .enabled = next ? m_page + 1 < pages : m_page > 0,
             .variant = ButtonVariant::Ghost,
             .padding = 0,
             .width = 26 * m_uiScale,
             .height = 24 * m_uiScale,
             .onClick = [this, next] {
               DeferredCall::callLater([this, alive = m_alive, next] {
                 if (*alive)
                   setPage(next ? m_page + 1 : m_page - 1);
               });
             }}
        );
        button->setPosition(chrome.contentX() + chrome.contentWidth - pad - (next ? 26 : 110) * m_uiScale, y);
        sceneRoot->addChild(std::move(button));
      }
    }
    for (std::size_t slot = 0; slot < m_images.size(); ++slot) {
      const auto window = windows[m_page * m_pageSize + slot];
      auto area = ui::inputArea(
          {.cursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER, .width = m_cardWidth, .height = m_cardHeight}
      );
      area->setPosition(
          x + static_cast<float>(slot % m_columns) * (m_cardWidth + gap),
          y + 32 * m_uiScale + static_cast<float>(slot / m_columns) * (m_cardHeight + gap)
      );
      auto frame = ui::box(
          {.fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.55F),
           .border = colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha),
           .borderWidth = Style::borderWidth,
           .radius = Style::scaledRadiusMd(m_uiScale),
           .width = m_cardWidth,
           .height = m_cardHeight}
      );
      auto* framePtr = frame.get();
      area->addChild(std::move(frame));
      area->setOnEnter([framePtr](const auto&) {
        framePtr->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth);
      });
      area->setOnLeave([framePtr] {
        framePtr->setBorder(colorSpecFromRole(ColorRole::Outline, Style::disabledOutlineAlpha), Style::borderWidth);
      });
      area->setOnClick([this, window](const auto&) {
        DeferredCall::callLater([alive = m_alive, callbacks = m_callbacks, window] {
          if (*alive && callbacks.activateWindow)
            callbacks.activateWindow(window);
        });
      });
      const float previewHeight = m_cardHeight - 30 * m_uiScale;
      auto preview = ui::image(
          {.fit = ImageFit::Contain,
           .radius = Style::scaledRadiusSm(m_uiScale),
           .width = m_cardWidth - 8 * m_uiScale,
           .height = previewHeight - 8 * m_uiScale}
      );
      preview->setPosition(4 * m_uiScale, 4 * m_uiScale);
      preview->setVisible(false);
      m_imageNodes.push_back(preview.get());
      area->addChild(std::move(preview));
      auto fallback = ui::image({.width = 48 * m_uiScale, .height = 48 * m_uiScale});
      fallback->setPosition((m_cardWidth - 48 * m_uiScale) / 2, (previewHeight - 48 * m_uiScale) / 2);
      if (!m_iconPath.empty() && fallback->setSourceFile(renderer, m_iconPath, static_cast<int>(64 * m_uiScale))) {
        m_fallbackNodes.push_back(fallback.get());
        area->addChild(std::move(fallback));
      } else {
        auto glyph = ui::glyph(
            {.glyph = "app-window",
             .glyphSize = 40 * m_uiScale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .width = m_cardWidth,
             .height = previewHeight}
        );
        m_fallbackNodes.push_back(glyph.get());
        area->addChild(std::move(glyph));
      }
      m_imageDirty[slot] = m_images[slot].has_value();
      auto caption = ui::label(
          {.text = window.title.empty() ? m_appName : window.title,
           .fontSize = Style::fontSizeCaption * m_uiScale,
           .width = m_cardWidth - 16 * m_uiScale,
           .height = 26 * m_uiScale,
           .minWidth = m_cardWidth - 16 * m_uiScale,
           .maxWidth = m_cardWidth - 16 * m_uiScale,
           .maxLines = 1,
           .textAlign = TextAlign::Center,
           .ellipsize = TextEllipsize::End}
      );
      caption->setPosition(8 * m_uiScale, previewHeight);
      area->addChild(std::move(caption));
      const bool closable = window.handle || (window.exactIdentity && !window.identifier.empty());
      auto close = ui::button(
          {.glyph = "close",
           .glyphSize = 12 * m_uiScale,
           .controlHeight = 24 * m_uiScale,
           .enabled = closable,
           .variant = ButtonVariant::Outline,
           .tooltip = i18n::tr("dock.actions.close"),
           .padding = 0,
           .width = 24 * m_uiScale,
           .height = 24 * m_uiScale,
           .onClick = [this, window] {
             DeferredCall::callLater([alive = m_alive, callbacks = m_callbacks, window] {
               if (*alive && callbacks.closeWindow)
                 callbacks.closeWindow(window);
             });
           }}
      );
      close->setPosition(m_cardWidth - 30 * m_uiScale, 6 * m_uiScale);
      area->addChild(std::move(close));
      sceneRoot->addChild(std::move(area));
    }
    updateImages();
    sceneRoot->layout(renderer);
    inputDispatcher.setSceneRoot(sceneRoot.get());
    inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform.setCursorShape(serial, shape);
    });
    surface->setSceneRoot(sceneRoot.get());
    if (pointerInside && m_platform.lastPointerSurface() == wlSurface)
      inputDispatcher.pointerEnter(
          static_cast<float>(m_platform.lastPointerX()), static_cast<float>(m_platform.lastPointerY()),
          m_platform.lastInputSerial()
      );
    m_dirty = false;
  }

} // namespace shell::dock
