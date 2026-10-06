#include "shell/desktop/widgets/desktop_media_player_widget.h"

#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <cmath>

using namespace mpris;

namespace {

  constexpr float kArtSize = 120.0F;
  constexpr float kControlSize = 32.0F;
  constexpr float kPlayPauseSize = 40.0F;
  constexpr float kSpacing = 6.0F;

} // namespace

namespace {

  constexpr float kShadowAlpha = 0.6F;
  constexpr float kShadowOffset = 1.5F;

} // namespace

DesktopMediaPlayerWidget::DesktopMediaPlayerWidget(MprisService* mpris, HttpClient* httpClient, Options options)
    : m_cardSize(options.cardSize), m_mpris(mpris), m_httpClient(httpClient), m_vertical(options.vertical),
      m_color(options.color), m_shadow(options.shadow), m_hideWhenNoMedia(options.hideWhenNoMedia) {}

DesktopMediaPlayerWidget::~DesktopMediaPlayerWidget() { m_aliveGuard.reset(); }

void DesktopMediaPlayerWidget::create() {
  auto rootNode = ui::node({});

  rootNode->addChild(
      ui::box(
          {.out = &m_artPlaceholder,
           .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
           .radius = Style::scaledRadiusLg()}
      )
  );
  rootNode->addChild(
      ui::glyph({.out = &m_musicGlyph, .glyph = "music", .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)})
  );
  m_artPlaceholder->setVisible(usesCardLayout());
  m_musicGlyph->setVisible(usesCardLayout());
  auto artwork = ui::image({
      .out = &m_artwork,
      .fit = ImageFit::Cover,
      .radius = Style::scaledRadiusMd(contentScale()),
  });
  rootNode->addChild(std::move(artwork));

  auto title = ui::label({
      .out = &m_title,
      .fontWeight = FontWeight::Bold,
      .color = m_color,
      .maxLines = 1,
  });
  rootNode->addChild(std::move(title));

  auto artist = ui::label({
      .out = &m_artist,
      .color = m_color,
      .maxLines = 1,
  });
  rootNode->addChild(std::move(artist));

  auto controls = ui::row(
      {
          .out = &m_controls,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
      },
      ui::button({
          .out = &m_prev,
          .glyph = "media-prev",
          .variant = ButtonVariant::Ghost,
          .onClick =
              [this]() {
                if (m_mpris != nullptr) {
                  m_mpris->previousActive();
                  requestRedraw();
                }
              },
      }),
      ui::button({
          .out = &m_playPause,
          .glyph = "media-play",
          .variant = ButtonVariant::Primary,
          .onClick =
              [this]() {
                if (m_mpris != nullptr) {
                  m_mpris->playPauseActive();
                  requestRedraw();
                }
              },
      }),
      ui::button({
          .out = &m_next,
          .glyph = "media-next",
          .variant = ButtonVariant::Ghost,
          .onClick = [this]() {
            if (m_mpris != nullptr) {
              m_mpris->nextActive();
              requestRedraw();
            }
          },
      })
  );

  rootNode->addChild(std::move(controls));
  rootNode->addChild(
      ui::label(
          {.out = &m_sourceLabel,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1}
      )
  );
  rootNode->addChild(ui::progressBar({.out = &m_progress}));
  rootNode->addChild(
      ui::label({.out = &m_elapsed, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  );
  rootNode->addChild(
      ui::label({.out = &m_duration, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1})
  );
  m_sourceLabel->setVisible(usesCardLayout());
  m_progress->setVisible(false);
  m_elapsed->setVisible(false);
  m_duration->setVisible(false);
  setRoot(std::move(rootNode));
  applyShadow();
}

bool DesktopMediaPlayerWidget::applySetting(
    const std::string& key, const WidgetSettingValue& value,
    const std::unordered_map<std::string, WidgetSettingValue>& allSettings, Renderer& renderer
) {
  if (key == "color") {
    if (const auto* v = std::get_if<std::string>(&value)) {
      m_color = colorSpecFromConfigString(*v, key);
      if (m_title != nullptr)
        m_title->setColor(m_color);
      if (m_artist != nullptr)
        m_artist->setColor(m_color);
      return true;
    }
    return false;
  }
  if (key == "shadow") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_shadow = *v;
      applyShadow();
      return true;
    }
    return false;
  }
  if (key == "hide_when_no_media") {
    if (const auto* v = std::get_if<bool>(&value)) {
      m_hideWhenNoMedia = *v;
      if (applyVisibility()) {
        requestLayout();
      }
      return true;
    }
    return false;
  }
  return DesktopWidget::applySetting(key, value, allSettings, renderer);
}

