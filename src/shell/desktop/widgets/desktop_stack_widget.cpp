#include "shell/desktop/widgets/desktop_stack_widget.h"

#include "calendar/calendar_service.h"
#include "config/config_service.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "shell/desktop/desktop_widget_factory.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "system/weather_service.h"
#include "ui/builders.h"
#include "ui/node_motion.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <nlohmann/json.hpp>
#include <utility>

namespace {
  bool interacting(const Node* node) {
    if (!node || !node->visible())
      return false;
    if (const auto* area = dynamic_cast<const InputArea*>(node);
        area && (area->hovered() || area->pressed() || area->focused()))
      return true;
    return std::ranges::any_of(node->children(), [](const auto& child) { return interacting(child.get()); });
  }
} // namespace

DesktopStackWidget::DesktopStackWidget(
    std::string id, std::vector<DesktopWidgetState> cards, std::unordered_map<std::string, WidgetSettingValue> settings,
    DesktopWidgetRuntimeServices services
)
    : m_id(std::move(id)), m_cards(std::move(cards)), m_settings(std::move(settings)), m_services(services) {
  if (auto it = m_settings.find("card_size"); it != m_settings.end())
    if (const auto* size = std::get_if<std::string>(&it->second))
      m_size = desktop_cards::sizeFromSetting(*size);
  if (auto it = m_settings.find("auto_rotate"); it != m_settings.end())
    if (const auto* enabled = std::get_if<bool>(&it->second))
      m_autoRotate = *enabled;
  if (auto it = m_settings.find("smart_rotate"); it != m_settings.end())
    if (const auto* enabled = std::get_if<bool>(&it->second))
      m_smartRotate = *enabled;
  if (auto it = m_settings.find("rotation_seconds"); it != m_settings.end())
    if (const auto* seconds = std::get_if<std::int64_t>(&it->second))
      m_rotation = desktop_stacks::RotationSchedule(static_cast<int>(std::clamp<std::int64_t>(*seconds, 5, 3600)));
  if (auto* config = m_services.scriptDeps.configService; config && !m_id.empty()) {
    if (auto stored = config->stateString("desktop_stacks", "pages")) {
      const auto pages = nlohmann::json::parse(*stored, nullptr, false);
      if (pages.is_object() && pages.contains(m_id) && pages[m_id].is_string()) {
        const auto it = std::ranges::find(m_cards, pages[m_id].get<std::string>(), &DesktopWidgetState::id);
        if (it != m_cards.end())
          m_page = static_cast<std::size_t>(it - m_cards.begin());
      }
    }
    if (auto stored = config->stateString("desktop_stacks", "mornings")) {
      const auto days = nlohmann::json::parse(*stored, nullptr, false);
      if (days.is_object() && days.contains(m_id) && days[m_id].is_string()) {
        const auto value = days[m_id].get<std::string>();
        int day = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), day);
        if (error == std::errc{} && end == value.data() + value.size() && day > 0)
          m_smartRotation.setLastWeatherDay(day);
      }
    }
    if (auto stored = config->stateString("desktop_stacks", "pins")) {
      const auto pins = nlohmann::json::parse(*stored, nullptr, false);
      if (pins.is_object() && pins.contains(m_id) && pins[m_id].is_string()) {
        const auto it = std::ranges::find(m_cards, pins[m_id].get<std::string>(), &DesktopWidgetState::id);
        if (it != m_cards.end()) {
          m_page = static_cast<std::size_t>(it - m_cards.begin());
          m_pinned = true;
        }
      }
    }
  }
}

