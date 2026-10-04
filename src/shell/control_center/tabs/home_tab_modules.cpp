// The Home tab's Big Sur layout: macOS Control Center's modules. A connectivity module lists
// the first three shortcuts as rows (round toggle, name, state), the fourth is the wide tile
// beside it and the fifth and sixth small tiles under that; then the Display and Sound sliders
// and Now Playing, each a rounded module with its title. A click on a name opens its detail tab,
// as a click on "Wi-Fi" or "Sound" does on macOS.

#include "config/config_service.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_service.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_registry.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/control_center/tabs/home_tab.h"
#include "shell/panel/panel_manager.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

using namespace control_center;

namespace {

  constexpr float kToggleDiameter = 28.0F;
  constexpr float kToggleGlyph = 15.0F;
  constexpr float kSliderHeight = Slider::kLevelHeight;
  constexpr auto kSliderCommitInterval = std::chrono::milliseconds(60);
  constexpr auto kSliderHoldoff = std::chrono::milliseconds(600);

  void openTab(std::string_view tab) {
    PanelManager::instance().togglePanel("control-center", PanelOpenRequest{.context = tab});
  }

  void moduleCardStyle(Flex& card, float scale, float opacity) {
    applySectionCardStyle(card, scale, opacity);
    card.setPadding(Style::spaceMd * scale);
    card.setGap(Style::spaceSm * scale);
  }

  // A row's or wide tile's name: the control's own name while a state line says the rest, its
  // live label (the layout, the temperature, Auto Mode) when it has none.
  std::string moduleTitle(const Shortcut& sc) {
    return sc.statusText().empty() ? sc.displayLabel() : sc.defaultLabel();
  }

  // Big Sur's round toggles: the accent with a white symbol when on, a grey disc when off.
  void styleModuleToggle(Button& button, bool enabled, bool on) {
    button.setVariant(enabled && on ? ButtonVariant::Primary : ButtonVariant::Secondary);
    button.setEnabled(enabled);
  }

  std::string volumeGlyph(const AudioNode* sink) {
    if (sink == nullptr || sink->muted || sink->volume <= 0.001F) {
      return "volume-3";
    }
    return sink->volume < 0.5F ? "volume-2" : "volume";
  }

} // namespace

std::vector<std::unique_ptr<Shortcut>> HomeTab::takeShortcuts(std::size_t limit) {
  const auto& configured =
      m_config != nullptr ? m_config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  std::vector<std::unique_ptr<Shortcut>> previous;
  previous.reserve(m_shortcutPads.size());
  for (auto& pad : m_shortcutPads) {
    previous.push_back(std::move(pad.shortcut));
  }
  m_shortcutPads.clear();

  const bool pluginsChanged = m_config != nullptr && !(m_config->config().plugins == m_lastPlugins);
  if (m_config != nullptr) {
    m_lastPlugins = m_config->config().plugins;
  }

  std::vector<std::unique_ptr<Shortcut>> result;
  for (const auto& sc : configured) {
    if (result.size() >= limit) {
      break;
    }
    std::unique_ptr<Shortcut> shortcut;
    for (auto it = previous.begin(); it != previous.end(); ++it) {
      if (*it != nullptr && (*it)->id() == sc.type) {
        shortcut = std::move(*it);
        previous.erase(it);
        break;
      }
    }
    // A plugin shortcut seeds its runtime at construction; changed plugin settings make it stale.
    if (shortcut != nullptr
        && pluginsChanged
        && scripting::isPluginEntryOfKind(sc.type, scripting::PluginEntryKind::Shortcut)) {
      shortcut.reset();
    }
    if (shortcut != nullptr) {
      shortcut->onPanelOpen();
    } else {
      shortcut = ShortcutRegistry::create(sc.type, m_services);
    }
    if (shortcut != nullptr) {
      result.push_back(std::move(shortcut));
    }
  }
  return result;
}

void HomeTab::addModuleOverlay(Flex& target, std::function<void()> onActivate) {
  auto area = ui::inputArea({});
  area->setParticipatesInLayout(false);
  area->setZIndex(3);
  area->setFocusable(false);
  area->setTabStop(false);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
  area->setOnClick([activate = std::move(onActivate)](const InputArea::PointerData&) { activate(); });
  auto* ptr = static_cast<InputArea*>(target.addChild(std::move(area)));
  m_moduleOverlays.emplace_back(&target, ptr);
}

