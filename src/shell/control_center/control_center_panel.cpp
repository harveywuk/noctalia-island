#include "shell/control_center/control_center_panel.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "notification/notification_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/input_dispatcher.h"
#include "render/scene/node.h"
#include "shell/control_center/section_overview.h"
#include "shell/control_center/tabs/focus_tab.h"
#include "shell/control_center/tabs/privacy_tab.h"
#include "shell/control_center/tabs/screen_time_tab.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_content_height.h"
#include "shell/panel/panel_manager.h"
#include "shell/tooltip/tooltip_manager.h"
#include "system/dependency_service.h"
#include "system/easyeffects_service.h"
#include "system/screen_time_service.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/motion.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string_view>
#include <wayland-client-protocol.h>

using namespace control_center;

namespace {

  constexpr auto kMprisRefreshMinInterval = std::chrono::milliseconds(750);
  // Full-height cards need room for rounded-rect AA before the tab viewport clips them.
  constexpr float kTabViewportClipInset = 1.0F;

  void configureIslandAction(Button& button, float scale) {
    if (button.variant() != ButtonVariant::Destructive)
      button.setVariant(ButtonVariant::Tab);
    button.setGlyphSize(21.0F * scale);
    button.setRadius(Style::scaledRadiusLg(scale));
  }

  float tabContentHeight(float viewportHeight) {
    return std::max(1.0F, std::floor(viewportHeight - kTabViewportClipInset));
  }

} // namespace

ControlCenterPanel::ControlCenterPanel(const ControlCenterServices& services) {
  m_hasPowerServices =
      services.upower != nullptr || services.powerProfiles != nullptr || services.idleInhibitor != nullptr;
  WaylandConnection* wayland = services.platform != nullptr ? &services.platform->wayland() : nullptr;
  m_config = services.config;
  m_mpris = services.mpris;
  m_notificationManager = services.notifications;
  m_dependencies = services.dependencies;
  m_sectionOverview = std::make_unique<SectionOverview>(services.mpris, services.httpClient, services.spectrum);
  m_tabs[tabIndex(TabId::Home)] = std::make_unique<HomeTab>(services);
  m_tabs[tabIndex(TabId::Media)] = std::make_unique<MediaTab>(
      services.mpris, services.httpClient, services.config, wayland, PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Audio)] = std::make_unique<AudioTab>(
      services.audio, services.easyEffects, services.mpris, services.config, wayland,
      PanelManager::instance().renderContext()
  );
  m_tabs[tabIndex(TabId::Weather)] = std::make_unique<WeatherTab>(services.weather, services.config);
  m_tabs[tabIndex(TabId::Calendar)] = std::make_unique<CalendarTab>(services.config, services.calendar);
  m_tabs[tabIndex(TabId::Notifications)] =
      std::make_unique<NotificationsTab>(services.notifications, services.platform);
  m_tabs[tabIndex(TabId::Network)] =
      std::make_unique<NetworkTab>(services.network, services.networkSecrets, services.externalIp, services.modem);
  m_tabs[tabIndex(TabId::Bluetooth)] = std::make_unique<BluetoothTab>(services.bluetooth, services.bluetoothAgent);
  m_tabs[tabIndex(TabId::Monitor)] = std::make_unique<MonitorTab>(services.brightness, services.config);
  m_tabs[tabIndex(TabId::System)] = std::make_unique<SystemTab>(services.sysmon);
  m_tabs[tabIndex(TabId::ScreenTime)] = std::make_unique<ScreenTimeTab>(services.screenTime);
  m_tabs[tabIndex(TabId::Power)] =
      std::make_unique<PowerTab>(services.upower, services.powerProfiles, services.idleInhibitor);
  m_tabs[tabIndex(TabId::Privacy)] = std::make_unique<PrivacyTab>(services.audio, services.config, services.platform);
  m_tabs[tabIndex(TabId::Focus)] = std::make_unique<FocusTab>(services.notifications);
  m_tabButtons.fill(nullptr);
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
}

ControlCenterPanel::~ControlCenterPanel() = default;

void ControlCenterPanel::showSectionOverview() {
  if (!m_islandPresentation || !m_overviewContainer)
    return;
  if (m_showOverview) {
    selectTab(m_activeTab, true);
    PanelManager::instance().refresh();
    return;
  }
  TooltipManager::instance().forceDestroy();
  m_tabs[tabIndex(m_activeTab)]->dismissTransientUi();
  if (m_tabTransitionAnimId && m_animations) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }
  m_showOverview = true;
  syncTabVisibility();
  updateTabChrome(m_activeTab);
  startTabTransition(m_activeTab, m_activeTab);
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  if (!dispatcher.pointerCaptured() && dispatcher.focusedArea())
    m_sectionOverview->focusCurrent();
  scheduleMprisRefreshFor(TabId::Media);
  PanelManager::instance().refresh();
}