void DesktopStackWidget::create() {
  auto node = ui::inputArea({.onAxis = [this](const InputArea::PointerData& data) {
    const auto steps = data.scrollSteps();
    if (m_preview || steps == 0 || m_widgets.size() < 2)
      return;
    showPage(steps > 0 ? (m_page + 1) % m_widgets.size() : (m_page + m_widgets.size() - 1) % m_widgets.size());
  }});
  DesktopWidgetFactory factory(m_services);
  const auto selectedId = m_page < m_cards.size() ? m_cards[m_page].id : std::string();
  const bool wasPinned = m_pinned;
  const auto definitions = std::exchange(m_cards, {});
  m_page = 0;
  m_pinned = false;
  for (const auto& card : definitions) {
    auto settings = card.settings;
    settings["background"] = false;
    settings["card_size"] = std::string(
        m_size == desktop_cards::Size::Small       ? "small"
            : m_size == desktop_cards::Size::Large ? "large"
                                                   : "medium"
    );
    auto widget = factory.create(card.type, settings);
    if (!widget)
      continue;
    if (canConfigure())
      widget->setConfigureCallback([this, id = card.id](const std::string&) { requestConfigure(id); });
    if (canShowDetails())
      widget->setDetailsCallback([this](DesktopWidgetDetailsRequest request) { requestDetails(request); });
    const auto page = m_widgets.size();
    m_cards.push_back(card);
    if (card.id == selectedId) {
      m_page = page;
      m_pinned = wasPinned;
    }
    if (card.type == "calendar" && !m_smartPages.calendar)
      m_smartPages.calendar = page;
    else if (card.type == "media_player" && !m_smartPages.media)
      m_smartPages.media = page;
    else if (card.type == "weather" && !m_smartPages.weather)
      m_smartPages.weather = page;
    widget->create();
    widget->setEditorPreview(m_preview);
    widget->setUpdateCallback([this, page]() {
      if (page == m_page)
        requestUpdate();
    });
    widget->setLayoutCallback([this, page]() {
      if (page == m_page && !isLayingOut())
        requestLayout();
    });
    widget->setRedrawCallback([this, page]() {
      if (page == m_page)
        requestRedraw();
    });
    widget->setFrameTickRequestCallback([this, page]() {
      if (page == m_page)
        requestFrameTick();
    });
    // Cards such as Now Playing control their own visibility. Keep page
    // visibility on a separate parent so a card cannot reveal an inactive page.
    auto pageRoot = ui::node({});
    pageRoot->addChild(widget->releaseRoot());
    m_roots.push_back(node->addChild(std::move(pageRoot)));
    m_widgets.push_back(std::move(widget));
  }
  for (std::size_t page = 0; page < m_roots.size(); ++page)
    m_roots[page]->setVisible(page == m_page);
  for (std::size_t i = 0; i < m_widgets.size(); ++i) {
    auto area = ui::inputArea({.onClick = [this, i](const InputArea::PointerData&) {
      if (!m_preview)
        showPage(i);
    }});
    area->setTooltip(desktop_settings::desktopWidgetTypeLabel(m_cards[i].type));
    m_pageAreas.push_back(area.get());
    auto dot = ui::box({.radius = 3.0F});
    dot->setHitTestVisible(false);
    m_dots.push_back(dot.get());
    area->addChild(std::move(dot));
    node->addChild(std::move(area));
  }
  auto pin = ui::inputArea(
      {.tooltip = i18n::tr("desktop-widgets.stack.pin"), .onClick = [this](const InputArea::PointerData&) {
         if (!m_preview)
           setPinned(!m_pinned);
       }}
  );
  m_pinArea = pin.get();
  pin->addChild(ui::glyph({.out = &m_pinGlyph, .glyph = "pin"}));
  node->addChild(std::move(pin));
  node->addChild(ui::label({.out = &m_empty, .text = i18n::tr("desktop-widgets.stack.empty"), .maxLines = 4}));
  node->addChild(
      ui::button(
          {.out = &m_configure,
           .text = i18n::tr("desktop-widgets.setup.configure"),
           .variant = ButtonVariant::Secondary,
           .onClick = [this]() { requestConfigure(); }}
      )
  );
  setRoot(std::move(node));
}

void DesktopStackWidget::setEditorPreview(bool enabled) noexcept {
  m_preview = enabled;
  m_rotation.reset();
  for (auto& widget : m_widgets)
    widget->setEditorPreview(enabled);
}

