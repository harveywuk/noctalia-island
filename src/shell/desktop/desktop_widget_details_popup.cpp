#include "shell/desktop/desktop_widget_details_popup.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "i18n/i18n.h"
#include "render/animation/motion_service.h"
#include "render/render_context.h"
#include "shell/control_center/tabs/weather_tab.h"
#include "shell/desktop/desktop_battery_ring.h"
#include "shell/desktop/desktop_widget_layout.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/motion.h"
#include "wayland/layer_surface.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>

DesktopWidgetDetailsPopup::DesktopWidgetDetailsPopup(const DesktopWidgetServices& services) : m_services(services) {}
DesktopWidgetDetailsPopup::~DesktopWidgetDetailsPopup() { close(); }

void DesktopWidgetDetailsPopup::open(
    DesktopWidgetDetailsRequest request, PopupSurfaceParent parent, PopupAnchorRect anchor, std::uint32_t serial
) {
  close();
  if (!m_services.config || !m_services.renderContext || !parent.layerSurface || !parent.wlSurface)
    return;
  m_request = request;
  if (!m_request.date.valid())
    m_request.date = calendar_view::stateForOffset(0).current;
  m_parent = parent;
  m_scale = std::max(0.1F, m_services.config->config().accessibility.uiScale);
  const bool weather = request.kind == DesktopWidgetDetailsRequest::Kind::Weather;
  float availableWidth = 1280, availableHeight = 720;
  if (const auto* output = m_services.wayland.findOutputByWl(parent.output)) {
    availableWidth = desktop_widgets::outputLogicalWidth(*output);
    availableHeight = desktop_widgets::outputLogicalHeight(*output);
  }
  const auto& shadow = m_services.config->config().shell.shadow;
  const auto padding = popup_chrome::computeGeometry(1, 1, shadow, Style::popupShadowsEnabled());
  const float width = std::max(
      1.0F,
      std::min((weather ? 680 : 440) * m_scale, availableWidth - static_cast<float>(padding.surfaceWidth - 1) - 24)
  );
  const float height = std::max(
      1.0F,
      std::min((weather ? 580 : 430) * m_scale, availableHeight - static_cast<float>(padding.surfaceHeight - 1) - 24)
  );
  m_compact = width < 620 * m_scale;
  m_chrome = popup_chrome::computeGeometry(width, height, shadow, Style::popupShadowsEnabled());
  m_calendarDirty = true;
  m_openToken = std::make_shared<bool>(true);
  if (request.kind == DesktopWidgetDetailsRequest::Kind::Calendar && m_services.runtime.calendar)
    m_calendarCallback = m_services.runtime.calendar->addChangeCallback([this]() { requestUpdate(); });

  PopupSurfaceConfig config{
      .anchorX = anchor.x,
      .anchorY = anchor.y,
      .anchorWidth = std::max(1, anchor.width),
      .anchorHeight = std::max(1, anchor.height),
      .width = m_chrome.surfaceWidth,
      .height = m_chrome.surfaceHeight,
      .anchor = XDG_POSITIONER_ANCHOR_RIGHT,
      .gravity = XDG_POSITIONER_GRAVITY_RIGHT,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = static_cast<int>(Style::spaceSm * m_scale),
      .serial = serial,
      .grab = true,
  };
  popup_chrome::applyToConfig(
      config, m_chrome,
      {.horizontal = popup_chrome::HorizontalAttachment::Left, .vertical = popup_chrome::VerticalAttachment::Center}
  );
  m_surface = std::make_unique<PopupSurface>(m_services.wayland);
  m_surface->setRenderContext(m_services.renderContext);
  m_surface->setAnimationManager(&m_animations);
  m_surface->setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface->requestLayout(); });
  m_surface->setPrepareFrameCallback([this](bool update, bool layout) { prepareFrame(update, layout); });
  m_surface->setDismissedCallback([this]() { deferClose(); });
  m_surface->setFrameTickCallback([this](float deltaMs) {
    if (m_weather && m_weather->hasActiveEffect() && MotionService::instance().enabled()) {
      m_weather->onFrameTick(deltaMs);
      m_surface->requestFrameTick();
    }
  });
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      parent.layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::OnDemand)
  );
  wl_surface_commit(parent.wlSurface);
  if (!m_surface->initialize(parent.layerSurface, parent.output, config)) {
    close();
    return;
  }
  popup_chrome::setContentInputRegion(*m_surface, m_chrome);
}