void HomeTab::addPad(Flex& parent, std::unique_ptr<Shortcut> shortcut, ShortcutPadKind kind, float scale) {
  const std::size_t padIdx = m_shortcutPads.size();
  const bool enabled = shortcut->enabled();
  const bool on = shortcut->isToggle() && shortcut->active();
  const float diameter = kToggleDiameter * scale;

  Button* toggle = nullptr;
  auto button = ui::button({
      .out = &toggle,
      .glyph = shortcut->displayIcon(),
      .glyphSize = kToggleGlyph * scale,
      .contentAlign = ButtonContentAlign::Center,
      .minWidth = diameter,
      .minHeight = diameter,
      .maxWidth = diameter,
      .maxHeight = diameter,
      .padding = 0.0F,
      .radius = diameter / 2.0F,
      .onClick =
          [this, padIdx]() {
            if (padIdx < m_shortcutPads.size()) {
              m_shortcutPads[padIdx].shortcut->onClick();
            }
          },
      .onRightClick =
          [this, padIdx]() {
            if (padIdx < m_shortcutPads.size()) {
              m_shortcutPads[padIdx].shortcut->onRightClick();
            }
          },
      .configure = [enabled, on](Button& b) { styleModuleToggle(b, enabled, on); },
  });
  if (auto* ia = toggle->inputArea(); ia != nullptr) {
    // Small tiles carry only a caption, so their tooltip names the state ("Night Light: Off").
    if (kind == ShortcutPadKind::Small) {
      ia->setTooltipProvider(
          [this, padIdx]() -> TooltipContent {
            if (padIdx >= m_shortcutPads.size()) {
              return std::monostate{};
            }
            return m_shortcutPads[padIdx].shortcut->tooltipText();
          },
          std::chrono::seconds(1)
      );
    }
    ia->setOnAxisHandler([this, padIdx](const InputArea::PointerData& data) -> bool {
      if (data.axis != WL_POINTER_AXIS_VERTICAL_SCROLL || padIdx >= m_shortcutPads.size()) {
        return false;
      }
      const float steps = data.scrollSteps();
      if (steps == 0.0F) {
        return false;
      }
      m_shortcutPads[padIdx].shortcut->onScroll(steps > 0.0F ? -1 : 1);
      return true;
    });
  }

  // A click on the name opens the shortcut's detail tab, or toggles it when it has none.
  const auto activateName = [this, padIdx]() {
    if (padIdx >= m_shortcutPads.size()) {
      return;
    }
    auto& sc = *m_shortcutPads[padIdx].shortcut;
    if (sc.opensDetail()) {
      sc.onRightClick();
    } else {
      sc.onClick();
    }
  };

  Label* title = nullptr;
  Label* status = nullptr;
  if (kind == ShortcutPadKind::Small) {
    auto tile = ui::column({
        .align = FlexAlign::Center,
        .justify = FlexJustify::Center,
        .flexGrow = 1.0F,
        .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
          moduleCardStyle(card, scale, opacity);
          card.setAlign(FlexAlign::Center);
          card.setJustify(FlexJustify::Center);
          card.setGap(Style::spaceXs * scale);
          card.setPadding(Style::spaceSm * scale);
        },
    });
    tile->addChild(std::move(button));
    tile->addChild(
        ui::label({
            .out = &title,
            .text = shortcut->displayLabel(),
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 2,
            .textAlign = TextAlign::Center,
        })
    );
    Flex* tilePtr = tile.get();
    parent.addChild(std::move(tile));
    addModuleOverlay(*tilePtr, activateName);
    // The toggle stays on top of the tile's overlay.
    toggle->setZIndex(4);
  } else {
    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale + 2.0F * scale,
        .flexGrow = kind == ShortcutPadKind::Wide ? 1.0F : 0.0F,
    });
    row->addChild(std::move(button));
    auto text = ui::column({.align = FlexAlign::Stretch, .gap = 1.0F * scale, .flexGrow = 1.0F});
    text->addChild(
        ui::label({
            .out = &title,
            .text = moduleTitle(*shortcut),
            .fontSize = Style::fontSizeBody * scale,
            .fontWeight = FontWeight::SemiBold,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = kind == ShortcutPadKind::Wide ? 2 : 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    const std::string state = shortcut->statusText();
    text->addChild(
        ui::label({
            .out = &status,
            .text = state,
            .fontSize = Style::fontSizeCaption * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
            .visible = !state.empty(),
        })
    );
    Flex* textPtr = text.get();
    row->addChild(std::move(text));
    if (kind == ShortcutPadKind::Wide) {
      // The wide tile is its own module, like Big Sur's Do Not Disturb.
      row->setFlexGrow(1.0F);
      moduleCardStyle(*row, scale, panelCardOpacity());
      row->setDirection(FlexDirection::Horizontal);
      row->setAlign(FlexAlign::Center);
      row->setGap(Style::spaceSm * scale + 2.0F * scale);
    }
    parent.addChild(std::move(row));
    addModuleOverlay(*textPtr, activateName);
  }

  ShortcutPad pad;
  pad.shortcut = std::move(shortcut);
  pad.button = toggle;
  pad.glyph = toggle->glyph();
  pad.label = title;
  pad.status = status;
  pad.kind = kind;
  m_shortcutPads.push_back(std::move(pad));
}

std::unique_ptr<Flex> HomeTab::makeShortcutModule(float scale) {
  auto shortcuts = takeShortcuts(6);
  if (shortcuts.empty()) {
    return nullptr;
  }
  auto block = ui::row({.out = &m_shortcutModule, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});

  auto list = ui::column({
      .align = FlexAlign::Stretch,
      .justify = FlexJustify::SpaceBetween,
      .flexGrow = 1.0F,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        moduleCardStyle(card, scale, opacity);
        card.setJustify(FlexJustify::SpaceBetween);
        card.setGap(Style::spaceMd * scale);
      },
  });
  std::size_t index = 0;
  for (; index < shortcuts.size() && index < 3; ++index) {
    addPad(*list, std::move(shortcuts[index]), ShortcutPadKind::Row, scale);
  }
  block->addChild(std::move(list));

  if (index < shortcuts.size()) {
    auto side = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .flexGrow = 1.0F});
    addPad(*side, std::move(shortcuts[index++]), ShortcutPadKind::Wide, scale);
    if (index < shortcuts.size()) {
      auto tiles = ui::row({.align = FlexAlign::Stretch, .gap = Style::spaceSm * scale, .flexGrow = 1.0F});
      for (; index < shortcuts.size(); ++index) {
        addPad(*tiles, std::move(shortcuts[index]), ShortcutPadKind::Small, scale);
      }
      side->addChild(std::move(tiles));
    }
    block->addChild(std::move(side));
  }
  return block;
}