bool DesktopStackWidget::wantsSecondTicks() const {
  return (!m_preview && (m_autoRotate || (m_smartRotate && !m_smartPages.empty())) && !m_pinned && m_widgets.size() > 1)
      || (m_page < m_widgets.size() && m_widgets[m_page]->wantsSecondTicks());
}

bool DesktopStackWidget::needsFrameTick() const {
  return !m_preview && m_page < m_widgets.size() && m_widgets[m_page]->needsFrameTick();
}

void DesktopStackWidget::onFrameTick(float deltaMs, Renderer& renderer) {
  if (needsFrameTick())
    m_widgets[m_page]->onFrameTick(deltaMs, renderer);
}

void DesktopStackWidget::doRebindRenderer(Renderer& renderer) {
  for (auto& widget : m_widgets)
    widget->rebindRenderer(renderer);
}

void DesktopStackWidget::setInteractionActive(bool active) {
  m_interacting = active;
  m_rotation.reset();
}

void DesktopStackWidget::saveState(const char* key, const std::string& value) {
  if (auto* config = m_services.scriptDeps.configService; config && !m_id.empty() && !m_preview) {
    const auto stored = config->stateString("desktop_stacks", key);
    auto entries = stored ? nlohmann::json::parse(*stored, nullptr, false) : nlohmann::json::object();
    if (entries.is_object()) {
      entries[m_id] = value;
      (void)config->setStateString("desktop_stacks", key, entries.dump());
    }
  }
}

void DesktopStackWidget::setPinned(bool pinned) {
  if (m_page >= m_cards.size())
    return;
  m_pinned = pinned;
  m_rotation.reset();
  if (!pinned)
    m_smartRotation.resume();
  saveState("pins", pinned ? m_cards[m_page].id : std::string());
  requestLayout();
}

void DesktopStackWidget::showPage(std::size_t page) {
  if (page >= m_widgets.size())
    return;
  if (m_pinned && page != m_page)
    setPinned(false);
  m_smartRotation.manualSelection();
  selectPage(page);
}

void DesktopStackWidget::selectPage(std::size_t page) {
  m_rotation.reset();
  if (page >= m_widgets.size() || page == m_page)
    return;
  m_page = page;
  const bool animate = m_animations && !m_preview && MotionService::instance().enabled();
  for (std::size_t i = 0; i < m_roots.size(); ++i) {
    auto* node = m_roots[i];
    if (m_animations)
      m_animations->cancelForOwner(node);
    node->setAnimationManager(m_animations);
    node->setHitTestVisible(i == page);
    node->setExcludeSubtreeFromTabOrder(i != page);
    if (!animate) {
      node->setVisible(i == page);
      node->setOpacity(1);
    } else if (i == page) {
      if (!node->visible())
        node->setOpacity(0);
      node->setVisible(true);
      Motion::fadeNode(*node, 1, Motion::contentMs);
    } else if (node->visible()) {
      Motion::fadeNode(*node, 0, Motion::contentMs, [node]() {
        node->setVisible(false);
        node->setOpacity(1);
      });
    }
  }
  saveState("pages", m_cards[m_page].id);
  requestLayout();
}

desktop_stacks::SmartContext DesktopStackWidget::smartContext() const {
  desktop_stacks::SmartContext context;
  const auto now = std::chrono::system_clock::now();
  if (m_smartPages.calendar && m_services.calendar && m_services.calendar->enabled() && m_services.calendar->hasData())
    context.calendarImminent = desktop_stacks::hasUpcomingEvent(m_services.calendar->snapshot().events, now);
  if (m_smartPages.media && m_services.mpris) {
    const auto player = m_services.mpris->activePlayer();
    context.mediaPlaying = player && player->playbackStatus == "Playing";
  }
  if (m_smartPages.weather && m_services.weather && m_services.weather->enabled() && m_services.weather->hasData()) {
    context.weatherAvailable = true;
    const auto raw = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    if (localtime_r(&raw, &local)) {
      context.localDay = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
      context.localHour = local.tm_hour;
    }
  }
  return context;
}