void DesktopWidgetDetailsPopup::close() {
  if (m_calendarCallback && m_services.runtime.calendar)
    m_services.runtime.calendar->removeChangeCallback(std::exchange(m_calendarCallback, 0));
  if (m_openToken)
    *m_openToken = false;
  const bool wasOpen = isOpen();
  m_input.setSceneRoot(nullptr);
  if (m_weather)
    m_weather->onClose();
  m_weather.reset();
  m_events.linkOverlays.clear();
  m_batteries.reset();
  m_heading = nullptr;
  m_dayNavigation = nullptr;
  m_scroll = nullptr;
  m_date = nullptr;
  m_root.reset();
  m_surface.reset();
  m_animations.cancelAll();
  if (m_parent.layerSurface && m_parent.wlSurface) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        m_parent.layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
    );
    wl_surface_commit(m_parent.wlSurface);
  }
  m_parent = {};
  if (wasOpen && m_onDismissed)
    m_onDismissed();
}

void DesktopWidgetDetailsPopup::deferClose() {
  const std::weak_ptr<bool> token = m_openToken;
  DeferredCall::callLater([this, token]() {
    if (auto alive = token.lock(); alive && *alive)
      close();
  });
}

void DesktopWidgetDetailsPopup::buildScene() {
  const bool weather = m_request.kind == DesktopWidgetDetailsRequest::Kind::Weather;
  m_root = ui::node({});
  m_root->setAnimationManager(&m_animations);
  m_root->setSize(static_cast<float>(m_surface->width()), static_cast<float>(m_surface->height()));
  if (Style::popupShadowsEnabled())
    (void)popup_chrome::addShadow(
        *m_root, m_chrome, m_services.config->config().shell.shadow, Style::scaledRadiusLg(m_scale)
    );
  (void)popup_chrome::addCardBackground(*m_root, m_chrome, m_scale);
  auto heading = ui::row(
      {.out = &m_heading, .align = FlexAlign::Center, .gap = Style::spaceSm * m_scale},
      ui::label(
          {.text = i18n::tr(
               weather ? "desktop-widgets.details.weather"
                   : m_request.kind == DesktopWidgetDetailsRequest::Kind::Batteries
                   ? "desktop-widgets.details.batteries"
                   : "desktop-widgets.details.calendar"
           ),
           .fontSize = Style::fontSizeTitle * m_scale,
           .fontWeight = FontWeight::Bold,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxLines = 1,
           .flexGrow = 1}
      ),
      ui::button(
          {.glyph = "x",
           .fontSize = Style::fontSizeBody * m_scale,
           .variant = ButtonVariant::Ghost,
           .tooltip = i18n::tr("desktop-widgets.details.close"),
           .onClick = [this]() { deferClose(); }}
      )
  );
  m_root->addChild(std::move(heading));
  if (m_request.kind == DesktopWidgetDetailsRequest::Kind::Calendar) {
    m_root->addChild(
        ui::row(
            {.out = &m_dayNavigation, .align = FlexAlign::Center, .gap = Style::spaceSm * m_scale},
            ui::button(
                {.glyph = "chevron-left",
                 .fontSize = Style::fontSizeBody * m_scale,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = i18n::tr("desktop-widgets.details.previous-day"),
                 .onClick = [this]() { changeDay(-1); }}
            ),
            ui::label(
                {.out = &m_date,
                 .fontSize = Style::fontSizeBody * m_scale,
                 .color = colorSpecFromRole(ColorRole::Primary),
                 .maxLines = 2,
                 .flexGrow = 1}
            ),
            ui::button(
                {.glyph = "chevron-right",
                 .fontSize = Style::fontSizeBody * m_scale,
                 .variant = ButtonVariant::Ghost,
                 .tooltip = i18n::tr("desktop-widgets.details.next-day"),
                 .onClick = [this]() { changeDay(1); }}
            )
        )
    );
  }
  auto scroll = std::make_unique<ScrollView>();
  m_scroll = scroll.get();
  scroll->setContentScale(m_scale);
  scroll->clearFill();
  scroll->clearBorder();
  scroll->setViewportPaddingH(0);
  scroll->setViewportPaddingV(0);
  scroll->content()->setAlign(FlexAlign::Stretch);
  if (weather) {
    m_weather = std::make_unique<WeatherTab>(m_services.runtime.weather, m_services.config, m_compact);
    m_weather->setContentScale(m_scale);
    m_weather->setRefreshCallback([this]() { requestUpdate(); });
    auto content = m_weather->create();
    content->setMinHeight((m_compact ? 960 : 510) * m_scale);
    scroll->content()->addChild(std::move(content));
  }
  m_root->addChild(std::move(scroll));
  m_input.setSceneRoot(m_root.get());
  m_input.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.wayland.setCursorShape(serial, shape);
  });
  m_surface->setSceneRoot(m_root.get());
  auto* root = m_root.get();
  root->setOpacity(0);
  m_animations.animate(
      0, 1, Motion::feedbackMs, Motion::reveal, [root](float value) { root->setOpacity(value); }, {}, root
  );
}