void DesktopMediaPlayerWidget::onFontFamilyChanged(const std::string& family, Renderer& /*renderer*/) {
  if (m_sourceLabel != nullptr)
    m_sourceLabel->setFontFamily(family);
  if (m_elapsed != nullptr)
    m_elapsed->setFontFamily(family);
  if (m_duration != nullptr)
    m_duration->setFontFamily(family);
  if (m_title != nullptr) {
    m_title->setFontFamily(family);
  }
  if (m_artist != nullptr) {
    m_artist->setFontFamily(family);
  }
}

void DesktopMediaPlayerWidget::setEditorPreview(bool enabled) noexcept {
  if (m_editorPreview == enabled) {
    return;
  }
  m_editorPreview = enabled;
  if (root() == nullptr) {
    return;
  }
  if (applyVisibility()) {
    requestLayout();
  } else if (enabled && m_visible) {
    requestRedraw();
  }
}

void DesktopMediaPlayerWidget::doLayout(Renderer& renderer) {
  if (root() == nullptr || m_artwork == nullptr || m_title == nullptr || m_artist == nullptr || m_controls == nullptr)
    return;

  applyVisibility();
  sync(renderer);
  applyShadow();

  if (usesCardLayout()) {
    layoutCard(renderer);
    return;
  }
  const float scale = contentScale();
  if (m_vertical) {
    layoutVertical(renderer, scale);
  } else {
    layoutHorizontal(renderer, scale);
  }
}

void DesktopMediaPlayerWidget::layoutVertical(Renderer& renderer, float scale) {
  const float artW = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;

  m_artwork->setSize(artW, artW);
  m_artwork->setRadius(Style::scaledRadiusMd(scale));
  m_artwork->setPosition(0.0F, 0.0F);

  m_title->setFontSize(fontSize);
  m_title->setMaxWidth(artW);
  m_title->measure(renderer);
  m_title->setPosition(0.0F, artW + spacing);

  m_artist->setFontSize(fontSize * 0.9F);
  m_artist->setMaxWidth(artW);
  m_artist->measure(renderer);
  const float artistY = m_title->y() + m_title->height() + spacing * 0.5F;
  m_artist->setPosition(0.0F, artistY);

  layoutButtons(renderer, scale);

  const float controlsY =
      (m_artist->visible() ? m_artist->y() + m_artist->height() : m_title->y() + m_title->height()) + spacing;
  const float controlsX = std::round((artW - m_controls->width()) * 0.5F);
  m_controls->setPosition(controlsX, controlsY);

  root()->setSize(artW, controlsY + m_controls->height());
}

void DesktopMediaPlayerWidget::layoutHorizontal(Renderer& renderer, float scale) {
  const float artH = kArtSize * scale;
  const float spacing = kSpacing * scale;
  const float fontSize = Style::fontSizeBody * scale;
  const float textWidth = artH * 1.5F;

  m_artwork->setSize(artH, artH);
  m_artwork->setRadius(Style::scaledRadiusMd(scale));
  m_artwork->setPosition(0.0F, 0.0F);

  const float textX = artH + spacing;
  const float totalWidth = textX + textWidth;

  m_title->setFontSize(fontSize);
  m_title->setMaxWidth(textWidth);
  m_title->measure(renderer);

  m_artist->setFontSize(fontSize * 0.9F);
  m_artist->setMaxWidth(textWidth);
  m_artist->measure(renderer);

  layoutButtons(renderer, scale);

  const float titleH = m_title->height();
  const float artistGap = m_artist->visible() ? spacing * 0.5F : 0.0F;
  const float artistH = m_artist->visible() ? m_artist->height() : 0.0F;
  const float controlsH = m_controls->height();
  const float textAreaH = std::max(0.0F, artH - controlsH - spacing);
  const float textBlockH = titleH + artistGap + artistH;
  const float textY = std::round(std::max(0.0F, (textAreaH - textBlockH) * 0.5F));

  m_title->setPosition(textX, textY);
  m_artist->setPosition(textX, textY + titleH + artistGap);

  const float controlsY = artH - controlsH;
  const float controlsX = totalWidth - m_controls->width();
  m_controls->setPosition(controlsX, controlsY);

  root()->setSize(totalWidth, artH);
}