float ControlCenterPanel::preferredWidth() const {
  const float fullSize = m_config != nullptr ? static_cast<float>(m_config->config().controlCenter.width)
                                             : static_cast<float>(ControlCenterConfig::kDefaultWidth);
  switch (sidebarModeForOpen(pendingOpenContext())) {
  case ControlCenterSidebarMode::Full:
    return fullSize * m_contentScale;
  case ControlCenterSidebarMode::None:
    return fullSize * 0.75F * m_contentScale;
  default:
  case ControlCenterSidebarMode::Compact:
    return fullSize * 0.85F * m_contentScale;
  }
}

float ControlCenterPanel::fittedHeight() const {
  if (!m_islandPresentation || !m_contentHeader || !m_content)
    return preferredHeight();
  const float body = m_showOverview ? panel_content::height(m_overviewContainer)
      : m_activeTab == TabId::Calendar
      ? static_cast<const CalendarTab*>(m_tabs[tabIndex(TabId::Calendar)].get())->fittedHeight()
      : panel_content::height(m_tabContainers[tabIndex(m_activeTab)]);
  const float navigation = panel_content::height(m_contentHeader);
  // Round up: tabContentHeight() floors the body, and at fractional UI scales a body of, say,
  // 200.7px would otherwise get a 200px viewport and show a scrollbar for under a pixel.
  return std::max(
      scaled(180),
      std::ceil(
          std::ceil(body)
          + kTabViewportClipInset
          + navigation
          + m_content->gap()
          + 2 * Style::panelPadding * contentScale()
      )
  );
}

PanelPlacement ControlCenterPanel::panelPlacement() const noexcept {
  return m_config == nullptr ? PanelPlacement::Attached : m_config->config().shell.panel.controlCenterPlacement;
}

bool ControlCenterPanel::dismissTransientUi() {
  if (m_showOverview) {
    selectTab(m_activeTab, true);
    PanelManager::instance().refresh();
    return true;
  }
  const std::size_t activeIdx = tabIndex(m_activeTab);
  return m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi();
}