std::unique_ptr<Flex> HomeTab::makeSliderModule(
    float scale, const std::string& title, const std::string& detailTab, const std::string& detailTooltip,
    Slider** slider, Glyph** glyph, std::function<void(double)> onChange,
    std::function<void()> onDragEnd, std::unique_ptr<Node> trailing
) {
  auto module = ui::column({
      .align = FlexAlign::Stretch,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) { moduleCardStyle(card, scale, opacity); },
  });

  // The title opens the detail tab, as Big Sur's module titles open their menus.
  auto header = ui::row(
      {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::label({
          .text = title,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
      }),
      ui::glyph({
          .glyph = "chevron-right",
          .glyphSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
      })
  );
  Flex* headerPtr = header.get();
  module->addChild(std::move(header));
  addModuleOverlay(*headerPtr, [detailTab]() { openTab(detailTab); });
  if (!m_moduleOverlays.empty()) {
    m_moduleOverlays.back().second->setTooltip(detailTooltip);
  }

  auto track = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
  track->addChild(
      ui::slider({
          .out = slider,
          .minValue = 0.0,
          .maxValue = 1.0,
          .step = 0.01,
          .value = 0.0,
          .trackHeight = kSliderHeight * scale,
          .thumbSize = kSliderHeight * scale,
          .controlHeight = kSliderHeight * scale,
          .wheelAdjustEnabled = true,
          .flexGrow = 1.0F,
          .onValueChanged = std::move(onChange),
          .onDragEnd = std::move(onDragEnd),
          .configure = [](Slider& s) { s.setLevelStyle(true); },
      })
  );
  // The symbol sits inside the track's leading end, over the white fill or the knob.
  track->addChild(
      ui::glyph({
          .out = glyph,
          .glyphSize = 14.0F * scale,
          .color = fixedColorSpec(rgba(0.36F, 0.36F, 0.38F, 1.0F)),
          .participatesInLayout = false,
          .configure = [](Glyph& g) {
            g.setZIndex(5);
            g.setHitTestVisible(false);
          },
      })
  );
  if (trailing != nullptr) {
    track->addChild(std::move(trailing));
  }
  module->addChild(std::move(track));
  return module;
}