void DesktopMediaPlayerWidget::layoutButtons(Renderer& renderer, float scale) {
  const float controlBtnSize = kControlSize * scale;
  const float playPauseBtnSize = kPlayPauseSize * scale;
  const float glyphSize = Style::fontSizeBody * scale;
  const float playPauseGlyphSize = Style::fontSizeBody * 1.2F * scale;

  m_controls->setGap(Style::spaceXs * scale);
  m_controls->setJustify(m_vertical ? FlexJustify::Center : FlexJustify::End);

  m_prev->setMinWidth(controlBtnSize);
  m_prev->setMinHeight(controlBtnSize);
  m_prev->setGlyphSize(glyphSize);
  m_prev->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
  m_prev->setRadius(Style::scaledRadiusMd(scale));

  m_playPause->setMinWidth(playPauseBtnSize);
  m_playPause->setMinHeight(playPauseBtnSize);
  m_playPause->setGlyphSize(playPauseGlyphSize);
  m_playPause->setPadding(Style::spaceSm * scale, Style::spaceSm * scale);
  m_playPause->setRadius(Style::scaledRadiusLg(scale));

  m_next->setMinWidth(controlBtnSize);
  m_next->setMinHeight(controlBtnSize);
  m_next->setGlyphSize(glyphSize);
  m_next->setPadding(Style::spaceXs * scale, Style::spaceXs * scale);
  m_next->setRadius(Style::scaledRadiusMd(scale));

  m_controls->layout(renderer);
  m_prev->updateInputArea();
  m_playPause->updateInputArea();
  m_next->updateInputArea();
}

void DesktopMediaPlayerWidget::doUpdate(Renderer& renderer) {
  if (applyVisibility()) {
    requestLayout();
  }
  sync(renderer);
  updateProgress();
}