void ControlCenterPanel::create() {
  const float scale = contentScale();
  const ControlCenterSidebarMode sidebarMode = sidebarModeForOpen(pendingOpenContext());
  m_islandPresentation = PanelManager::instance().isIslandOpen();
  m_compact = m_islandPresentation || sidebarMode == ControlCenterSidebarMode::Compact;
  m_showSidebar = !m_islandPresentation && sidebarMode != ControlCenterSidebarMode::None;

  for (auto& tab : m_tabs) {
    tab->setContentScale(scale);
    tab->setPanelCardOpacity(panelCardOpacity());
  }

  auto rootLayout = ui::row({
      .out = &m_rootLayout,
      .align = FlexAlign::Stretch,
      .gap = Style::panelPadding * scale,
      .padding = 0.0F,
  });

  if (m_islandPresentation)
    rootLayout->setDirection(FlexDirection::Vertical);

  if (m_showSidebar) {
    auto sidebar = ui::column({
        .out = &m_sidebar,
        .align = FlexAlign::Start,
        .gap = 0.0F,
        .padding = Style::spaceMd * scale,
        .fillHeight = true,
        .configure = [this, scale](Flex& column) {
          column.setFill(colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()));
          column.setRadius(Style::scaledRadiusXl(scale));
        },
    });

    auto sidebarScrollArea = ui::inputArea({});
    sidebarScrollArea->setParticipatesInLayout(false);
    sidebarScrollArea->setZIndex(-1);
    m_sidebarScrollArea = sidebarScrollArea.get();
    wireSidebarScroll(m_sidebarScrollArea);
    sidebar->addChild(std::move(sidebarScrollArea));

    const std::optional<float> sidebarScrollWidth =
        m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::nullopt;

    auto sidebarScroll = ui::scrollView({
        .out = &m_sidebarScrollView,
        .state = &m_sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .fillWidth = false,
        .fillHeight = true,
        .width = sidebarScrollWidth,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .wrap = true,
        .scrollIntoView = [this](const Node* node) { scrollSidebarNodeIntoView(node); },
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("control-center.sidebar");
    if (!m_compact) {
      sidebarNav->setAlign(FlexAlign::Stretch);
      sidebarNav->setFillWidth(true);
    } else {
      sidebarNav->setAlign(FlexAlign::Start);
    }
    sidebarNav->setGap(Style::spaceXs * scale);
    m_sidebarNav = sidebarNav.get();

    for (const auto& tab : kTabs) {
      const std::size_t idx = tabIndex(tab.id);
      const auto onClick = [this, id = tab.id]() {
        selectTab(id, true);
        PanelManager::instance().refresh();
      };
      sidebarNav->addChild(
          ui::button({
              .out = &m_tabButtons[idx],
              .text = m_compact ? std::optional<std::string>{} : std::optional<std::string>{i18n::tr(tab.titleKey)},
              .glyph = tab.glyph,
              .glyphSize = 18.0F * scale,
              .contentAlign = m_compact ? ButtonContentAlign::Center : ButtonContentAlign::Start,
              .variant = ButtonVariant::Tab,
              .tooltip = m_compact ? i18n::tr(tab.titleKey) : std::string{},
              .minWidth = m_compact ? std::optional<float>{Style::controlHeightSm * scale} : std::optional<float>{},
              .minHeight = Style::controlHeightSm * scale,
              .paddingV = Style::spaceXs * scale,
              .paddingH = (m_compact ? Style::spaceXs : Style::spaceSm) * scale,
              .gap = Style::spaceSm * scale,
              .radius = Style::scaledRadiusLg(scale),
              .onClick = onClick,
              .configure = [this, scale](Button& button) {
                if (button.label() != nullptr) {
                  button.label()->setFontWeight(FontWeight::Normal);
                  button.label()->setFontSize(Style::fontSizeBody * scale);
                }
                wireSidebarScroll(button.inputArea());
              },
          })
      );
      sidebarNav->registerItem(m_tabButtons[idx], onClick);
    }

    if (sidebarScroll->content() != nullptr) {
      if (!m_compact) {
        sidebarScroll->content()->setAlign(FlexAlign::Stretch);
      }
      sidebarScroll->content()->addChild(std::move(sidebarNav));
    }
    sidebar->addChild(std::move(sidebarScroll));
    rootLayout->addChild(std::move(sidebar));
  }

  auto content = ui::column({
      .out = &m_content,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
      .clipChildren = true,
      .flexGrow = 4.0F,
  });

  auto dismissArea = ui::inputArea({});
  dismissArea->setParticipatesInLayout(false);
  dismissArea->setZIndex(-1);
  dismissArea->setFocusable(false);
  dismissArea->setTabStop(false);
  dismissArea->setOnPress([this](const InputArea::PointerData&) {
    const std::size_t activeIdx = tabIndex(m_activeTab);
    if (m_tabs[activeIdx] != nullptr && m_tabs[activeIdx]->dismissTransientUi()) {
      PanelManager::instance().refresh();
    }
  });
  m_contentDismissArea = static_cast<InputArea*>(content->addChild(std::move(dismissArea)));

  auto header = ui::row({
      .out = &m_contentHeader,
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
  });

  if (m_islandPresentation) {
    header->addChild(
        ui::button({
            .out = &m_sectionButton,
            .text = i18n::tr("control-center.tabs.home"),
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("control-center.choose-section"),
            .minHeight = 32.0F * scale,
            .padding = 0.0F,
            .flexGrow = 1.0F,
            .onClick = [this] { showSectionOverview(); },
            .configure =
                [this, scale](Button& button) {
                  m_contentTitle = button.label();
                  m_contentTitle->setFontSize(18.0F * scale);
                  m_contentTitle->setFontWeight(FontWeight::SemiBold);
                  auto titlePalette = Button::defaultPalette(ButtonVariant::Ghost);
                  for (auto* state : {&titlePalette.normal, &titlePalette.hover, &titlePalette.pressed}) {
                    state->bg = clearColorSpec();
                    state->border = clearColorSpec();
                  }
                  titlePalette.pressed.label = scaleAlpha(titlePalette.normal.label, 0.6F);
                  button.setCustomPalette(titlePalette);
                  button.inputArea()->setTabFocusKey("control-center.section");
                },
        })
    );
  } else {
    header->addChild(
        ui::label({
            .out = &m_contentTitle,
            .text = i18n::tr("control-center.tabs.home"),
            .fontSize = Style::fontSizeHeader * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .flexGrow = 1.0F,
        })
    );
  }

  auto headerActions = ui::row({
      .out = &m_contentHeaderActions,
      .align = FlexAlign::Center,
      .gap = (m_islandPresentation ? Style::spaceXs : Style::spaceSm) * scale,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto actions = m_tabs[i]->createHeaderActions();
    m_tabHeaderActions[i] = actions.get();
    if (actions != nullptr) {
      if (m_islandPresentation) {
        actions->setGap(Style::spaceXs * scale);
        for (const auto& child : actions->children()) {
          if (auto* button = dynamic_cast<Button*>(child.get()))
            configureIslandAction(*button, scale);
        }
      }
      actions->setVisible(false);
      m_contentHeaderActions->addChild(std::move(actions));
    }
  }

  m_contentHeaderActions->addChild(
      ui::button({
          .out = &m_closeButton,
          .glyph = "close",
          .tooltip = i18n::tr("dock.actions.close"),
          .onClick =
              [this] {
                if (m_showOverview) {
                  selectTab(m_activeTab, true);
                  PanelManager::instance().refresh();
                } else {
                  PanelManager::instance().close();
                }
              },
          .configure =
              [this, scale](Button& button) {
                panel_button_style::configureHeaderIconButton(button, scale);
                if (m_islandPresentation)
                  configureIslandAction(button, scale);
              },
      })
  );
  header->addChild(std::move(headerActions));

  content->addChild(std::move(header));

  auto bodies = ui::column({
      .out = &m_tabBodies,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .clipChildren = true,
      .flexGrow = 1.0F,
  });

  for (std::size_t i = 0; i < kTabCount; ++i) {
    auto container = m_tabs[i]->create();
    container->setFlexGrow(1.0F);
    container->setParticipatesInLayout(false);
    container->setVisible(false);
    m_tabContainers[i] = container.get();
    m_tabBodies->addChild(std::move(container));
  }

  if (m_islandPresentation) {
    m_sectionOverview->setContentScale(scale);
    std::vector<SectionOverview::Section> sections;
    constexpr std::array order{TabId::Home,    TabId::Media,   TabId::Audio,   TabId::Network,       TabId::Bluetooth,
                               TabId::Power,   TabId::Focus,   TabId::Privacy, TabId::Notifications, TabId::Calendar,
                               TabId::Weather, TabId::Monitor, TabId::System,  TabId::ScreenTime};
    for (const auto id : order) {
      const auto meta = std::ranges::find(kTabs, id, &TabMeta::id);
      sections.push_back(
          {static_cast<int>(id), meta->key,
           i18n::tr(id == TabId::Media ? "control-center.media.now-playing" : meta->titleKey), meta->glyph}
      );
    }
    m_sectionOverview->setSections(
        std::move(sections),
        [this](int id) {
          TooltipManager::instance().forceDestroy();
          selectTab(static_cast<TabId>(id), true);
          PanelManager::instance().refresh();
        },
        static_cast<int>(TabId::Media)
    );
    auto overview = m_sectionOverview->create();
    overview->setParticipatesInLayout(false);
    overview->setVisible(false);
    m_overviewContainer = overview.get();
    m_tabBodies->addChild(std::move(overview));
  }

  content->addChild(std::move(bodies));
  rootLayout->addChild(std::move(content));
  setRoot(std::move(rootLayout));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  syncTabVisibility();
  m_firstOpenAfterCreate = true;
  selectTab(m_activeTab);
}

void ControlCenterPanel::onPanelCardOpacityChanged(float opacity) {
  for (auto& tab : m_tabs) {
    if (tab != nullptr) {
      tab->setPanelCardOpacity(opacity);
    }
  }
  if (m_sidebar != nullptr) {
    m_sidebar->setFill(colorSpecFromRole(ColorRole::SurfaceVariant, opacity));
  }
}

void ControlCenterPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_rootLayout == nullptr || m_content == nullptr || m_tabBodies == nullptr) {
    return;
  }

  if (!m_compact && m_showSidebar) {
    layoutFullSidebarWidth(renderer);
  }

  m_rootLayout->setSize(width, height);
  m_rootLayout->layout(renderer);
  const float contentInnerWidth =
      std::max(0.0F, m_content->width() - (m_content->paddingLeft() + m_content->paddingRight()));
  const float bodyWidth = m_tabBodies->width();
  const float bodyHeight = m_tabBodies->height();
  const float bodyContentHeight = tabContentHeight(bodyHeight);

  if (m_sidebarScrollArea != nullptr && m_sidebar != nullptr) {
    m_sidebarScrollArea->setPosition(0.0F, 0.0F);
    m_sidebarScrollArea->setSize(m_sidebar->width(), m_sidebar->height());
  }

  if (m_contentDismissArea != nullptr) {
    m_contentDismissArea->setPosition(0.0F, 0.0F);
    m_contentDismissArea->setFrameSize(m_content->width(), m_content->height());
  }

  if (m_contentHeader != nullptr) {
    m_contentHeader->setSize(contentInnerWidth, 0.0F);
  }

  if (m_contentTitle != nullptr) {
    const float actionsWidth = m_contentHeaderActions != nullptr ? m_contentHeaderActions->width() : 0.0F;
    const float headerGap = m_contentHeader != nullptr ? m_contentHeader->gap() : 0.0F;
    const float titleWidth = std::max(0.0F, contentInnerWidth - actionsWidth - headerGap);
    m_contentTitle->setMaxWidth(titleWidth);
  }

  for (auto* container : m_tabContainers) {
    if (container != nullptr && container->visible()) {
      container->setSize(bodyWidth, bodyContentHeight);
    }
  }

  layoutTabContainers(bodyWidth, bodyHeight);

  const auto layoutTab = [this, bodyWidth, bodyContentHeight, &renderer](TabId tabId) {
    const std::size_t idx = tabIndex(tabId);
    if (m_tabs[idx] == nullptr || m_tabContainers[idx] == nullptr || !m_tabContainers[idx]->visible()) {
      return;
    }
    m_tabs[idx]->layout(renderer, bodyWidth, bodyContentHeight);
  };

  if (m_tabTransitionActive) {
    layoutTab(m_tabTransitionOutgoing);
  }
  layoutTab(m_activeTab);
  if (m_overviewContainer && m_overviewContainer->visible())
    m_sectionOverview->layout(renderer, bodyWidth, bodyContentHeight);
}