void DesktopStackWidget::doUpdate(Renderer& renderer) {
  const bool paused = m_preview || m_pinned || m_interacting || interacting(root());
  bool smartHold = false;
  if (m_smartRotate && !m_smartPages.empty() && m_widgets.size() > 1) {
    const auto previousDay = m_smartRotation.lastWeatherDay();
    const auto decision = m_smartRotation.evaluate(
        paused ? desktop_stacks::SmartContext{} : smartContext(), m_smartPages, m_page, paused
    );
    smartHold = decision.hold;
    if (decision.page)
      selectPage(*decision.page);
    if (previousDay != m_smartRotation.lastWeatherDay())
      saveState("mornings", std::to_string(m_smartRotation.lastWeatherDay()));
  }
  if (m_autoRotate && m_widgets.size() > 1 && m_rotation.due(paused || smartHold))
    selectPage((m_page + 1) % m_widgets.size());
  if (m_page < m_widgets.size())
    m_widgets[m_page]->update(renderer);
}

void DesktopStackWidget::doLayout(Renderer& renderer) {
  const auto card =
      desktop_cards::resolve(m_size, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  for (auto* page : m_roots)
    page->setFrameSize(card.width, card.height);
  if (m_page < m_widgets.size()) {
    auto& widget = *m_widgets[m_page];
    widget.setContentScale(card.scale);
    if (!m_fontFamily.empty())
      widget.setFontFamily(m_fontFamily);
    widget.setAnimationManager(m_animations);
    widget.setBox(card.width, card.height);
    widget.update(renderer);
    widget.layout(renderer);
  }
  const float footerY = card.height + std::min(0.0F, backgroundPadding() - 14 * card.scale);
  const bool showPin = (m_autoRotate || m_smartRotate) && m_widgets.size() > 1;
  const float pinWidth = showPin ? 24 * card.scale : 0.0F;
  const float dotsX = (card.width - pinWidth - static_cast<float>(m_dots.size()) * 18 * card.scale) * .5F
      + (Style::rtl() ? pinWidth : 0.0F);
  for (std::size_t i = 0; i < m_dots.size(); ++i) {
    const auto position = Style::rtl() ? m_dots.size() - i - 1 : i;
    m_pageAreas[i]->setFrameSize(18 * card.scale, 14 * card.scale);
    m_pageAreas[i]->setPosition(dotsX + static_cast<float>(position) * 18 * card.scale, footerY);
    m_dots[i]->setPosition(6 * card.scale, 3 * card.scale);
    m_dots[i]->setSize(6 * card.scale, 6 * card.scale);
    m_dots[i]->setFill(
        colorSpecFromRole(i == m_page ? ColorRole::Primary : ColorRole::OnSurfaceVariant, i == m_page ? 1.0F : .35F)
    );
  }
  m_pinArea->setVisible(showPin);
  m_pinArea->setTooltip(i18n::tr(m_pinned ? "desktop-widgets.stack.unpin" : "desktop-widgets.stack.pin"));
  m_pinArea->setFrameSize(18 * card.scale, 14 * card.scale);
  m_pinArea->setPosition(Style::rtl() ? 0.0F : card.width - 18 * card.scale, footerY);
  m_pinGlyph->setGlyphSize(12 * card.scale);
  m_pinGlyph->setColor(colorSpecFromRole(m_pinned ? ColorRole::Primary : ColorRole::OnSurfaceVariant));
  m_pinGlyph->layout(renderer);
  m_pinGlyph->setPosition(3 * card.scale, card.scale);
  m_pinGlyph->setHitTestVisible(false);
  m_empty->setVisible(m_widgets.empty());
  m_empty->setFontFamily(m_fontFamily);
  m_empty->setMaxWidth(card.width);
  m_empty->measure(renderer);
  m_empty->setPosition(0, 24 * card.scale);
  m_configure->setVisible(m_widgets.empty());
  m_configure->setEnabled(canConfigure());
  m_configure->layout(renderer);
  m_configure->setPosition(0, card.height - m_configure->height());
  m_configure->updateInputArea();
  root()->setSize(card.width, card.height);
}