void DesktopMediaPlayerWidget::sync(Renderer& renderer) {
  if (m_title == nullptr || m_artist == nullptr || m_playPause == nullptr)
    return;

  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;

  std::string title;
  std::string artist;
  std::string artUrl;
  std::string playbackStatus;
  bool canGoPrevious = false;
  bool canGoNext = false;

  if (active.has_value()) {
    title = active->title;
    artist = joinArtists(active->artists);
    artUrl = effectiveArtUrl(*active);
    playbackStatus = active->playbackStatus;
    canGoPrevious = active->canControl && active->canGoPrevious;
    canGoNext = active->canControl && active->canGoNext;
  }

  const bool canPlayPause =
      active && active->canControl && (playbackStatus == "Playing" ? active->canPause : active->canPlay);
  const std::string identity = active ? active->identity : std::string();
  const bool titleChanged = title != m_lastTitle;
  const bool artistChanged = artist != m_lastArtist;
  const bool artChanged = artUrl != m_lastArtUrl;
  const bool statusChanged = playbackStatus != m_lastPlaybackStatus;
  const bool canGoPreviousChanged = canGoPrevious != m_lastCanGoPrevious;
  const bool canGoNextChanged = canGoNext != m_lastCanGoNext;
  const bool artAwaitingDecode = m_artwork != nullptr && !artUrl.empty() && !m_artwork->hasImage();
  if (m_syncInitialized
      && canPlayPause == m_lastCanPlayPause
      && identity == m_lastIdentity
      && !titleChanged
      && !artistChanged
      && !artChanged
      && !statusChanged
      && !canGoPreviousChanged
      && !canGoNextChanged
      && !artAwaitingDecode) {
    return;
  }

  const bool firstSync = !m_syncInitialized;
  m_syncInitialized = true;
  m_lastCanPlayPause = canPlayPause;
  m_lastIdentity = identity;
  m_lastTitle = title;
  m_lastArtist = artist;
  m_lastArtUrl = artUrl;
  m_lastPlaybackStatus = playbackStatus;
  m_lastCanGoPrevious = canGoPrevious;
  m_lastCanGoNext = canGoNext;

  m_title->setText(m_lastTitle.empty() ? i18n::tr("desktop-widgets.media.nothing-playing") : m_lastTitle);
  m_artist->setText(!active && usesCardLayout() ? i18n::tr("desktop-widgets.media.start-playback") : m_lastArtist);
  m_artist->setVisible(!m_lastArtist.empty() || (!active && usesCardLayout()));
  m_sourceLabel->setText(identity.empty() ? i18n::tr("desktop-widgets.media.now-playing") : identity);
  m_playPause->setEnabled(canPlayPause);

  m_playPause->setGlyph(m_lastPlaybackStatus == "Playing" ? "media-pause" : "media-play");
  if (m_prev != nullptr) {
    m_prev->setVisible(usesCardLayout() || canGoPrevious);
    m_prev->setEnabled(canGoPrevious);
  }
  if (m_next != nullptr) {
    m_next->setVisible(usesCardLayout() || canGoNext);
    m_next->setEnabled(canGoNext);
  }

  if (m_artwork != nullptr) {
    const int targetPx = static_cast<int>(std::round((usesCardLayout() ? 432.0F : kArtSize) * contentScale()));
    if (artChanged) {
      const std::string artPath = resolveArtworkSource(
          m_httpClient, m_pendingArtDownloads, m_lastArtUrl, [this] { requestUpdate(); }, m_aliveGuard
      );
      if (!artPath.empty()) {
        if (!m_artwork->setSourceFile(renderer, artPath, targetPx, true, true))
          m_artwork->clear(renderer);
      } else {
        m_artwork->clear(renderer);
      }
    } else if (!m_lastArtUrl.empty() && !m_artwork->hasImage()) {
      const std::string artPath = cachedArtworkPath(m_lastArtUrl);
      if (!artPath.empty() && m_artwork->setSourceFile(renderer, artPath, targetPx, true, true))
        requestRedraw();
    }
  }

  m_musicGlyph->setVisible(usesCardLayout() && (m_artwork == nullptr || !m_artwork->hasImage()));
  if ((firstSync || titleChanged || artistChanged || canGoPreviousChanged || canGoNextChanged) && !isLayingOut()) {
    requestLayout();
  } else {
    requestRedraw();
  }
}

void DesktopMediaPlayerWidget::applyShadow() {
  if (m_title == nullptr || m_artist == nullptr) {
    return;
  }
  if (m_shadow && !usesCardLayout()) {
    const float offset = kShadowOffset * contentScale();
    const ColorSpec shadow = colorSpecFromRole(ColorRole::Shadow, kShadowAlpha);
    m_title->setShadow(shadow, offset, offset);
    m_artist->setShadow(shadow, offset, offset);
  } else {
    m_title->clearShadow();
    m_artist->clearShadow();
  }
}

bool DesktopMediaPlayerWidget::hasActiveMedia() const {
  return m_mpris != nullptr && m_mpris->activePlayer().has_value();
}

bool DesktopMediaPlayerWidget::shouldBeVisible() const {
  return m_editorPreview || !m_hideWhenNoMedia || hasActiveMedia();
}

bool DesktopMediaPlayerWidget::applyVisibility() {
  if (presentationRoot() == nullptr) {
    return false;
  }
  const bool nextVisible = shouldBeVisible();
  if (!m_visibilityInitialized) {
    m_visibilityInitialized = true;
    m_visible = nextVisible;
    presentationRoot()->setOpacity(m_visible ? 1.0F : 0.0F);
    setVisibilityCollapsed(!m_visible);
    return !m_visible;
  }

  if (m_visible == nextVisible) {
    return false;
  }

  m_visible = nextVisible;
  presentationRoot()->setOpacity(m_visible ? 1.0F : 0.0F);
  setVisibilityCollapsed(!m_visible);
  return true;
}

void DesktopMediaPlayerWidget::setVisibilityCollapsed(bool collapsed) {
  if (Node* node = presentationRoot(); node != nullptr) {
    node->setVisible(!collapsed);
  }
}

bool DesktopMediaPlayerWidget::wantsSecondTicks() const {
  return !usesCardLayout() || (m_visible && m_showProgress && m_lastPlaybackStatus == "Playing");
}