void ControlCenterPanel::doUpdate(Renderer& renderer) {
  if (!isTabFeatureAvailable(m_activeTab) || (!m_activeTabForced && !isTabVisible(m_activeTab))) {
    selectTab(firstVisibleTab());
  } else {
    syncTabVisibility();
  }
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_showOverview) {
    m_sectionOverview->update(renderer);
  } else if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->update(renderer);
  }
}

void ControlCenterPanel::onFrameTick(float deltaMs) {
  if (m_showOverview)
    m_sectionOverview->onFrameTick(deltaMs);
  const std::size_t activeIdx = tabIndex(m_activeTab);
  if (m_tabs[activeIdx] != nullptr) {
    m_tabs[activeIdx]->onFrameTick(deltaMs);
  }
}

void ControlCenterPanel::onOpen(std::string_view context) {
  if (m_dependencies != nullptr) {
    m_dependencies->rescan();
  }
  const bool animateTabSwitch = !m_firstOpenAfterCreate;
  m_firstOpenAfterCreate = false;
  selectTab(tabFromContext(context), animateTabSwitch);
}

bool ControlCenterPanel::isContextActive(std::string_view context) const {
  return !m_showOverview && m_activeTab == tabFromContext(context);
}

bool ControlCenterPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (m_islandPresentation && pressed && !preedit) {
    if (m_showOverview && m_sectionOverview->handleKey(sym, modifiers))
      return true;
    if (KeySymbol::isTab(sym) && (modifiers == KeyMod::Ctrl || modifiers == (KeyMod::Ctrl | KeyMod::Shift))) {
      selectAdjacentVisibleTab((modifiers & KeyMod::Shift) ? -1 : 1);
      return true;
    }
  }
  if (!m_showSidebar || m_sidebarNav == nullptr || m_sidebarScrollView == nullptr || m_content == nullptr) {
    return false;
  }

  const SplitPaneFocusConfig panes{
      .sidebarFocus = m_sidebarNav->focusArea(),
      .sidebarRoot = m_sidebarScrollView,
      .contentRoot = m_content,
      .headerFocus = nullptr,
  };
  auto& dispatcher = PanelManager::instance().inputDispatcher();
  const SplitPaneFocusResult splitResult =
      handleSplitPaneFocusNavigation(dispatcher, panes, sym, modifiers, pressed, preedit);
  return splitResult == SplitPaneFocusResult::Consumed;
}