void DesktopWidgetDetailsPopup::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (!m_surface)
    return;
  m_services.renderContext->makeCurrent(m_surface->renderTarget());
  auto& renderer = m_surface->renderTarget().renderer();
  UiPhaseScope phase(UiPhase::Layout);
  if (!m_root) {
    buildScene();
    needsUpdate = needsLayout = true;
  }
  const float inset = Style::spaceMd * m_scale;
  const float x = m_chrome.contentX() + inset, y = m_chrome.contentY() + inset;
  const float width = std::max(1.0F, m_chrome.contentWidth - inset * 2);
  const float headerHeight = Style::controlHeightLg * m_scale;
  m_heading->setPosition(x, y);
  m_heading->setSize(width, headerHeight);
  m_heading->layout(renderer);
  float bodyY = y + headerHeight + Style::spaceSm * m_scale;
  if (m_dayNavigation) {
    m_dayNavigation->setPosition(x, bodyY);
    m_dayNavigation->setSize(width, headerHeight);
    m_dayNavigation->layout(renderer);
    bodyY += headerHeight + Style::spaceMd * m_scale;
  }
  m_scroll->setPosition(x, bodyY);
  m_scroll->setSize(width, std::max(1.0F, m_chrome.contentY() + m_chrome.contentHeight - inset - bodyY));
  if (m_weather) {
    if (needsUpdate)
      m_weather->update(renderer);
    if (needsUpdate || needsLayout) {
      m_weather->layout(renderer, m_scroll->contentViewportWidth(true), (m_compact ? 960 : 510) * m_scale);
    }
    if (m_weather->hasActiveEffect() && MotionService::instance().enabled())
      m_surface->requestFrameTick();
  } else if (m_request.kind == DesktopWidgetDetailsRequest::Kind::Batteries) {
    if (needsUpdate || needsLayout || !m_batteries)
      rebuildBatteries();
  } else if (m_calendarDirty || needsUpdate || needsLayout) {
    const auto& config = m_services.config->config().calendar;
    calendar_view::rebuildEventList(
        {.scroll = *m_scroll,
         .reserveScrollbarGutter = true,
         .title = m_date,
         .snapshot = m_services.runtime.calendar ? &m_services.runtime.calendar->snapshot() : nullptr,
         .selected = m_request.date,
         .scale = m_scale,
         .dateFormat = config.eventDateFormat,
         .timeFormat = config.eventTimeFormat,
         .state = &m_events,
         .requestRedraw = [this]() { invalidate(); },
         .showDetails = true}
    );
    m_calendarDirty = false;
  }
  m_scroll->layout(renderer);
  if (m_weather)
    m_weather->layout(renderer, m_scroll->contentViewportWidth(true), (m_compact ? 960 : 510) * m_scale);
  else if (m_dayNavigation) {
    m_dayNavigation->layout(renderer);
    calendar_view::layoutEventLinkOverlays(m_events);
  }
}