void DesktopMediaPlayerWidget::updateProgress() {
  if (m_progress == nullptr)
    return;
  const auto active = m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
  const bool show = m_showProgress && active && active->lengthUs > 0;
  m_progress->setVisible(show);
  m_elapsed->setVisible(show);
  m_duration->setVisible(show);
  if (!show)
    return;
  const auto position = std::clamp(m_mpris->positionActive().value_or(0), std::int64_t{0}, active->lengthUs);
  m_progress->setProgress(static_cast<float>(position) / static_cast<float>(active->lengthUs));
  m_elapsed->setText(formatClockTime(position / 1000000));
  m_duration->setText(formatClockTime(active->lengthUs / 1000000));
}

void DesktopMediaPlayerWidget::layoutCard(Renderer& renderer) {
  const auto card =
      desktop_cards::resolve(m_cardSize, contentScale(), boxInnerWidth(), boxInnerHeight(), backgroundPadding());
  const float scale = card.scale;
  const bool large = card.size == desktop_cards::Size::Large;
  const bool medium = card.size == desktop_cards::Size::Medium;
  const float artSize =
      large ? std::min(card.width * 0.65F, card.height - 180.0F * scale) : (medium ? 144.0F : 80.0F) * scale;
  const float artX = large ? (card.width - artSize) * 0.5F : 0.0F;
  const float artY = 28.0F * scale;
  auto place = [&](Node* node, float x, float y) {
    node->setPosition(Style::rtl() ? card.width - x - node->width() : x, y);
  };
  m_artPlaceholder->setSize(artSize, artSize);
  m_artPlaceholder->setRadius(Style::scaledRadiusLg(scale));
  m_artwork->setSize(artSize, artSize);
  m_artwork->setRadius(Style::scaledRadiusLg(scale));
  place(m_artPlaceholder, artX, artY);
  place(m_artwork, artX, artY);
  m_musicGlyph->setGlyphSize(artSize * 0.35F);
  m_musicGlyph->layout(renderer);
  place(
      m_musicGlyph, artX + (artSize - m_musicGlyph->width()) * 0.5F, artY + (artSize - m_musicGlyph->height()) * 0.5F
  );
  m_musicGlyph->setVisible(!m_artwork->hasImage());
  auto label = [&](Label* node, float x, float y, float width, float fontSize, int lines = 1) {
    node->setFontSize(fontSize * scale);
    node->setMinWidth(width);
    node->setMaxWidth(width);
    node->setMaxLines(lines);
    node->setTextAlign(large ? TextAlign::Center : TextAlign::Start);
    node->measure(renderer);
    place(node, x, y);
  };
  label(m_sourceLabel, 0.0F, 0.0F, card.width, Style::fontSizeCaption);
  const float textX = large ? 0.0F : artSize + 12.0F * scale;
  const float textWidth = card.width - textX;
  const float titleY = large ? artY + artSize + 12.0F * scale : 40.0F * scale;
  label(
      m_title, textX, titleY, textWidth, large || medium ? Style::fontSizeHeader : Style::fontSizeBody, large ? 1 : 2
  );
  label(m_artist, textX, large ? titleY + 28.0F * scale : 86.0F * scale, textWidth, Style::fontSizeCaption);
  layoutButtons(renderer, scale);
  const float controlsX = medium ? textX : (card.width - m_controls->width()) * 0.5F;
  place(m_controls, controlsX, large ? card.height - 90.0F * scale : card.height - m_controls->height());
  m_showProgress = large;
  updateProgress();
  m_progress->setSize(card.width, 3.0F * scale);
  place(m_progress, 0.0F, card.height - 27.0F * scale);
  label(m_elapsed, 0.0F, card.height - 20.0F * scale, card.width * 0.5F, Style::fontSizeMini);
  label(m_duration, card.width * 0.5F, card.height - 20.0F * scale, card.width * 0.5F, Style::fontSizeMini);
  m_elapsed->setTextAlign(TextAlign::Start);
  m_duration->setTextAlign(TextAlign::End);
  m_elapsed->measure(renderer);
  m_duration->measure(renderer);
  root()->setSize(card.width, card.height);
}