void ControlCenterPanel::onClose() {
  m_sectionOverview->onClose();
  m_overviewContainer = nullptr;
  m_showOverview = false;
  m_sectionButton = nullptr;
  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
  }
  m_tabTransitionActive = false;
  m_activeTab = TabId::Home;
  for (auto& tab : m_tabs) {
    tab->setActive(false);
    tab->onClose();
  }
  m_rootLayout = nullptr;
  m_sidebar = nullptr;
  m_sidebarScrollView = nullptr;
  m_sidebarScrollState = {};
  m_sidebarNav = nullptr;
  m_sidebarScrollArea = nullptr;
  m_content = nullptr;
  m_contentDismissArea = nullptr;
  m_contentHeader = nullptr;
  m_contentHeaderActions = nullptr;
  m_contentTitle = nullptr;
  m_closeButton = nullptr;
  m_tabBodies = nullptr;
  m_tabButtons.fill(nullptr);
  m_tabContainers.fill(nullptr);
  m_tabHeaderActions.fill(nullptr);
  clearReleasedRoot();
}

bool ControlCenterPanel::deferExternalRefresh() const {
  if (m_showOverview || m_activeTab != TabId::Audio) {
    return false;
  }
  const auto* audioTab = dynamic_cast<const AudioTab*>(m_tabs[tabIndex(TabId::Audio)].get());
  return audioTab != nullptr && audioTab->dragging();
}

bool ControlCenterPanel::deferPointerRelayout() const { return deferExternalRefresh(); }

bool ControlCenterPanel::isTabFeatureAvailable(TabId tab) const {
  if (m_config == nullptr) {
    switch (tab) {
    case TabId::ScreenTime:
      return false;
    case TabId::Power:
      return m_hasPowerServices;
    default:
      return true;
    }
  }
  const auto& cfg = m_config->config();
  switch (tab) {
  case TabId::Notifications:
    return false; // Notification Centre is its own panel (NotificationCenterPanel).
  case TabId::Weather:
    return cfg.weather.enabled;
  case TabId::ScreenTime:
    return cfg.shell.screenTimeEnabled;
  case TabId::System:
    return cfg.system.monitor.enabled;
  case TabId::Power:
    return m_hasPowerServices;
  default:
    return true;
  }
}

bool ControlCenterPanel::isTabVisible(TabId tab) const {
  if (!isTabFeatureAvailable(tab)) {
    return false;
  }
  // Home is always shown so the panel never opens to an empty surface.
  if (tab == TabId::Home || m_config == nullptr) {
    return true;
  }
  const auto& hidden = m_config->config().controlCenter.hiddenTabs;
  return !std::ranges::contains(hidden, tabKey(tab));
}

bool ControlCenterPanel::isTabShown(TabId tab) const {
  if (tab == m_activeTab && m_activeTabForced) {
    return isTabFeatureAvailable(tab);
  }
  return isTabVisible(tab);
}

std::vector<ControlCenterPanel::TabCatalogEntry> ControlCenterPanel::hideableTabCatalog() {
  std::vector<TabCatalogEntry> out;
  out.reserve(kTabCount - 1);
  for (const auto& meta : kTabs) {
    if (meta.id == TabId::Home) {
      continue;
    }
    out.push_back({.key = meta.key, .titleKey = meta.titleKey});
  }
  return out;
}