void DesktopWidgetDetailsPopup::rebuildBatteries() {
  auto devices = desktop_batteries::collect(m_services.runtime);
  if (m_batteries && *m_batteries == devices)
    return;
  m_batteries = std::move(devices);
  auto* content = m_scroll->content();
  while (!content->children().empty())
    content->removeChild(content->children().front().get());
  content->setGap(Style::spaceSm * m_scale);
  if (m_batteries->empty()) {
    content->addChild(
        ui::label(
            {.text = i18n::tr("desktop-widgets.cards.no-batteries"),
             .fontSize = Style::fontSizeBody * m_scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .maxLines = 3}
        )
    );
    return;
  }
  for (const auto& device : *m_batteries) {
    auto icon = ui::column(
        {.align = FlexAlign::Center,
         .justify = FlexJustify::Center,
         .minWidth = 64 * m_scale,
         .minHeight = 80 * m_scale,
         .maxWidth = 64 * m_scale}
    );
    auto ring = std::make_unique<DesktopBatteryRing>();
    ring->setDevice(&device);
    ring->setDiameter(56 * m_scale);
    icon->addChild(std::move(ring));
    auto text =
        ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * m_scale, .minWidth = 0, .flexGrow = 1});
    text->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = Style::spaceSm * m_scale},
            ui::label(
                {.text = device.name,
                 .fontSize = Style::fontSizeBody * m_scale,
                 .fontWeight = FontWeight::Medium,
                 .minWidth = 0,
                 .maxLines = 2,
                 .flexGrow = 1}
            ),
            ui::label(
                {.text = desktop_batteries::percentageText(device),
                 .fontSize = Style::fontSizeTitle * m_scale,
                 .color = colorSpecFromRole(desktop_batteries::low(device) ? ColorRole::Error : ColorRole::OnSurface),
                 .maxLines = 1}
            )
        )
    );
    auto status = desktop_batteries::statusText(device);
    if (!device.percentage)
      status = i18n::tr("desktop-widgets.batteries.unavailable");
    if (!status.empty())
      text->addChild(
          ui::label(
              {.text = status,
               .fontSize = Style::fontSizeCaption * m_scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2}
          )
      );
    const bool charging = desktop_batteries::charging(device);
    const auto seconds = charging                   ? device.timeToFull
        : device.state == BatteryState::Discharging ? device.timeToEmpty
                                                    : 0;
    if (seconds > 0)
      text->addChild(
          ui::label(
              {.text = i18n::tr(
                   charging ? "control-center.power.time-to-full" : "control-center.power.time-to-empty", "time",
                   formatDuration(std::chrono::seconds{seconds})
               ),
               .fontSize = Style::fontSizeCaption * m_scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2}
          )
      );
    if (device.health)
      text->addChild(
          ui::label(
              {.text = i18n::tr("desktop-widgets.batteries.health", "percent", std::format("{:.0f}", *device.health)),
               .fontSize = Style::fontSizeCaption * m_scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 2}
          )
      );
    content->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .gap = Style::spaceSm * m_scale,
             .padding = Style::spaceSm * m_scale,
             .fill = colorSpecFromRole(ColorRole::OnSurface, .04F),
             .radius = Style::scaledRadiusMd(m_scale)},
            std::move(icon), std::move(text)
        )
    );
  }
}

void DesktopWidgetDetailsPopup::changeDay(int delta) {
  std::tm date{};
  date.tm_year = m_request.date.year - 1900;
  date.tm_mon = m_request.date.month;
  date.tm_mday = m_request.date.day + delta;
  date.tm_hour = 12;
  date.tm_isdst = -1;
  if (std::mktime(&date) == -1)
    return;
  m_request.date = {.year = date.tm_year + 1900, .month = date.tm_mon, .day = date.tm_mday};
  m_calendarDirty = true;
  m_scroll->requestScrollToOffset(0);
  requestUpdate();
}

void DesktopWidgetDetailsPopup::requestUpdate() {
  if (m_surface)
    m_surface->requestUpdate();
}

void DesktopWidgetDetailsPopup::invalidate() {
  if (!m_surface || !m_root)
    return;
  if (m_root->layoutDirty())
    m_surface->requestLayout();
  else
    m_surface->requestRedraw();
}

bool DesktopWidgetDetailsPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen())
    return false;
  if (event.pressed && !event.preedit && KeySymbol::isEscape(event.sym))
    deferClose();
  else {
    m_input.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    invalidate();
  }
  return true;
}

bool DesktopWidgetDetailsPopup::onPointerEvent(const PointerEvent& event) {
  if (!m_surface)
    return false;
  auto* eventSurface = event.surface ? event.surface : m_services.wayland.lastPointerSurface();
  const bool onPopup = eventSurface == m_surface->wlSurface();
  float x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (!onPopup) {
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      deferClose();
      return true;
    }
    if (!m_input.pointerCaptured() || eventSurface != m_parent.wlSurface)
      return false;
    x -= static_cast<float>(m_surface->configuredX());
    y -= static_cast<float>(m_surface->configuredY());
  }
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_input.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_input.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_input.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button:
    m_input.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    m_input.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  invalidate();
  return true;
}