std::unique_ptr<Flex> HomeTab::makeNowPlayingModule(float scale) {
  auto module = ui::row({
      .out = &m_mediaCard,
      .align = FlexAlign::Center,
      .configure = [scale, opacity = panelCardOpacity()](Flex& card) {
        moduleCardStyle(card, scale, opacity);
        card.setDirection(FlexDirection::Horizontal);
        card.setAlign(FlexAlign::Center);
      },
  });

  const float artSize = Style::controlHeightLg * scale;
  auto info = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale + 2.0F * scale, .flexGrow = 1.0F});
  info->addChild(
      ui::column(
          {.out = &m_mediaArtSlot,
           .align = FlexAlign::Center,
           .justify = FlexJustify::Center,
           .fill = colorSpecFromRole(ColorRole::OnSurface, 0.08F),
           .radius = Style::scaledRadiusMd(scale),
           .width = artSize,
           .height = artSize},
          ui::glyph({
              .out = &m_mediaArtFallback,
              .glyph = "music",
              .glyphSize = artSize * 0.45F,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          }),
          ui::image({
              .out = &m_mediaArt,
              .fit = ImageFit::Cover,
              .radius = Style::scaledRadiusMd(scale),
              .width = artSize,
              .height = artSize,
              .participatesInLayout = false,
              .configure = [](Image& image) { image.setZIndex(1); },
          })
      )
  );
  auto text = ui::column({.out = &m_mediaText, .align = FlexAlign::Stretch, .gap = 1.0F * scale, .flexGrow = 1.0F});
  text->addChild(
      ui::label({
          .out = &m_mediaTrack,
          .text = i18n::tr("control-center.home.media.nothing-playing"),
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  text->addChild(
      ui::label({
          .out = &m_mediaArtist,
          .text = "",
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  // The dashboard's status and position lines are kept for the shared media sync, unshown.
  auto hidden = ui::column({.visible = false, .participatesInLayout = false});
  hidden->addChild(ui::label({.out = &m_mediaStatus, .text = ""}));
  hidden->addChild(ui::label({.out = &m_mediaProgress, .text = ""}));
  text->addChild(std::move(hidden));
  info->addChild(std::move(text));
  Flex* infoPtr = info.get();
  module->addChild(std::move(info));
  addModuleOverlay(*infoPtr, []() { openTab("media"); });

  const float controlSize = Style::controlHeight * scale;
  auto controls = ui::row(
      {.out = &m_mediaControls, .align = FlexAlign::Center, .gap = Style::spaceXs * scale},
      ui::button({
          .out = &m_mediaPlayButton,
          .glyph = "player-play-filled",
          .glyphSize = Style::fontSizeTitle * 1.2F * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.play"),
          .minWidth = controlSize,
          .minHeight = controlSize,
          .padding = 0.0F,
          .radius = controlSize / 2.0F,
          .onClick =
              [this]() {
                if (m_mpris != nullptr) {
                  (void)m_mpris->playPauseActive();
                }
              },
      }),
      ui::button({
          .out = &m_mediaNextButton,
          .glyph = "player-skip-forward-filled",
          .glyphSize = Style::fontSizeTitle * 1.2F * scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = i18n::tr("control-center.media.next"),
          .minWidth = controlSize,
          .minHeight = controlSize,
          .padding = 0.0F,
          .radius = controlSize / 2.0F,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              (void)m_mpris->nextActive();
            }
          },
      })
  );
  module->addChild(std::move(controls));
  return module;
}

std::unique_ptr<Flex> HomeTab::createModules() {
  const float scale = contentScale();
  m_moduleOverlays.clear();
  const auto home =
      m_config != nullptr ? m_config->config().controlCenter.homeTab : ControlCenterConfig::HomeTabConfig{};
  const auto shown = [&home](std::string_view key) { return std::ranges::contains(home.cards, key); };

  auto column = ui::column({.out = &m_modulesColumn, .align = FlexAlign::Stretch, .gap = Style::spaceSm * scale});
  m_rootLayout = m_modulesColumn;

  if (shown("shortcuts")) {
    if (auto block = makeShortcutModule(scale); block != nullptr) {
      column->addChild(std::move(block));
    }
  } else {
    (void)takeShortcuts(0);
  }

  const bool brightnessAvailable = m_brightness != nullptr
      && m_brightness->available()
      && std::ranges::any_of(m_brightness->displays(), [](const auto& display) { return display.controllable; });
  if (shown("display") && brightnessAvailable) {
    auto module = makeSliderModule(
        scale, i18n::tr("control-center.home.modules.display"), "monitor",
        i18n::tr("control-center.home.modules.display-settings"), &m_displaySlider, &m_displayGlyph,
        [this](double value) {
          m_pendingBrightness = static_cast<float>(value);
          m_sliderHoldoff = std::chrono::steady_clock::now() + kSliderHoldoff;
          if (!m_brightnessTimer.active()) {
            flushBrightness();
            m_brightnessTimer.start(kSliderCommitInterval, [this]() { flushBrightness(); });
          }
        },
        [this]() { flushBrightness(); }, nullptr
    );
    m_displayModule = module.get();
    if (m_displayGlyph != nullptr) {
      m_displayGlyph->setGlyph("sun");
    }
    column->addChild(std::move(module));
  }

  if (shown("sound") && m_services.audio != nullptr) {
    const float buttonSize = kToggleDiameter * scale;
    auto output = ui::button({
        .glyph = "device-speaker", // the output picker; "cast" reads as screen sharing
        .glyphSize = kToggleGlyph * scale,
        .variant = ButtonVariant::Secondary,
        .tooltip = i18n::tr("control-center.home.modules.sound-output"),
        .minWidth = buttonSize,
        .minHeight = buttonSize,
        .maxWidth = buttonSize,
        .maxHeight = buttonSize,
        .padding = 0.0F,
        .radius = buttonSize / 2.0F,
        .onClick = []() { openTab("audio"); },
    });
    auto module = makeSliderModule(
        scale, i18n::tr("control-center.home.modules.sound"), "audio",
        i18n::tr("control-center.home.modules.sound-settings"), &m_soundSlider, &m_soundGlyph,
        [this](double value) {
          m_pendingVolume = static_cast<float>(value);
          m_sliderHoldoff = std::chrono::steady_clock::now() + kSliderHoldoff;
          if (!m_volumeTimer.active()) {
            flushVolume();
            m_volumeTimer.start(kSliderCommitInterval, [this]() { flushVolume(); });
          }
        },
        [this]() { flushVolume(); }, std::move(output)
    );
    m_soundModule = module.get();
    if (m_soundSlider != nullptr && m_config != nullptr && m_config->config().audio.enableOverdrive) {
      m_soundSlider->setRange(0.0, 1.5);
    }
    column->addChild(std::move(module));
  }

  if (shown("media")) {
    column->addChild(makeNowPlayingModule(scale));
  }

  if (column->children().empty()) {
    column->addChild(
        ui::label({
            .text = i18n::tr("control-center.home.empty"),
            .fontSize = Style::fontSizeBody * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        })
    );
  }

  auto wrapper = ui::column({.align = FlexAlign::Stretch});
  auto scroll = ui::scrollView({
      .out = &m_modulesScroll,
      .contentScale = scale,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .fillWidth = true,
      .flexGrow = 1.0F,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
      },
  });
  scroll->content()->setAlign(FlexAlign::Stretch);
  scroll->content()->addChild(std::move(column));
  wrapper->addChild(std::move(scroll));
  syncModules();
  return wrapper;
}

void HomeTab::flushBrightness() {
  if (m_pendingBrightness < 0.0F || m_brightness == nullptr) {
    return;
  }
  m_brightness->setAllBrightness(m_pendingBrightness);
  m_pendingBrightness = -1.0F;
}

void HomeTab::flushVolume() {
  if (m_pendingVolume < 0.0F || m_services.audio == nullptr) {
    return;
  }
  const AudioNode* sink = m_services.audio->defaultSink();
  if (sink != nullptr && sink->muted && m_pendingVolume > 0.0F) {
    // Moving the slider unmutes, as on macOS.
    m_services.audio->setMuted(false);
  }
  m_services.audio->setVolume(m_pendingVolume);
  m_pendingVolume = -1.0F;
}

void HomeTab::layoutModules(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_modulesColumn == nullptr) {
    return;
  }
  const auto pass = [&]() {
    LayoutConstraints constraints;
    constraints.setExactWidth(contentWidth);
    const float height = m_modulesColumn->measure(renderer, constraints).height;
    m_modulesColumn->setSize(contentWidth, height);
    m_modulesColumn->layout(renderer);
  };
  pass();

  // Text wraps and ellipsizes within its column once the columns have their widths.
  for (auto& pad : m_shortcutPads) {
    for (Label* label : {pad.label, pad.status}) {
      if (label != nullptr && label->parent() != nullptr) {
        label->setMaxWidth(
            std::max(1.0F, label->parent()->width() - (pad.kind == ShortcutPadKind::Small ? 4.0F : 0.0F))
        );
      }
    }
  }
  if (m_mediaText != nullptr) {
    for (Label* label : {m_mediaTrack, m_mediaArtist}) {
      if (label != nullptr) {
        label->setMaxWidth(std::max(1.0F, m_mediaText->width()));
      }
    }
  }
  pass();

  if (m_modulesScroll != nullptr) {
    m_modulesScroll->setSize(contentWidth, bodyHeight);
    m_modulesScroll->layout(renderer);
  }

  for (auto& [target, area] : m_moduleOverlays) {
    area->setPosition(0.0F, 0.0F);
    area->setSize(target->width(), target->height());
  }
  const float scale = contentScale();
  struct SliderParts {
    Slider* slider;
    Glyph* glyph;
  };
  for (const auto& [slider, glyph] :
       {SliderParts{m_displaySlider, m_displayGlyph}, SliderParts{m_soundSlider, m_soundGlyph}}) {
    if (slider == nullptr) {
      continue;
    }
    // The slider insets its groove; the symbol follows the groove, not the node.
    const float grooveX = slider->x() + Style::sliderHorizontalPadding;
    const float grooveH = kSliderHeight * scale;
    const float grooveY = slider->y() + (slider->height() - grooveH) / 2.0F;
    if (glyph != nullptr) {
      glyph->measure(renderer);
      glyph->setPosition(grooveX + (grooveH - glyph->width()) / 2.0F, grooveY + (grooveH - glyph->height()) / 2.0F);
    }
  }
  if (m_mediaArt != nullptr && m_mediaArtSlot != nullptr) {
    m_mediaArt->setPosition(0.0F, 0.0F);
  }
}

void HomeTab::syncModules() {
  if (!m_modules) {
    return;
  }
  const bool holding = std::chrono::steady_clock::now() < m_sliderHoldoff;

  for (auto& pad : m_shortcutPads) {
    if (pad.kind == ShortcutPadKind::Grid || pad.shortcut == nullptr) {
      continue;
    }
    const Shortcut& sc = *pad.shortcut;
    if (pad.button != nullptr) {
      styleModuleToggle(*pad.button, sc.enabled(), sc.isToggle() && sc.active());
    }
    if (pad.glyph != nullptr) {
      pad.glyph->setGlyph(sc.displayIcon());
    }
    if (pad.label != nullptr) {
      const std::string title = pad.kind == ShortcutPadKind::Small ? sc.displayLabel() : moduleTitle(sc);
      if (pad.label->text() != title) {
        pad.label->setText(title);
        PanelManager::instance().requestLayout();
      }
    }
    if (pad.status != nullptr) {
      const std::string state = sc.statusText();
      if (pad.status->text() != state || pad.status->visible() == state.empty()) {
        pad.status->setText(state);
        pad.status->setVisible(!state.empty());
        PanelManager::instance().requestLayout();
      }
    }
  }

  if (m_displaySlider != nullptr && m_brightness != nullptr) {
    if (!m_displaySlider->dragging() && !holding && m_pendingBrightness < 0.0F) {
      for (const auto& display : m_brightness->displays()) {
        if (display.controllable) {
          m_displaySlider->setValue(display.brightness);
          break;
        }
      }
    }
  }

  if (m_soundSlider != nullptr && m_services.audio != nullptr) {
    const AudioNode* sink = m_services.audio->defaultSink();
    m_soundSlider->setEnabled(sink != nullptr);
    if (sink != nullptr && !m_soundSlider->dragging() && !holding && m_pendingVolume < 0.0F) {
      m_soundSlider->setValue(sink->muted ? 0.0 : sink->volume);
    }
    if (m_soundGlyph != nullptr) {
      m_soundGlyph->setGlyph(volumeGlyph(sink));
    }
  }

  if (m_mediaPlayButton != nullptr) {
    const auto player = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
    const bool playing = player.has_value() && player->playbackStatus == "Playing";
    m_mediaPlayButton->setGlyph(playing ? "player-pause-filled" : "player-play-filled");
    m_mediaPlayButton->setTooltip(i18n::tr(playing ? "control-center.media.pause" : "control-center.media.play"));
    m_mediaPlayButton->setEnabled(player.has_value() && (playing ? player->canPause : player->canPlay));
    if (m_mediaNextButton != nullptr) {
      m_mediaNextButton->setEnabled(player.has_value() && player->canGoNext);
    }
  }
}