std::vector<ControlCenterPanel::LauncherTabEntry> ControlCenterPanel::visibleTabsForLauncher() const {
  std::vector<LauncherTabEntry> out;
  out.reserve(kTabCount - 1);
  for (const auto& meta : kTabs) {
    if (meta.id == TabId::Home || !isTabVisible(meta.id)) {
      continue;
    }
    out.push_back({.key = meta.key, .titleKey = meta.titleKey, .glyph = meta.glyph});
  }
  return out;
}

std::string_view ControlCenterPanel::tabKey(TabId tab) {
  for (const auto& meta : kTabs) {
    if (meta.id == tab) {
      return meta.key;
    }
  }
  return {};
}

ControlCenterPanel::TabId ControlCenterPanel::firstVisibleTab() const {
  for (const auto& meta : kTabs) {
    if (isTabVisible(meta.id)) {
      return meta.id;
    }
  }
  return TabId::Home;
}

void ControlCenterPanel::syncTabVisibility() {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool visible = isTabShown(meta.id);
    if (m_overviewContainer)
      m_sectionOverview->setShown(static_cast<int>(meta.id), visible);
    if (m_tabButtons[idx] != nullptr) {
      m_tabButtons[idx]->setVisible(visible);
    }
    if (!visible) {
      if (m_tabContainers[idx] != nullptr) {
        m_tabContainers[idx]->setVisible(false);
      }
      if (m_tabHeaderActions[idx] != nullptr) {
        m_tabHeaderActions[idx]->setVisible(false);
      }
    }
  }
}

void ControlCenterPanel::updateTabChrome(TabId tab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool tabEnabled = isTabShown(meta.id);
    if (m_tabs[idx] != nullptr) {
      m_tabs[idx]->setActive(!m_showOverview && tabEnabled && meta.id == tab);
    }
    if (m_tabButtons[idx] != nullptr) {
      m_tabButtons[idx]->setVisible(tabEnabled);
      m_tabButtons[idx]->setVariant(meta.id == tab ? ButtonVariant::TabActive : ButtonVariant::Tab);
    }
    if (meta.id == tab && m_contentTitle != nullptr) {
      m_contentTitle->setText(
          i18n::tr(
              m_islandPresentation && (m_showOverview || tab == TabId::Home)
                  ? "launcher.providers.panel.builtin.control-center"
                  : m_islandPresentation && tab == TabId::Media ? "control-center.media.now-playing"
                                                                : meta.titleKey
          )
      );
    }
    if (m_tabHeaderActions[idx] != nullptr) {
      m_tabHeaderActions[idx]->setVisible(!m_showOverview && tabEnabled && meta.id == tab);
    }
  }

  if (m_overviewContainer) {
    m_sectionOverview->setCurrent(static_cast<int>(tab));
    m_sectionOverview->setActive(m_showOverview);
    m_sectionButton->setTooltip(
        i18n::tr(m_showOverview ? "control-center.return-to-section" : "control-center.choose-section")
    );
    m_closeButton->setTooltip(i18n::tr(m_showOverview ? "control-center.return-to-section" : "dock.actions.close"));
  }
  if (m_contentTitle != nullptr) {
    m_contentTitle->setVisible(true);
  }
  if (m_contentHeaderActions != nullptr) {
    m_contentHeaderActions->setVisible(true);
  }
  if (m_sidebarNav != nullptr) {
    m_sidebarNav->notifyExternalSelectionChanged();
  }
}

void ControlCenterPanel::applyTabContainerVisibility(TabId activeTab) {
  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    const bool active = !m_showOverview && isTabShown(meta.id) && meta.id == activeTab;
    if (m_tabContainers[idx]) {
      m_tabContainers[idx]->setVisible(active);
      m_tabContainers[idx]->setHitTestVisible(active);
      m_tabContainers[idx]->setExcludeSubtreeFromTabOrder(!active);
    }
  }
  if (m_overviewContainer) {
    m_overviewContainer->setVisible(m_showOverview);
    m_overviewContainer->setHitTestVisible(m_showOverview);
    m_overviewContainer->setExcludeSubtreeFromTabOrder(!m_showOverview);
  }
}

void ControlCenterPanel::layoutTabContainers(float bodyWidth, float bodyHeight) {
  const float travel = std::min(std::max(bodyHeight, 0.0F), 12.0F * m_contentScale);
  const float contentHeight = tabContentHeight(bodyHeight);
  const auto layout = [&](Flex* container, bool incoming) {
    if (!container || !container->visible())
      return;
    container->setSize(bodyWidth, contentHeight);
    float offsetY = 0.0F;
    float opacity = 1.0F;
    if (m_tabTransitionActive) {
      opacity = incoming ? m_tabTransitionProgress : 1.0F - m_tabTransitionProgress;
      if (!m_islandPresentation)
        offsetY = static_cast<float>(m_tabTransitionDirection)
            * travel
            * (incoming ? 1.0F - m_tabTransitionProgress : -m_tabTransitionProgress);
    }
    container->setPosition(0.0F, offsetY);
    container->setOpacity(opacity);
    container->setZIndex(m_tabTransitionActive && incoming ? 1 : 0);
  };
  for (std::size_t i = 0; i < kTabCount; ++i)
    layout(m_tabContainers[i], !m_showOverview && i == tabIndex(m_activeTab));
  layout(m_overviewContainer, m_showOverview);
}

