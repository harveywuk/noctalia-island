#include "shell/control_center/section_overview.h"

#include "core/input/key_symbols.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/animation/motion_service.h"
#include "render/scene/input_dispatcher.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/grid_view.h"
#include "ui/scroll_into_view.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <cmath>

namespace control_center {

  SectionOverview::SectionOverview(MprisService* mpris, HttpClient* http, PipeWireSpectrum* spectrum)
      : m_mpris(mpris), m_http(http), m_spectrum(spectrum) {}

  SectionOverview::~SectionOverview() {
    setActive(false);
    m_alive.reset();
  }

  void SectionOverview::setSections(std::vector<Section> sections, std::function<void(int)> activate, int mediaId) {
    m_sections = std::move(sections);
    m_activate = std::move(activate);
    m_mediaId = mediaId;
  }

  std::unique_ptr<Flex> SectionOverview::create() {
    const float scale = contentScale();
    auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch, .gap = 0.0F});
    auto scroll = ui::scrollView({
        .out = &m_scroll,
        .contentScale = scale,
        .viewportPaddingH = 2 * scale,
        .viewportPaddingV = 2 * scale,
        .fillWidth = true,
        .fillHeight = true,
        .flexGrow = 1.0F,
    });
    scroll->clearFill();
    scroll->clearBorder();
    scroll->content()->setGap(18 * scale);
    scroll->content()->setAlign(FlexAlign::Stretch);
    auto grid = std::make_unique<GridView>();
    m_grid = grid.get();
    grid->setColumns(3);
    grid->setAutoColumnMinWidth(160 * scale);
    grid->setColumnGap(8 * scale);
    grid->setRowGap(8 * scale);
    grid->setMinCellHeight(46 * scale);
    grid->setStretchItems(true);
    m_buttons.clear();
    for (const auto& section : m_sections) {
      auto button = ui::button({
          .text = section.title,
          .glyph = section.glyph,
          .fontSize = 13 * scale,
          .glyphSize = 22 * scale,
          .controlHeight = 46 * scale,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .tooltip = section.title,
          .paddingV = 0.0F,
          .paddingH = 8 * scale,
          .gap = 12 * scale,
          .onClick = [this, id = section.id] { m_activate(id); },
      });
      button->inputArea()->setTabFocusKey("control-center.overview." + section.key);
      m_buttons.push_back(button.get());
      grid->addChild(std::move(button));
    }
    scroll->content()->addChild(std::move(grid));
    auto media = ui::column({.out = &m_media, .align = FlexAlign::Stretch, .gap = 18 * scale, .visible = false});
    media->addChild(ui::separator({.color = colorSpecFromRole(ColorRole::OnSurface, 0.09F), .thickness = scale}));
    auto row = ui::row({.out = &m_mediaRow, .align = FlexAlign::Center, .gap = 12 * scale});
    auto artSlot = ui::box({.width = 38 * scale, .height = 38 * scale});
    artSlot->addChild(
        ui::glyph({
            .out = &m_artFallback,
            .glyph = "music",
            .glyphSize = 26 * scale,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .configure = [scale](Glyph& glyph) { glyph.setPosition(6 * scale, 6 * scale); },
        })
    );
    artSlot->addChild(
        ui::image(
            {.out = &m_art,
             .fit = ImageFit::Cover,
             .radius = 8 * scale,
             .width = 38 * scale,
             .height = 38 * scale,
             .visible = false}
        )
    );
    row->addChild(std::move(artSlot));
    auto text = ui::column({.out = &m_mediaText, .align = FlexAlign::Stretch, .gap = 3 * scale, .flexGrow = 1.0F});
    text->addChild(
        ui::label(
            {.out = &m_track,
             .fontSize = 13 * scale,
             .fontWeight = FontWeight::SemiBold,
             .color = colorSpecFromRole(ColorRole::OnSurface)}
        )
    );
    text->addChild(
        ui::label({.out = &m_artist, .fontSize = 11 * scale, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)})
    );
    row->addChild(std::move(text));
    auto wave = std::make_unique<AudioVisualizer>();
    m_wave = wave.get();
    wave->setSize(28 * scale, 24 * scale);
    wave->setGradient(colorSpecFromRole(ColorRole::OnSurface), colorSpecFromRole(ColorRole::OnSurface));
    wave->setCentered(true);
    wave->setMirrored(false);
    wave->setRestAsDots(true);
    wave->setValues(std::vector<float>(5, 0.0F));
    row->addChild(std::move(wave));
    row->addChild(
        ui::glyph(
            {.out = &m_paused,
             .glyph = "media-pause",
             .glyphSize = 20 * scale,
             .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
             .visible = false}
        )
    );
    auto action = ui::button({
        .out = &m_mediaAction,
        .variant = ButtonVariant::Ghost,
        .tooltip = i18n::tr("control-center.media.now-playing"),
        .padding = 0.0F,
        .radius = 8 * scale,
        .participatesInLayout = false,
        .onClick = [this] { m_activate(m_mediaId); },
    });
    auto actionPalette = Button::defaultPalette(ButtonVariant::Ghost);
    for (auto* state : {&actionPalette.normal, &actionPalette.hover, &actionPalette.pressed}) {
      state->bg = clearColorSpec();
      state->border = clearColorSpec();
    }
    action->setCustomPalette(actionPalette);
    action->inputArea()->setTabFocusKey("control-center.overview.media");
    row->addChild(std::move(action));
    media->addChild(std::move(row));
    scroll->content()->addChild(std::move(media));
    root->addChild(std::move(scroll));
    setCurrent(m_current);
    return root;
  }

  void SectionOverview::setCurrent(int id) {
    m_current = id;
    for (std::size_t i = 0; i < m_buttons.size(); ++i) {
      const bool selected = m_sections[i].id == id;
      auto palette = Button::defaultPalette(ButtonVariant::Ghost);
      for (auto* state : {&palette.normal, &palette.hover, &palette.pressed}) {
        state->bg = clearColorSpec();
        state->border = clearColorSpec();
        state->label = colorSpecFromRole(selected ? ColorRole::Primary : ColorRole::OnSurface);
      }
      palette.pressed.label = colorSpecFromRole(selected ? ColorRole::Primary : ColorRole::OnSurface, 0.6F);
      m_buttons[i]->setCustomPalette(palette);
      m_buttons[i]->label()->setFontWeight(selected ? FontWeight::SemiBold : FontWeight::Normal);
    }
  }

  void SectionOverview::setShown(int id, bool shown) {
    for (std::size_t i = 0; i < m_buttons.size(); ++i)
      if (m_sections[i].id == id)
        m_buttons[i]->setVisible(shown);
  }

  void SectionOverview::focusButton(Button* button) {
    PanelManager::instance().inputDispatcher().setFocus(button->inputArea());
    scrollNodeIntoScrollView(*m_scroll, nullptr, *button, 2 * contentScale());
    PanelManager::instance().requestLayout();
    PanelManager::instance().requestRedraw();
  }

  void SectionOverview::focusCurrent() {
    for (std::size_t i = 0; i < m_buttons.size(); ++i)
      if (m_sections[i].id == m_current && m_buttons[i]->visible()) {
        focusButton(m_buttons[i]);
        return;
      }
  }

  bool SectionOverview::handleKey(std::uint32_t sym, std::uint32_t modifiers) {
    if (modifiers != 0
        || (!KeySymbol::isLeft(sym) && !KeySymbol::isRight(sym) && !KeySymbol::isUp(sym) && !KeySymbol::isDown(sym)))
      return false;
    std::vector<Button*> visible;
    for (auto* button : m_buttons)
      if (button->visible())
        visible.push_back(button);
    if (visible.empty())
      return false;
    const auto* focused = PanelManager::instance().inputDispatcher().focusedArea();
    auto current = std::ranges::find_if(visible, [focused](const Button* b) { return b->inputArea() == focused; });
    if (current == visible.end()) {
      focusCurrent();
      return true;
    }
    const auto columns = static_cast<int>(m_grid->effectiveColumns(m_grid->width(), visible.size()));
    const int step = KeySymbol::isLeft(sym) ? -1
        : KeySymbol::isRight(sym)           ? 1
        : KeySymbol::isUp(sym)              ? -columns
                                            : columns;
    const int index = static_cast<int>(std::distance(visible.begin(), current));
    const int next = std::clamp(index + step, 0, static_cast<int>(visible.size()) - 1);
    focusButton(visible[static_cast<std::size_t>(next)]);
    return true;
  }

  void SectionOverview::doLayout(Renderer& renderer, float width, float height) {
    // GridView retains its last arranged height; remeasure its rows when width
    // or available sections change instead of stretching the previous row count.
    m_grid->setSize(std::max(1.0F, width - 4 * contentScale()), 0.0F);
    m_root->setSize(width, height);
    m_root->layout(renderer);
    if (m_media->visible()) {
      const float available = std::max(1.0F, m_mediaRow->width() - 100 * contentScale());
      m_mediaText->setMaxWidth(available);
      m_track->setMaxWidth(available);
      m_artist->setMaxWidth(available);
      m_mediaAction->setSize(m_mediaRow->width(), m_mediaRow->height());
      m_mediaAction->setControlHeight(m_mediaRow->height());
      m_mediaAction->layout(renderer);
    }
  }

  void SectionOverview::doUpdate(Renderer& renderer) {
    if (!m_root)
      return;
    const auto player = m_mpris ? m_mpris->activePlayer() : std::nullopt;
    const bool hasMedia = player && (player->playbackStatus == "Playing" || player->playbackStatus == "Paused");
    if (m_media->visible() != hasMedia) {
      m_media->setVisible(hasMedia);
      PanelManager::instance().requestLayout();
    }
    m_playing = hasMedia && player->playbackStatus == "Playing";
    syncSpectrum();
    if (!hasMedia)
      return;
    m_track->setText(player->title.empty() ? player->identity : player->title);
    const auto artists = joinedArtists(player->artists);
    m_artist->setText(artists.empty() ? player->identity : artists);
    m_wave->setVisible(m_playing);
    m_paused->setVisible(!m_playing);
    const auto path = mpris::resolveArtworkSource(
        m_http, m_pendingArt, mpris::effectiveArtUrl(*player),
        [this] {
          if (m_active)
            PanelManager::instance().refresh();
        },
        m_alive
    );
    if (path != m_artPath || (!path.empty() && !m_art->hasImage())) {
      m_artPath = path;
      if (path.empty()
          || !m_art->setSourceFile(renderer, path, static_cast<int>(std::ceil(76 * contentScale())), true, true))
        m_art->clear(renderer);
    }
    m_art->setVisible(m_art->hasImage());
    m_artFallback->setVisible(!m_art->hasImage());
  }

  void SectionOverview::setActive(bool active) {
    m_active = active;
    syncSpectrum();
  }

  void SectionOverview::syncSpectrum() {
    const bool listen = m_active && m_playing && m_wave && m_spectrum;
    if (listen && !m_spectrumListener) {
      m_spectrumListener = m_spectrum->addChangeListener(5, [] { PanelManager::instance().requestFrameTick(); });
      PanelManager::instance().requestFrameTick();
    } else if (!listen && m_spectrumListener) {
      m_spectrum->removeChangeListener(m_spectrumListener);
      m_spectrumListener = 0;
    }
  }

  void SectionOverview::onFrameTick(float deltaMs) {
    if (!m_active || !m_wave || !m_spectrumListener)
      return;
    m_wave->setValues(m_spectrum->values(m_spectrumListener));
    m_wave->setSmoothingTimeMs(MotionService::instance().enabled() ? 60.0F : 0.0F);
    const bool changed = !m_wave->converged();
    m_wave->tick(deltaMs);
    if (changed)
      PanelManager::instance().requestRedraw();
    if (!m_wave->converged() || !m_spectrum->idle())
      PanelManager::instance().requestFrameTick();
  }

  void SectionOverview::onClose() {
    setActive(false);
    m_root = nullptr;
    m_scroll = nullptr;
    m_grid = nullptr;
    m_media = nullptr;
    m_mediaRow = nullptr;
    m_mediaText = nullptr;
    m_mediaAction = nullptr;
    m_art = nullptr;
    m_artFallback = nullptr;
    m_paused = nullptr;
    m_track = nullptr;
    m_artist = nullptr;
    m_wave = nullptr;
    m_buttons.clear();
    m_artPath.clear();
    m_playing = false;
  }

} // namespace control_center