void ControlCenterPanel::resetTabContainerTransforms() {
  const auto reset = [](Flex* container) {
    if (container) {
      container->setPosition(0.0F, 0.0F);
      container->setOpacity(1.0F);
      container->setZIndex(0);
    }
  };
  for (auto* container : m_tabContainers)
    reset(container);
  reset(m_overviewContainer);
}

int ControlCenterPanel::visibleTabOrdinal(TabId tab) const {
  int ordinal = 0;
  for (const auto& meta : kTabs) {
    if (!isTabShown(meta.id)) {
      continue;
    }
    if (meta.id == tab) {
      return ordinal;
    }
    ++ordinal;
  }
  return 0;
}

void ControlCenterPanel::applyTabTransitionLayout() {
  if (m_tabBodies == nullptr) {
    return;
  }
  layoutTabContainers(m_tabBodies->width(), m_tabBodies->height());
}

void ControlCenterPanel::startTabTransition(TabId from, TabId to, bool fromOverview) {
  if (m_animations == nullptr || m_tabBodies == nullptr || !MotionService::instance().enabled()) {
    applyTabContainerVisibility(to);
    resetTabContainerTransforms();
    return;
  }

  m_tabTransitionActive = true;
  m_tabTransitionOutgoing = from;
  m_tabTransitionProgress = 0.0F;

  const int fromOrdinal = visibleTabOrdinal(from);
  const int toOrdinal = visibleTabOrdinal(to);
  m_tabTransitionDirection = toOrdinal >= fromOrdinal ? 1 : -1;

  for (const auto& meta : kTabs) {
    const std::size_t idx = tabIndex(meta.id);
    if (m_tabContainers[idx] == nullptr) {
      continue;
    }
    const bool incoming = !m_showOverview && meta.id == to;
    const bool outgoing = !fromOverview && meta.id == from;
    m_tabContainers[idx]->setVisible((incoming || outgoing) && isTabFeatureAvailable(meta.id));
    m_tabContainers[idx]->setHitTestVisible(incoming);
    m_tabContainers[idx]->setExcludeSubtreeFromTabOrder(!incoming);
  }
  if (m_overviewContainer) {
    m_overviewContainer->setVisible(m_showOverview || fromOverview);
    m_overviewContainer->setHitTestVisible(m_showOverview);
    m_overviewContainer->setExcludeSubtreeFromTabOrder(!m_showOverview);
  }

  applyTabTransitionLayout();
  PanelManager::instance().requestLayout();
  PanelManager::instance().requestRedraw();
  PanelManager::instance().requestFrameTick();

  m_tabTransitionAnimId = m_animations->animate(
      0.0F, 1.0F, Motion::contentMs, Motion::reveal,
      [this](float progress) {
        m_tabTransitionProgress = progress;
        applyTabTransitionLayout();
        PanelManager::instance().requestRedraw();
      },
      [this]() {
        m_tabTransitionAnimId = 0;
        finishTabTransition();
        PanelManager::instance().requestLayout();
        PanelManager::instance().requestRedraw();
      },
      m_tabBodies
  );
}

void ControlCenterPanel::finishTabTransition() {
  m_tabTransitionActive = false;
  resetTabContainerTransforms();
  applyTabContainerVisibility(m_activeTab);
}

void ControlCenterPanel::wireSidebarScroll(InputArea* area) {
  if (area == nullptr) {
    return;
  }
  area->setOnAxis([this](const InputArea::PointerData& data) {
    if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
      return;
    }
    const float steps = data.scrollSteps();
    if (steps == 0.0F) {
      return;
    }
    selectAdjacentVisibleTab(steps > 0.0F ? 1 : -1);
  });
}

void ControlCenterPanel::selectAdjacentVisibleTab(int direction) {
  if (direction == 0) {
    return;
  }

  const int activeOrdinal = visibleTabOrdinal(m_activeTab);
  const int targetOrdinal = activeOrdinal + direction;

  int ordinal = 0;
  for (const auto& meta : kTabs) {
    if (!isTabShown(meta.id)) {
      continue;
    }
    if (ordinal == targetOrdinal) {
      if (meta.id != m_activeTab) {
        selectTab(meta.id, true);
        PanelManager::instance().refresh();
      }
      return;
    }
    ++ordinal;
  }
}

void ControlCenterPanel::selectTab(TabId tab, bool animated) {
  if (!isTabFeatureAvailable(tab)) {
    tab = firstVisibleTab();
  }
  m_activeTabForced = !isTabVisible(tab);

  const TabId previousTab = m_activeTab;
  const bool tabChanged = tab != previousTab;

  if (m_tabTransitionAnimId != 0 && m_animations != nullptr) {
    m_animations->cancel(m_tabTransitionAnimId);
    m_tabTransitionAnimId = 0;
    finishTabTransition();
  }

  const bool fromOverview = m_showOverview;
  m_showOverview = false;
  m_activeTab = tab;
  if (tab == TabId::Notifications && m_notificationManager != nullptr) {
    m_notificationManager->markNotificationHistorySeen();
  }

  updateTabChrome(tab);

  if ((tabChanged || fromOverview) && animated && m_animations != nullptr && m_tabBodies != nullptr) {
    startTabTransition(previousTab, tab, fromOverview);
  } else {
    m_tabTransitionActive = false;
    applyTabContainerVisibility(tab);
    resetTabContainerTransforms();
  }

  if (fromOverview) {
    auto& dispatcher = PanelManager::instance().inputDispatcher();
    if (!dispatcher.pointerCaptured())
      dispatcher.setFocus(m_sectionButton->inputArea());
  }
  scheduleMprisRefreshFor(tab);
}

void ControlCenterPanel::scheduleMprisRefreshFor(TabId tab) {
  if (m_mpris == nullptr || m_mprisRefreshScheduled || (tab != TabId::Home && tab != TabId::Media)) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastMprisRefreshAt.time_since_epoch().count() != 0 && now - m_lastMprisRefreshAt < kMprisRefreshMinInterval) {
    return;
  }

  m_lastMprisRefreshAt = now;
  m_mprisRefreshScheduled = true;
  DeferredCall::callLater([this]() {
    m_mprisRefreshScheduled = false;
    if (m_mpris == nullptr || !PanelManager::instance().isOpenPanel("control-center")) {
      return;
    }
    m_mpris->refreshPlayers();
    PanelManager::instance().requestUpdateOnly();
    PanelManager::instance().requestRedraw();
  });
}

bool ControlCenterPanel::isDirectSectionOpenContext(std::string_view context) const {
  if (context.empty() || context == "home") {
    return false;
  }
  for (const auto& tab : kTabs) {
    if (tab.id != TabId::Home && context == tab.key) {
      return true;
    }
  }
  return false;
}

ControlCenterSidebarMode ControlCenterPanel::sidebarModeForOpen(std::string_view context) const {
  if (m_config == nullptr) {
    return ControlCenterSidebarMode::Compact;
  }
  const auto& cc = m_config->config().controlCenter;
  return isDirectSectionOpenContext(context) ? cc.sidebarSectionMode : cc.sidebarMode;
}

ControlCenterPanel::TabId ControlCenterPanel::tabFromContext(std::string_view context) const {
  for (const auto& tab : kTabs) {
    if (context == tab.key) {
      return tab.id;
    }
  }
  return TabId::Home;
}

std::size_t ControlCenterPanel::tabIndex(TabId id) { return static_cast<std::size_t>(id); }

void ControlCenterPanel::layoutFullSidebarWidth(Renderer& renderer) {
  if (m_sidebarScrollView == nullptr || m_sidebarNav == nullptr) {
    return;
  }

  const float scale = contentScale();
  const float fontSize = Style::fontSizeBody * scale;
  const float paddingH = Style::spaceSm * scale * 2.0F;
  const float gap = Style::spaceSm * scale;
  const float glyphW = 21.0F * scale;

  float maxTabWidth = 0.0F;
  for (const auto& meta : kTabs) {
    if (!isTabVisible(meta.id)) {
      continue;
    }
    const TextMetrics text = renderer.measureText(i18n::tr(meta.titleKey), fontSize, FontWeight::Bold);
    maxTabWidth = std::max(maxTabWidth, paddingH + glyphW + gap + text.width);
  }

  const float minWidth = Style::controlHeightSm * scale;
  const float contentWidth = std::max(minWidth, std::ceil(maxTabWidth));

  // Scrollbar gutter lives inside the scroll viewport; reserve it only when the nav overflows.
  float targetWidth = contentWidth;
  const float scrollHeight = m_sidebarScrollView->height();
  if (scrollHeight > 0.0F) {
    LayoutConstraints navConstraints;
    navConstraints.setExactWidth(contentWidth);
    const float navHeight = m_sidebarNav->measure(renderer, navConstraints).height;
    if (navHeight > scrollHeight + 0.5F) {
      targetWidth = contentWidth + m_sidebarScrollView->scrollbarGutter();
    }
  }

  if (std::abs(m_sidebarScrollView->width() - targetWidth) > 0.5F) {
    m_sidebarScrollView->setSize(targetWidth, m_sidebarScrollView->height());
  }
}

void ControlCenterPanel::scrollSidebarNodeIntoView(const Node* node) {
  if (node == nullptr || m_sidebarScrollView == nullptr) {
    return;
  }
  scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *node, Style::spaceXs * contentScale());
  PanelManager::instance().requestLayout();
}

void ControlCenterPanel::scrollFocusedInputIntoView(InputArea* area) {
  if (area == nullptr) {
    return;
  }

  if (m_sidebarScrollView != nullptr && m_sidebarScrollView->content() != nullptr) {
    for (const Node* node = area; node != nullptr; node = node->parent()) {
      if (node == m_sidebarScrollView->content()) {
        scrollNodeIntoScrollView(*m_sidebarScrollView, &m_sidebarScrollState, *area, Style::spaceXs * contentScale());
        PanelManager::instance().requestLayout();
        return;
      }
    }
  }

  if (ScrollView* scrollView = findEnclosingScrollView(area)) {
    scrollNodeIntoScrollView(*scrollView, nullptr, *area, Style::spaceMd * contentScale());
    PanelManager::instance().requestLayout();
  }
}
