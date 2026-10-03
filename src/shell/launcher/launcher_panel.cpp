#include "shell/launcher/launcher_panel.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/ui_phase.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "launcher/app_provider.h"
#include "notification/notifications.h"
#include "render/core/async_texture_cache.h"
#include "render/core/renderer.h"
#include "render/scene/node.h"
#include "shell/dock/pinned_apps.h"
#include "shell/panel/panel_content_height.h"
#include "shell/panel/panel_manager.h"
#include "system/desktop_entry.h"
#include "ui/app_icon_colorization.h"
#include "ui/builders.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/image.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/style.h"
#include "util/fuzzy_match.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <tuple>

namespace {

  constexpr std::size_t kRowOverscan = 3;
  // Minimum trimmed query length before prefixed opt-in providers join the global search.
  constexpr std::size_t kGlobalOptInMinChars = 2;
  constexpr float kIconSizeDefault = 40.0F;
  constexpr float kIconSizeCompact = 28.0F;
  constexpr float kListIconSizeDefault = 22.0F;
  constexpr float kListIconSizeCompact = 22.0F;
  constexpr std::size_t kAppGridColumns = 5;
  constexpr std::string_view kApplicationsProviderId = "Applications";
  constexpr double kUsageScorePerCount = 0.1;
  constexpr double kTypedUsageScoreCap = 0.5;
  constexpr std::string_view kProviderOverviewProviderId = "__launcher_provider_overview__";
  constexpr std::string_view kProviderOverviewResultPrefix = "provider:";
  constexpr std::string_view kAliasEditorProviderId = "__launcher_alias_editor__";
  constexpr std::string_view kFormProviderId = "__launcher_form__";
  constexpr std::string_view kFormSubmitId = "submit";

  // Multiline form values are shown and typed in the single-line field with "\n" for a newline.
  [[nodiscard]] std::string escapeNewlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      if (c == '\n') {
        out += "\\n";
      } else if (c != '\r') {
        out.push_back(c);
      }
    }
    return out;
  }

  [[nodiscard]] std::string unescapeNewlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
        out.push_back('\n');
        ++i;
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }
  // An alias typed exactly puts its result above everything else, the calculator included.
  constexpr double kAliasScore = 1e9;

  double usageBoostForScore(double score, int usageCount, bool typedQuery) {
    if (usageCount <= 0) {
      return 0.0;
    }

    const double rawBoost = static_cast<double>(usageCount) * kUsageScorePerCount;
    if (!typedQuery) {
      return rawBoost;
    }
    if (!FuzzyMatch::isMatch(score)) {
      return 0.0;
    }

    // For typed searches, usage should nudge close matches without letting a
    // weak fuzzy hit outrank a much stronger lexical match.
    return std::min(rawBoost, kTypedUsageScoreCap);
  }

  [[nodiscard]] bool isDescendantOf(const Node* node, const Node* ancestor) {
    if (node == nullptr || ancestor == nullptr) {
      return false;
    }
    for (const Node* current = node; current != nullptr; current = current->parent()) {
      if (current == ancestor) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] std::string singleLinePreview(std::string_view text) {
    std::string preview;
    preview.reserve(text.size());
    bool lastWasSpace = false;
    for (const char c : text) {
      const bool whitespace = c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v';
      if (whitespace) {
        if (!lastWasSpace) {
          preview.push_back(' ');
          lastWasSpace = true;
        }
        continue;
      }
      preview.push_back(c);
      lastWasSpace = c == ' ';
    }
    return preview;
  }

  [[nodiscard]] bool isDetailPresentation(const LauncherResult& result) { return result.presentation == "detail"; }

  [[nodiscard]] std::string providerOverviewId(std::string_view prefix) {
    std::string id(kProviderOverviewResultPrefix);
    id += prefix;
    return id;
  }

  void sortResultsByScore(std::vector<LauncherResult>& results) {
    std::ranges::stable_sort(results, std::ranges::greater{}, &LauncherResult::score);
  }

  struct LauncherListStyle {
    float scale = 1.0F;
    bool showIcons = true;
    bool showAppOriginIndicator = true;
    bool compact = false;
    std::optional<ColorSpec> appIconColorizeTint;
    std::optional<ColorSpec> listItemBackground;
  };

  [[nodiscard]] float launcherIconSize(const LauncherListStyle& style) {
    return (style.compact ? kIconSizeCompact : kIconSizeDefault) * style.scale;
  }

  // Raycast rows are a single line with a small icon.
  [[nodiscard]] float launcherListIconSize(const LauncherListStyle& style) {
    return (style.compact ? kListIconSizeCompact : kListIconSizeDefault) * style.scale;
  }

  [[nodiscard]] float launcherListPaddingY(const LauncherListStyle& style) {
    return (style.compact ? Style::spaceXs * 1.5F : Style::spaceSm) * style.scale;
  }

  // Room above the first row of a section for its caption.
  [[nodiscard]] float launcherSectionHeaderHeight(const LauncherListStyle& style) {
    return std::ceil((Style::fontSizeCaption * 1.35F + Style::spaceSm + Style::spaceXs) * style.scale);
  }

  [[nodiscard]] float stableLabelHeight(const TextMetrics& metrics) { return std::round(metrics.bottom - metrics.top); }

  [[nodiscard]] float launcherTextStackHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float bodySize = Style::fontSizeBody * style.scale;
    return stableLabelHeight(renderer.measureFont(bodySize, FontWeight::Medium));
  }

  [[nodiscard]] float launcherRowHeight(Renderer& renderer, const LauncherListStyle& style) {
    const float paddingY = launcherListPaddingY(style);
    const float textHeight = launcherTextStackHeight(renderer, style);
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherListIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] float launcherRowHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = launcherListPaddingY(style);
    const float textHeight = Style::fontSizeBody * style.scale * 1.25F;
    if (!style.showIcons) {
      return std::ceil(textHeight + paddingY * 2.0F);
    }
    return std::ceil(std::max(launcherListIconSize(style), textHeight) + paddingY * 2.0F);
  }

  [[nodiscard]] float launcherAppGridLabelHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float fontSize = Style::fontSizeCaption * style.scale;
    const TextMetrics metrics =
        renderer.measureText("Ag\nyg", fontSize, FontWeight::Normal, wrapWidth, 2, TextAlign::Center);
    const float actualHeight = metrics.bottom - metrics.top;
    const float inkSpan = std::max(0.0F, metrics.inkBottom - metrics.inkTop);
    const float rowExtent = renderer.fontRowExtent(fontSize, FontWeight::Normal);
    return std::ceil(std::max({actualHeight, inkSpan, rowExtent * 2.0F}));
  }

  [[nodiscard]] float launcherAppGridCellHeight(Renderer& renderer, const LauncherListStyle& style, float wrapWidth) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = launcherAppGridLabelHeight(renderer, style, wrapWidth);
    return std::ceil(paddingY * 2.0F + iconSize + gap + labelHeight);
  }

  [[nodiscard]] float launcherAppGridCellHeightEstimate(const LauncherListStyle& style) {
    const float paddingY = Style::spaceSm * style.scale;
    const float gap = Style::spaceXs * style.scale;
    const float iconSize = launcherIconSize(style);
    const float labelHeight = Style::fontSizeCaption * style.scale * 2.4F;
    return std::ceil(paddingY * 2.0F + iconSize + gap + labelHeight);
  }

  [[nodiscard]] LauncherListStyle launcherListStyleFrom(const ConfigService* config, float scale, float cardOpacity) {
    LauncherListStyle style{.scale = scale, .appIconColorizeTint = std::nullopt, .listItemBackground = std::nullopt};
    if (config != nullptr) {
      const auto& launcher = config->config().shell.launcher;
      style.showIcons = launcher.showIcons;
      style.showAppOriginIndicator = launcher.showAppOriginIndicator;
      style.compact = launcher.compact;
      style.appIconColorizeTint = effectiveShellAppIconColorizationTint(config->config().shell);
      if (config->config().shell.panel.listItemBackground) {
        style.listItemBackground = colorSpecFromRole(ColorRole::SurfaceVariant, cardOpacity);
      }
    }
    return style;
  }

  class LauncherResultRow final : public Node {
  public:
    LauncherResultRow(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float iconSize = launcherListIconSize(m_style);
      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float paddingV = launcherListPaddingY(m_style);
      addChild(
          ui::label({
              .out = &m_sectionLabel,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
          })
      );
      auto row = ui::row(
          {.out = &m_row,
           .align = FlexAlign::Center,
           .gap = gap,
           .paddingV = paddingV,
           .paddingH = Style::spaceSm * m_style.scale,
           .radius = Style::scaledRadiusMd(m_style.scale)}
      );
      addChild(std::move(row));

      m_row->addChild(
          ui::label({
              .out = &m_badgeLabel,
              .fontSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::image({
              .out = &m_image,
              .width = iconSize,
              .height = iconSize,
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = iconSize,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (!m_style.showIcons
            || m_badgeVisible
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      // One line, Raycast style: the title, then its subtitle in a quieter colour.
      m_row->addChild(
          ui::row(
              {
                  .out = &m_textCol,
                  .align = FlexAlign::Center,
                  .gap = Style::spaceSm * m_style.scale,
                  .flexGrow = 1.0F,
              },
              ui::label({
                  .out = &m_title,
                  .fontSize = Style::fontSizeBody * m_style.scale,
                  .fontWeight = FontWeight::Medium,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              }),
              ui::label({
                  .out = &m_subtitle,
                  .fontSize = Style::fontSizeCaption * m_style.scale,
                  .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
                  .maxLines = 1,
                  .baselineMode = LabelBaselineMode::TextFixedHeight,
              })
          )
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );

      m_row->addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      );

      // Raycast shows a result's alias as a small tag beside it.
      auto aliasTag = ui::row({
          .out = &m_aliasTag,
          .align = FlexAlign::Center,
          .paddingV = 1.0F * m_style.scale,
          .paddingH = Style::spaceXs * 1.5F * m_style.scale,
          .fill = colorSpecFromRole(ColorRole::OnSurface, Style::hoverFillAlpha * 1.5F),
          .radius = Style::radiusSm * m_style.scale,
          .visible = false,
          .participatesInLayout = false,
      });
      aliasTag->addChild(
          ui::label({
              .out = &m_aliasLabel,
              .fontSize = Style::fontSizeMini * m_style.scale,
              .fontWeight = FontWeight::Medium,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
          })
      );
      m_row->addChild(std::move(aliasTag));

      m_row->addChild(
          ui::label({
              .out = &m_kindLabel,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 1,
              .visible = false,
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherListIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));
      m_badgeVisible = !result.badge.empty();

      // The grid sizes the tile to include the section header above the row.
      const bool hasSection = !result.section.empty();
      const float leading = hasSection ? std::min(height, launcherSectionHeaderHeight(m_style)) : 0.0F;
      const float rowHeight = height - leading;
      m_rowHeight = rowHeight;

      setSize(width, height);
      m_row->setPosition(0.0F, leading);
      m_row->setFrameSize(width, rowHeight);

      m_sectionLabel->setVisible(hasSection);
      if (hasSection) {
        const float captionSize = Style::fontSizeCaption * m_style.scale;
        const float labelHeight = stableLabelHeight(renderer.measureFont(captionSize, FontWeight::SemiBold));
        m_sectionLabel->setText(result.section);
        m_sectionLabel->setMaxWidth(std::max(0.0F, width - Style::spaceSm * m_style.scale * 2.0F));
        m_sectionLabel->setPosition(
            Style::spaceSm * m_style.scale, std::round(leading - Style::spaceXs * m_style.scale - labelHeight)
        );
      }

      m_badgeLabel->setVisible(false);
      m_badgeLabel->setParticipatesInLayout(false);
      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      const bool showAppIcon = m_style.showIcons && !m_badgeVisible;
      const bool showLeadingVisual = m_badgeVisible || showAppIcon;
      if (m_badgeVisible) {
        m_badgeLabel->setText(singleLinePreview(result.badge));
        m_badgeLabel->setSize(iconSize, iconSize);
        m_badgeLabel->setVisible(true);
        m_badgeLabel->setParticipatesInLayout(true);
        m_image->clear(renderer);
      } else if (showAppIcon) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0F;
      const float leadingWidth = showLeadingVisual ? iconSize + gap : 0.0F;
      const float pinnedWidth = result.pinned ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      m_pinnedGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setParticipatesInLayout(result.pinned);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(Style::fontSizeBody * m_style.scale);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setParticipatesInLayout(hasOrigin);
      const float originWidth = hasOrigin ? Style::fontSizeBody * m_style.scale + gap : 0.0F;
      const float captionSize = Style::fontSizeCaption * m_style.scale;
      const bool hasKind = !result.kind.empty();
      float kindWidth = 0.0F;
      if (hasKind) {
        m_kindLabel->setText(result.kind);
        kindWidth = std::ceil(renderer.measureText(result.kind, captionSize).width) + gap;
      }
      m_kindLabel->setVisible(hasKind);
      m_kindLabel->setParticipatesInLayout(hasKind);
      const bool hasAlias = !result.alias.empty();
      float aliasWidth = 0.0F;
      if (hasAlias) {
        m_aliasLabel->setText(result.alias);
        aliasWidth = std::ceil(
            renderer.measureText(result.alias, Style::fontSizeMini * m_style.scale, FontWeight::Medium).width
            + Style::spaceXs * 3.0F * m_style.scale
            + gap
        );
      }
      m_aliasTag->setVisible(hasAlias);
      m_aliasTag->setParticipatesInLayout(hasAlias);
      const float textWidth =
          std::max(0.0F, width - leadingWidth - pinnedWidth - originWidth - kindWidth - aliasWidth - horizontalPad);
      const std::string title = singleLinePreview(result.title);
      const float titleWidth = std::min(
          textWidth,
          std::ceil(renderer.measureText(title, Style::fontSizeBody * m_style.scale, FontWeight::Medium).width)
      );
      m_title->setText(title);
      m_title->setMaxWidth(textWidth);

      // The subtitle takes what the title leaves and drops out when that is too little to read.
      const float subtitleWidth = textWidth - titleWidth - Style::spaceSm * m_style.scale;
      const bool showSubtitle =
          !m_style.compact && !result.subtitle.empty() && subtitleWidth >= Style::controlHeightLg * m_style.scale;
      if (!showSubtitle) {
        m_subtitle->setVisible(false);
        m_subtitle->setParticipatesInLayout(false);
        m_subtitle->setText("");
      } else {
        m_subtitle->setVisible(true);
        m_subtitle->setParticipatesInLayout(true);
        m_subtitle->setText(singleLinePreview(result.subtitle));
        m_subtitle->setMaxWidth(subtitleWidth);
      }

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_badgeVisible || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      m_image->setSize(launcherListIconSize(m_style), launcherListIconSize(m_style));
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      if (m_style.showIcons && !m_badgeVisible && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      // Raycast marks the selection with a quiet grey fill and keeps the text colours.
      if (m_selected) {
        m_row->setFill(colorSpecFromRole(ColorRole::OnSurface, Style::pressedFillAlpha));
      } else if (m_hovered) {
        m_row->setFill(colorSpecFromRole(ColorRole::OnSurface, Style::hoverFillAlpha));
      } else {
        m_row->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_row->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_row->clearBorder();
      }

      const ColorSpec foreground = colorSpecFromRole(ColorRole::OnSurface);
      const ColorSpec mutedForeground = colorSpecFromRole(ColorRole::OnSurfaceVariant);
      m_badgeLabel->setColor(foreground);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_subtitle->setColor(mutedForeground);
      m_pinnedGlyph->setColor(mutedForeground);
      m_originGlyph->setColor(mutedForeground);
      m_kindLabel->setColor(mutedForeground);
    }

    LauncherListStyle m_style{};
    float m_rowHeight = 0.0F;
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_row = nullptr;
    Label* m_badgeLabel = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Flex* m_textCol = nullptr;
    Label* m_title = nullptr;
    Label* m_subtitle = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    Label* m_sectionLabel = nullptr;
    Label* m_kindLabel = nullptr;
    Flex* m_aliasTag = nullptr;
    Label* m_aliasLabel = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_badgeVisible = false;
    bool m_reorderTarget = false;
  };

  class LauncherAppGridTile final : public Node {
  public:
    LauncherAppGridTile(LauncherListStyle style, AsyncTextureCache* asyncTextures)
        : m_style(style), m_asyncTextures(asyncTextures) {
      const float gap = Style::spaceXs * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      auto col = ui::column({
          .out = &m_col,
          .align = FlexAlign::Center,
          .gap = gap,
          .paddingV = padding,
          .paddingH = padding,
          .radius = Style::scaledRadiusMd(m_style.scale),
          .fillWidth = true,
          .fillHeight = true,
      });
      addChild(std::move(col));

      addChild(
          ui::glyph({
              .out = &m_pinnedGlyph,
              .glyph = "pin-filled",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      addChild(
          ui::glyph({
              .out = &m_originGlyph,
              .glyph = "package",
              .glyphSize = Style::fontSizeBody * m_style.scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
              .participatesInLayout = false,
          })
      );

      m_col->addChild(
          ui::image({
              .out = &m_image,
              .visible = false,
          })
      );

      m_col->addChild(
          ui::glyph({
              .out = &m_glyph,
              .glyphSize = launcherIconSize(m_style),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .visible = false,
          })
      );

      m_image->setAsyncReadyCallback([this]() {
        if (!m_style.showIcons
            || m_iconPath.empty()
            || m_image == nullptr
            || m_glyph == nullptr
            || !m_image->hasImage()) {
          return;
        }
        m_image->setVisible(true);
        m_glyph->setVisible(false);
      });

      m_col->addChild(
          ui::label({
              .out = &m_title,
              .fontSize = Style::fontSizeCaption * m_style.scale,
              .fontWeight = FontWeight::Normal,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .configure = [](Label& label) { label.setTextAlign(TextAlign::Center); },
          })
      );
    }

    void setListStyle(LauncherListStyle style) { m_style = style; }
    void setReorderTarget(bool target) { m_reorderTarget = target; }

    void
    bind(Renderer& renderer, const LauncherResult& result, float width, float height, bool selected, bool hovered) {
      m_selected = selected;
      m_hovered = hovered;
      m_iconPath = result.iconPath;
      m_fallbackGlyph = result.glyphName.empty() ? "app-window" : result.glyphName;
      const float iconSize = launcherIconSize(m_style);
      m_iconTargetSize = static_cast<int>(std::round(iconSize));

      setSize(width, height);
      m_col->setSize(width, height);

      const float pinSize = Style::fontSizeBody * m_style.scale;
      const float padding = Style::spaceSm * m_style.scale;
      m_pinnedGlyph->setGlyphSize(pinSize);
      m_pinnedGlyph->setVisible(result.pinned);
      m_pinnedGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, padding);
      m_pinnedGlyph->setFrameSize(pinSize, pinSize);
      const bool hasOrigin = m_style.showAppOriginIndicator && !result.originGlyph.empty();
      if (hasOrigin) {
        m_originGlyph->setGlyph(result.originGlyph);
      }
      m_originGlyph->setGlyphSize(pinSize);
      m_originGlyph->setVisible(hasOrigin);
      m_originGlyph->setPosition(Style::rtl() ? padding : width - padding - pinSize, height - padding - pinSize);
      m_originGlyph->setFrameSize(pinSize, pinSize);

      m_image->setVisible(false);
      m_image->setParticipatesInLayout(false);
      m_glyph->setVisible(false);
      m_glyph->setParticipatesInLayout(false);

      if (m_style.showIcons) {
        m_image->setParticipatesInLayout(true);
        m_glyph->setParticipatesInLayout(true);
        m_image->setSize(iconSize, iconSize);
        m_glyph->setGlyphSize(iconSize);
        if (!m_iconPath.empty()) {
          const bool ready = refreshAsyncIcon(renderer);
          m_image->setVisible(ready);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(!ready);
        } else {
          m_image->clear(renderer);
          m_glyph->setGlyph(m_fallbackGlyph);
          m_glyph->setVisible(true);
        }
      } else {
        m_image->clear(renderer);
      }

      const float horizontalPad = Style::spaceSm * m_style.scale * 2.0F;
      const float textWidth = std::max(0.0F, width - horizontalPad);
      m_title->setText(singleLinePreview(result.title));
      m_title->setMaxWidth(textWidth);

      applyVisualState();
    }

    bool refreshAsyncIcon(Renderer& renderer) {
      if (!m_style.showIcons || m_iconPath.empty()) {
        m_image->setVisible(false);
        m_glyph->setVisible(false);
        return false;
      }

      m_image->setAppIconColorization(m_style.appIconColorizeTint);

      bool ready = false;
      if (m_asyncTextures != nullptr) {
        ready = m_image->setSourceFileAsync(renderer, *m_asyncTextures, m_iconPath, m_iconTargetSize, true);
      } else {
        ready = m_image->setSourceFile(renderer, m_iconPath, m_iconTargetSize, true);
      }

      const float iconSize = launcherIconSize(m_style);
      m_image->setSize(iconSize, iconSize);
      m_image->setVisible(ready);
      m_glyph->setGlyph(m_fallbackGlyph);
      m_glyph->setVisible(!ready);
      return ready;
    }

  protected:
    void doLayout(Renderer& renderer) override {
      m_col->setSize(width(), height());
      if (m_style.showIcons && !m_iconPath.empty()) {
        (void)refreshAsyncIcon(renderer);
      }
      Node::doLayout(renderer);
    }

  private:
    void applyVisualState() {
      if (m_selected) {
        m_col->setFill(colorSpecFromRole(ColorRole::Primary));
      } else if (m_hovered) {
        m_col->setFill(colorSpecFromRole(ColorRole::OnSurface, Style::hoverFillAlpha));
      } else {
        m_col->setFill(m_style.listItemBackground.value_or(clearColorSpec()));
      }
      if (m_reorderTarget) {
        m_col->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
      } else {
        m_col->clearBorder();
      }

      const auto activeRole = m_selected ? ColorRole::OnPrimary : ColorRole::OnSurface;
      const bool active = m_selected || m_hovered;
      const ColorSpec foreground = colorSpecFromRole(active ? activeRole : ColorRole::OnSurface);
      m_glyph->setColor(foreground);
      m_title->setColor(foreground);
      m_pinnedGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
      m_originGlyph->setColor(
          active ? colorSpecFromRole(activeRole, 0.7F) : colorSpecFromRole(ColorRole::OnSurfaceVariant)
      );
    }

    LauncherListStyle m_style{};
    bool m_selected = false;
    bool m_hovered = false;
    Flex* m_col = nullptr;
    Image* m_image = nullptr;
    Glyph* m_glyph = nullptr;
    Glyph* m_pinnedGlyph = nullptr;
    Glyph* m_originGlyph = nullptr;
    Label* m_title = nullptr;
    AsyncTextureCache* m_asyncTextures = nullptr;
    std::string m_iconPath;
    std::string m_fallbackGlyph;
    int m_iconTargetSize = 0;
    bool m_reorderTarget = false;
  };

} // namespace

class LauncherResultAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherResultAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] float itemLeadingSpace(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].section.empty()) {
      return 0.0F;
    }
    return launcherSectionHeaderHeight(m_style);
  }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherResultRow>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* row = static_cast<LauncherResultRow*>(&tile);
    row->setListStyle(m_style);
    row->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    row->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets>
  itemTooltipAnchorInsets(std::size_t index, float cellWidth, float cellHeight) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float gap = (m_style.compact ? Style::spaceSm : Style::spaceMd) * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float pinnedWidth = (*m_results)[index].pinned ? glyphSize + gap : 0.0F;
    const float left = std::max(padding, cellWidth - padding - pinnedWidth - glyphSize);
    const float top = std::max(0.0F, (cellHeight - glyphSize) * 0.5F);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;

    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  bool onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value() && m_onActivate;
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    } else if (activated) {
      m_onActivate(*sourceIndex);
    }
    return sourceIndex.has_value() || targetIndex.has_value();
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

class LauncherAppGridAdapter final : public VirtualGridAdapter {
public:
  using ActivateCallback = std::function<void(std::size_t)>;
  using SecondaryActivateCallback = std::function<void(std::size_t, float, float)>;
  using ReorderCallback = std::function<void(std::size_t, std::size_t)>;

  LauncherAppGridAdapter(LauncherListStyle style, AsyncTextureCache* cache) : m_style(style), m_cache(cache) {}

  void setListStyle(LauncherListStyle style) { m_style = style; }
  void setResults(const std::vector<LauncherResult>* results) { m_results = results; }
  void setRenderer(Renderer* renderer) { m_renderer = renderer; }
  [[nodiscard]] bool setReorderEnabled(bool enabled) {
    if (m_reorderEnabled == enabled) {
      return false;
    }
    m_reorderEnabled = enabled;
    return true;
  }
  void setOnActivate(ActivateCallback callback) { m_onActivate = std::move(callback); }
  void setOnSecondaryActivate(SecondaryActivateCallback callback) { m_onSecondaryActivate = std::move(callback); }
  void setOnReorder(ReorderCallback callback) { m_onReorder = std::move(callback); }

  [[nodiscard]] std::size_t itemCount() const override { return m_results == nullptr ? 0U : m_results->size(); }

  [[nodiscard]] std::unique_ptr<Node> createTile() override {
    return std::make_unique<LauncherAppGridTile>(m_style, m_cache);
  }

  void bindTile(Node& tile, std::size_t index, bool selected, bool hovered) override {
    if (m_renderer == nullptr || m_results == nullptr || index >= m_results->size()) {
      return;
    }
    auto* gridTile = static_cast<LauncherAppGridTile*>(&tile);
    gridTile->setListStyle(m_style);
    gridTile->setReorderTarget(m_dropIndex.has_value() && *m_dropIndex == index);
    gridTile->bind(*m_renderer, (*m_results)[index], tile.width(), tile.height(), selected, hovered);
  }

  [[nodiscard]] std::string itemTooltip(std::size_t index) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return {};
    }
    return (*m_results)[index].origin;
  }

  [[nodiscard]] std::optional<TooltipAnchorInsets>
  itemTooltipAnchorInsets(std::size_t index, float cellWidth, float cellHeight) const override {
    if (m_results == nullptr || index >= m_results->size() || (*m_results)[index].originGlyph.empty()) {
      return std::nullopt;
    }
    const float glyphSize = Style::fontSizeBody * m_style.scale;
    const float padding = Style::spaceSm * m_style.scale;
    const float left = std::max(0.0F, cellWidth - padding - glyphSize);
    const float top = std::max(0.0F, cellHeight - padding - glyphSize);
    return TooltipAnchorInsets{
        .top = top,
        .right = std::max(0.0F, cellWidth - left - glyphSize),
        .bottom = std::max(0.0F, cellHeight - top - glyphSize),
        .left = left,
    };
  }

  [[nodiscard]] bool overlayHitTest(
      std::size_t index, float /*cellLocalX*/, float /*cellLocalY*/, float /*cellWidth*/, float /*cellHeight*/
  ) const override {
    return isReorderable(index);
  }

  bool
  onPointerPress(std::size_t index, float cellLocalX, float cellLocalY, float cellWidth, float cellHeight) override {
    if (!overlayHitTest(index, cellLocalX, cellLocalY, cellWidth, cellHeight)) {
      return false;
    }
    m_dragSourceIndex = index;
    m_dropIndex.reset();
    m_dragging = false;
    return true;
  }

  bool onPointerDrag(
      std::optional<std::size_t> index, float /*localX*/, float /*localY*/, float /*cellWidth*/, float /*cellHeight*/
  ) override {
    if (!m_dragSourceIndex.has_value()) {
      return false;
    }
    m_dragging = true;
    const std::optional<std::size_t> nextTarget =
        index.has_value() && *index != *m_dragSourceIndex && isReorderable(*index) ? index : std::nullopt;
    if (nextTarget == m_dropIndex) {
      return false;
    }
    m_dropIndex = nextTarget;
    return true;
  }

  bool onPointerRelease(std::optional<std::size_t> /*index*/) override {
    const auto sourceIndex = m_dragSourceIndex;
    const auto targetIndex = m_dropIndex;
    const bool reordered = m_dragging && sourceIndex.has_value() && targetIndex.has_value() && m_onReorder;
    const bool activated = !m_dragging && sourceIndex.has_value() && m_onActivate;
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
    if (reordered) {
      m_onReorder(*sourceIndex, *targetIndex);
    } else if (activated) {
      m_onActivate(*sourceIndex);
    }
    return sourceIndex.has_value() || targetIndex.has_value();
  }

  void onPointerCancel() override {
    m_dragSourceIndex.reset();
    m_dropIndex.reset();
    m_dragging = false;
  }

  void onActivate(std::size_t index) override {
    if (m_onActivate) {
      m_onActivate(index);
    }
  }

  void onSecondaryActivate(std::size_t index, float anchorX, float anchorY) override {
    if (m_onSecondaryActivate) {
      m_onSecondaryActivate(index, anchorX, anchorY);
    }
  }

private:
  [[nodiscard]] bool isReorderable(std::size_t index) const {
    return m_reorderEnabled && m_results != nullptr && index < m_results->size() && (*m_results)[index].pinned;
  }

  LauncherListStyle m_style{};
  AsyncTextureCache* m_cache = nullptr;
  Renderer* m_renderer = nullptr;
  const std::vector<LauncherResult>* m_results = nullptr;
  ActivateCallback m_onActivate;
  SecondaryActivateCallback m_onSecondaryActivate;
  ReorderCallback m_onReorder;
  bool m_reorderEnabled = false;
  std::optional<std::size_t> m_dragSourceIndex;
  std::optional<std::size_t> m_dropIndex;
  bool m_dragging = false;
};

LauncherPanel::LauncherPanel(ConfigService* config, AsyncTextureCache* asyncTextures)
    : m_iconResolver(true), m_config(config), m_asyncTextures(asyncTextures) {}

LauncherPanel::~LauncherPanel() = default;

PanelPlacement LauncherPanel::panelPlacement() const noexcept {
  return m_config != nullptr ? m_config->config().shell.panel.launcherPlacement : PanelPlacement::Floating;
}

void LauncherPanel::applyProviderConfig(LauncherProvider& provider) const {
  std::string triggerWord = std::string(provider.defaultPrefix());
  std::string prefix = "/";
  std::optional<bool> global;
  if (m_config != nullptr) {
    const auto& launcherCfg = m_config->config().shell.launcher;
    prefix = launcherCfg.providerPrefix;
    if (provider.allowCustomPrefix()) {
      const std::string key = StringUtils::toLower(std::string(provider.id()));
      auto it = std::ranges::find(launcherCfg.providers, key, &LauncherProviderConfig::name);
      if (it != launcherCfg.providers.end()) {
        if (!it->prefix.empty()) {
          triggerWord = it->prefix;
        }
        global = it->global;
      }
    }
  }

  if (provider.allowCustomPrefix()) {
    provider.setCustomPrefix(triggerWord.empty() ? std::string() : prefix + triggerWord);
    provider.setCustomIncludeInGlobalSearch(global);
  }
}

void LauncherPanel::finishActivation(LauncherProvider& provider, const std::string& resultId, bool copied) {
  if (shouldTrackUsage() && provider.trackUsage()) {
    m_usageTracker.record(provider.id(), resultId);
  }
  PanelManager::instance().closePanel(false);
  if (copied && provider.supportsAutoPaste() && m_onCopiedActivation) {
    m_onCopiedActivation(provider);
  }
}

void LauncherPanel::addProvider(std::unique_ptr<LauncherProvider> provider) {
  applyProviderConfig(*provider);
  provider->initialize();
  provider->setResultsChangedCallback([this]() { onProviderResultsChanged(); });
  provider->setQueryRequestedCallback([this](std::string query) { setQuery(std::move(query)); });
  LauncherProvider* providerPtr = provider.get();
  provider->setFormRequestedCallback([this, providerPtr](LauncherForm form) {
    beginForm(std::move(form), providerPtr);
  });
  provider->setActivationDoneCallback([this, providerPtr](const std::string& resultId, bool copied) {
    finishActivation(*providerPtr, resultId, copied);
  });
  m_providers.push_back(std::move(provider));
}

void LauncherPanel::clearDynamicProviders() {
  std::erase_if(m_providers, [](const std::unique_ptr<LauncherProvider>& provider) { return provider->isDynamic(); });
}

void LauncherPanel::clearProvidersWithIdPrefix(std::string_view prefix) {
  std::erase_if(m_providers, [&](const std::unique_ptr<LauncherProvider>& provider) {
    return provider->id().starts_with(prefix);
  });
}

void LauncherPanel::setScopedProvider(std::string_view providerId, std::string_view placeholder) {
  m_scopedProviderId = providerId;
  m_scopedPlaceholder = placeholder;
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
}

void LauncherPanel::create() {
  m_launcherRowHeight = 0.0F;
  const float scale = contentScale();
  auto container = ui::column({
      .out = &m_container,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
  });

  // Raycast search bar: a large frameless field over a hairline.
  auto searchBar = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .paddingH = Style::spaceXs * scale,
  });
  searchBar->addChild(
      ui::input({
          .out = &m_input,
          .placeholder = m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder,
          .fontSize = Style::fontSizeHeader * scale,
          .controlHeight = (Style::controlHeightLg + Style::spaceSm) * scale,
          .horizontalPadding = Style::spaceXs * scale,
          .clearButtonEnabled = true,
          .lineEditing = true,
          .frameVisible = false,
          .surfaceOpacity = panelCardOpacity(),
          .flexGrow = 1.0F,
          .onChange =
              [this](const std::string& text) {
                onInputChanged(text);
                if (m_input == nullptr) {
                  return;
                }
                const std::string preview = singleLinePreview(text);
                if (preview != text) {
                  m_input->setValue(preview);
                }
              },
          .onSubmit = [this](const std::string& /*text*/) { activateSelected(); },
          .onKeyEvent = [this](std::uint32_t sym, std::uint32_t modifiers) { return handleKeyEvent(sym, modifiers); },
      })
  );
  container->addChild(std::move(searchBar));
  container->addChild(
      ui::separator({
          .color = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .thickness = 1.0F,
          .spacing = 0.0F,
      })
  );

  container->addChild(
      ui::segmented({
          .out = &m_categoryFilter,
          .scale = scale,
          .compact = true,
          .surfaceOpacity = panelCardOpacity(),
          .equalSegmentWidths = true,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Segmented& segmented) { segmented.setAlign(FlexAlign::Center); },
      })
  );

  // The results list, with the preview pane beside it for providers that ask for one.
  auto body = ui::row({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
      .flexGrow = 1.0F,
  });
  auto listColumn = ui::column({
      .out = &m_listColumn,
      .align = FlexAlign::Stretch,
      .fillHeight = true,
      .flexGrow = 1.0F,
  });

  const LauncherListStyle initialStyle = launcherListStyleFrom(m_config, scale, panelCardOpacity());
  m_listAdapter = std::make_unique<LauncherResultAdapter>(initialStyle, m_asyncTextures);
  m_gridAdapter = std::make_unique<LauncherAppGridAdapter>(initialStyle, m_asyncTextures);
  m_listAdapter->setResults(&m_results);
  m_gridAdapter->setResults(&m_results);
  const auto onActivate = [this](std::size_t index) { activateAt(index); };
  const auto onSecondaryActivate = [this](std::size_t index, float ax, float ay) {
    (void)openActionsMenu(index, ax, ay);
  };
  m_listAdapter->setOnActivate(onActivate);
  m_listAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_listAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });
  m_gridAdapter->setOnActivate(onActivate);
  m_gridAdapter->setOnSecondaryActivate(onSecondaryActivate);
  m_gridAdapter->setOnReorder([this](std::size_t sourceIndex, std::size_t targetIndex) {
    if (sourceIndex >= m_results.size() || targetIndex >= m_results.size()) {
      return;
    }
    reorderPinnedApplication(m_results[sourceIndex].desktopEntryPath, m_results[targetIndex].desktopEntryPath);
  });

  listColumn->addChild(
      ui::virtualGridView({
          .out = &m_grid,
          .columns = 1,
          .cellHeight = launcherRowHeightEstimate(initialStyle),
          .squareCells = false,
          .columnGap = 0.0F,
          .rowGap = Style::spaceXs * scale,
          .overscanRows = kRowOverscan,
          .itemCursorShape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
          .adapter = m_listAdapter.get(),
          .flexGrow = 1.0F,
          .onSelectionChanged =
              [this](std::optional<std::size_t> idx) {
                if (idx.has_value() && *idx < m_results.size()) {
                  m_selectedIndex = *idx;
                }
                syncFooter();
                syncPreview();
              },
          .configure = [](VirtualGridView& grid) { grid.setFillWidth(true); },
      })
  );

  auto detailScroll = ui::scrollView({
      .out = &m_detailScroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = Style::spaceSm * scale,
      .viewportPaddingV = Style::spaceSm * scale,
      .flexGrow = 1.0F,
      .visible = false,
      .participatesInLayout = false,
      .configure = [scale, opacity = panelCardOpacity()](ScrollView& scrollView) {
        scrollView.setCardStyle(scale, opacity);
      },
  });
  auto* detailContent = detailScroll->content();
  detailContent->setDirection(FlexDirection::Vertical);
  detailContent->setAlign(FlexAlign::Stretch);
  detailContent->setGap(Style::spaceSm * scale);
  detailContent->addChild(
      ui::label({
          .out = &m_detailSubtitle,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  detailContent->addChild(
      ui::label({
          .out = &m_detailBody,
          .fontSize = Style::fontSizeBody * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 0,
          .flexGrow = 1.0F,
      })
  );
  listColumn->addChild(std::move(detailScroll));

  listColumn->addChild(
      ui::label({
          .out = &m_emptyLabel,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
          .participatesInLayout = false,
      })
  );

  body->addChild(std::move(listColumn));
  body->addChild(
      ui::separator({
          .out = &m_previewDivider,
          .color = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .thickness = 1.0F,
          .spacing = 0.0F,
          .orientation = SeparatorOrientation::VerticalRule,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  body->addChild(buildPreviewPane(scale));

  container->addChild(std::move(body));
  container->addChild(buildFooter(scale));

  setRoot(std::move(container));

  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }

  m_appIconColorizeConn = shellAppIconColorizationChanged().connect([this]() { refreshLauncherAppIconColorization(); });

  syncLauncherListStyle();
}

void LauncherPanel::refreshLauncherAppIconColorization() {
  if (m_listAdapter == nullptr || m_gridAdapter == nullptr || m_grid == nullptr) {
    return;
  }
  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale(), panelCardOpacity());
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  m_grid->notifyDataChanged();
}

bool LauncherPanel::shouldUseAppGrid() const {
  if (m_config == nullptr || !m_config->config().shell.launcher.appGrid || !m_launcherShowIcons) {
    return false;
  }
  if (m_results.empty()) {
    return false;
  }
  return std::ranges::all_of(m_results, [](const LauncherResult& result) {
    return result.providerId == kApplicationsProviderId;
  });
}

void LauncherPanel::syncLauncherViewLayout(Renderer* renderer) {
  if (m_grid == nullptr || m_listAdapter == nullptr || m_gridAdapter == nullptr) {
    return;
  }

  const bool useGrid = shouldUseAppGrid();
  const float scale = contentScale();
  const LauncherListStyle style = launcherListStyleFrom(m_config, scale, panelCardOpacity());
  m_grid->setScale(scale);
  m_listAdapter->setListStyle(style);
  m_gridAdapter->setListStyle(style);
  const bool reorderEnabled = m_query.empty() && m_scopedProviderId.empty() && m_activeCategoryType == All;
  const bool listReorderEnabledChanged = m_listAdapter->setReorderEnabled(reorderEnabled);
  const bool gridReorderEnabledChanged = m_gridAdapter->setReorderEnabled(reorderEnabled);
  const bool reorderEnabledChanged = listReorderEnabledChanged || gridReorderEnabledChanged;
  if (renderer != nullptr) {
    m_listAdapter->setRenderer(renderer);
    m_gridAdapter->setRenderer(renderer);
  }

  const bool modeChanged = useGrid != m_usingAppGrid;
  m_usingAppGrid = useGrid;
  if (modeChanged || reorderEnabledChanged) {
    m_launcherRowHeight = 0.0F;
  }

  if (useGrid) {
    m_grid->setColumns(kAppGridColumns);
    m_grid->setSquareCells(false);
    m_grid->setColumnGap(Style::spaceSm * scale);
    m_grid->setRowGap(Style::spaceSm * scale);
    m_grid->setCellHeight(launcherAppGridCellHeightEstimate(style));
    if (modeChanged) {
      m_grid->setAdapter(m_gridAdapter.get());
    }
  } else {
    m_grid->setColumns(1);
    m_grid->setColumnGap(0.0F);
    m_grid->setRowGap(Style::spaceXs * scale);
    const float listCellHeight =
        renderer != nullptr ? launcherRowHeight(*renderer, style) : launcherRowHeightEstimate(style);
    m_grid->setCellHeight(listCellHeight);
    if (renderer != nullptr) {
      m_launcherRowHeight = listCellHeight;
    }
    if (modeChanged) {
      m_grid->setAdapter(m_listAdapter.get());
    }
  }

  if (modeChanged || reorderEnabledChanged) {
    if (renderer != nullptr) {
      updateLauncherGridMetrics(*renderer);
    }
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::syncLauncherListStyle() {
  const bool showIcons = m_config == nullptr || m_config->config().shell.launcher.showIcons;
  const bool showAppOriginIndicator = m_config == nullptr || m_config->config().shell.launcher.showAppOriginIndicator;
  const bool compact = m_config != nullptr && m_config->config().shell.launcher.compact;
  const bool appGrid = m_config != nullptr && m_config->config().shell.launcher.appGrid;
  if (showIcons == m_launcherShowIcons
      && showAppOriginIndicator == m_launcherShowAppOriginIndicator
      && compact == m_launcherCompact
      && appGrid == m_launcherAppGrid
      && m_listAdapter != nullptr) {
    return;
  }
  m_launcherShowIcons = showIcons;
  m_launcherShowAppOriginIndicator = showAppOriginIndicator;
  m_launcherCompact = compact;
  m_launcherAppGrid = appGrid;
  m_launcherRowHeight = 0.0F;
  syncLauncherViewLayout(nullptr);
  if (m_grid != nullptr) {
    m_grid->notifyDataChanged();
  }
}

void LauncherPanel::updateLauncherGridMetrics(Renderer& renderer) {
  if (m_grid == nullptr) {
    return;
  }

  const LauncherListStyle style = launcherListStyleFrom(m_config, contentScale(), panelCardOpacity());
  float cellHeight = launcherRowHeight(renderer, style);
  if (m_usingAppGrid) {
    float wrapWidth = 0.0F;
    const std::size_t columns = std::max<std::size_t>(1, m_grid->layoutColumnCount());
    const float viewportW = m_grid->scrollView().contentViewportWidth();
    const float gap = Style::spaceSm * contentScale();
    const float cellW =
        columns > 0 ? (viewportW - static_cast<float>(columns - 1) * gap) / static_cast<float>(columns) : viewportW;
    const float paddingH = Style::spaceSm * contentScale() * 2.0F;
    wrapWidth = std::max(0.0F, cellW - paddingH);
    cellHeight = launcherAppGridCellHeight(renderer, style, wrapWidth);
  }
  if (std::abs(cellHeight - m_launcherRowHeight) < 0.5F) {
    return;
  }

  m_launcherRowHeight = cellHeight;
  m_grid->setCellHeight(cellHeight);
}

void LauncherPanel::onPanelCardOpacityChanged(float opacity) {
  if (m_input != nullptr) {
    m_input->setSurfaceOpacity(opacity);
  }
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->setSurfaceOpacity(opacity);
  }
  if (m_detailScroll != nullptr) {
    m_detailScroll->setCardStyle(contentScale(), opacity);
  }
}

float LauncherPanel::fittedHeight() const {
  return m_container
      ? std::max(scaled(150), panel_content::height(m_container) + 2 * Style::panelPadding * contentScale())
      : preferredHeight();
}

// Spotlight-like proportions in the Island: a share of the screen, never narrower than the
// standalone launcher, and a height that follows the results up to a cap instead of
// stretching a long list down the whole output.
float LauncherPanel::islandWidth(float availableWidth) const {
  return std::clamp(availableWidth * 0.42F, preferredWidth(), std::max(preferredWidth(), scaled(880.0F)));
}

float LauncherPanel::islandHeight(float availableHeight) const {
  const float cap = std::clamp(availableHeight * 0.55F, preferredHeight(), std::max(preferredHeight(), scaled(760.0F)));
  return std::min(fittedHeight(), cap);
}

void LauncherPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_container == nullptr || m_input == nullptr) {
    return;
  }

  syncLauncherListStyle();
  syncLauncherViewLayout(&renderer);
  updateLauncherGridMetrics(renderer);

  if (m_previewPane != nullptr && m_previewPane->visible()) {
    // Raycast gives the preview a little over half the window.
    const float paneWidth = std::round(width * 0.52F);
    m_previewPane->setMinWidth(paneWidth);
    m_previewPane->setMaxWidth(paneWidth);
    for (Label* value : m_previewMetaValues) {
      value->setMaxWidth(paneWidth * 0.62F);
    }
    if (m_previewImageDirty && m_previewImage != nullptr) {
      m_previewImageDirty = false;
      const int target = static_cast<int>(std::round(paneWidth));
      bool loaded = false;
      if (!m_pendingPreviewImagePath.empty()) {
        loaded = m_previewImage->setSourceFile(renderer, m_pendingPreviewImagePath, target, true);
      } else if (!m_pendingPreviewImageBytes.empty()) {
        loaded = m_previewImage->setSourceBytes(
            renderer, m_pendingPreviewImageBytes.data(), m_pendingPreviewImageBytes.size(), true
        );
      }
      m_previewImage->setVisible(loaded);
      m_previewImage->setParticipatesInLayout(loaded);
    }
  }

  m_container->setSize(width, height);
  m_container->layout(renderer);
}

void LauncherPanel::onOpen(std::string_view context) {
  for (auto& provider : m_providers) {
    applyProviderConfig(*provider);
  }
  if (m_config != nullptr) {
    m_aliases.setConfigAliases(m_config->config().shell.launcher.aliases);
  }
  m_aliasTarget.reset();
  m_form.reset();
  m_formProvider = nullptr;

  // Pick up apps installed since the last scan (notably Nix profile swaps that
  // inotify cannot observe). Cheap stat-only check; only rescans on real change.
  refreshDesktopEntriesIfSourcesChanged();

  m_categoryFilterVisible = m_config != nullptr && m_config->config().shell.launcher.categories;
  m_activeCategoryType = All;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  if (m_categoryFilter != nullptr) {
    m_categoryFilter->clearOptions();
    m_categoryFilter->setVisible(false);
    m_categoryFilter->setParticipatesInLayout(false);
  }

  const std::string initialValue(context);
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
    m_input->setValue(singleLinePreview(initialValue));
  }
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(initialValue);
}

bool LauncherPanel::isContextActive(std::string_view context) const {
  return !m_form.has_value()
      && !StringUtils::isBlank(context)
      && StringUtils::trim(m_query) == StringUtils::trim(context);
}

void LauncherPanel::onClose() {
  if (m_actionsMenu != nullptr && m_actionsMenu->isOpen()) {
    m_actionsMenu->close();
  }

  if (m_asyncTextures != nullptr) {
    DeferredCall::callLater([asyncTextures = m_asyncTextures]() { asyncTextures->trimUnused(0); });
  }

  for (auto& provider : m_providers) {
    provider->reset();
  }

  m_query.clear();
  m_results.clear();
  m_allResults.clear();
  m_aliasTarget.reset();
  m_form.reset();
  m_formProvider = nullptr;
  m_previewProvider = nullptr;
  m_previewKey.clear();
  m_pendingPreviewImagePath.clear();
  m_pendingPreviewImageBytes.clear();
  m_previewImageDirty = false;
  m_scopedProviderId.clear();
  m_scopedPlaceholder.clear();
  m_activeCategoryType = All;
  m_activeCategory.clear();
  m_currentCategories.clear();
  m_categoryFilterSlots.clear();
  m_hasRecentlyUsed = false;
  m_selectedIndex = 0;
  m_usingAppGrid = false;
  m_launcherRowHeight = 0.0F;

  if (m_grid != nullptr) {
    m_grid->setAdapter(nullptr);
  }
  m_listAdapter.reset();
  m_gridAdapter.reset();

  // The scene tree (and all nodes) is destroyed by PanelManager after onClose().
  m_container = nullptr;
  m_input = nullptr;
  m_categoryFilter = nullptr;
  m_body = nullptr;
  m_listColumn = nullptr;
  m_previewPane = nullptr;
  m_previewDivider = nullptr;
  m_previewImage = nullptr;
  m_previewBadge = nullptr;
  m_previewTitle = nullptr;
  m_previewBody = nullptr;
  m_previewMetaDivider = nullptr;
  m_previewMetaRows.clear();
  m_previewMetaLabels.clear();
  m_previewMetaValues.clear();
  m_grid = nullptr;
  m_detailScroll = nullptr;
  m_detailSubtitle = nullptr;
  m_detailBody = nullptr;
  m_emptyLabel = nullptr;
  clearReleasedRoot();
}

void LauncherPanel::onIconThemeChanged() { reapplyCurrentQuery(); }

void LauncherPanel::clearUsage() {
  m_usageTracker.clear();
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

bool LauncherPanel::shouldTrackUsage() const {
  return m_config != nullptr && m_config->config().shell.launcher.sortByUsage;
}

void LauncherPanel::syncUsageTrackingState() {
  if (m_input != nullptr) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::reapplyCurrentQuery() {
  std::string selectedProvider;
  std::string selectedId;
  if (m_selectedIndex < m_results.size()) {
    selectedProvider = m_results[m_selectedIndex].providerId;
    selectedId = m_results[m_selectedIndex].id;
  }

  onInputChanged(m_query);

  if (!selectedId.empty()) {
    for (std::size_t i = 0; i < m_results.size(); ++i) {
      if (m_results[i].providerId == selectedProvider && m_results[i].id == selectedId) {
        m_selectedIndex = i;
        break;
      }
    }
  }
  refreshResults();
}

void LauncherPanel::setQuery(std::string query) {
  if (m_input == nullptr) {
    return;
  }
  m_input->setValue(singleLinePreview(query));
  if (m_grid != nullptr) {
    m_grid->scrollView().setScrollOffset(0.0F);
  }
  onInputChanged(query);
}

void LauncherPanel::onProviderResultsChanged() {
  // Only re-gather while the panel is open and built; after onClose the scene
  // nodes are gone and a refresh would touch null grid/label pointers.
  if (m_input == nullptr) {
    return;
  }
  reapplyCurrentQuery();
}

InputArea* LauncherPanel::initialFocusArea() const { return m_input != nullptr ? m_input->inputArea() : nullptr; }

bool LauncherPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!pressed || preedit) {
    return false;
  }

  auto& dispatcher = PanelManager::instance().inputDispatcher();
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr) {
    const bool onInput = (m_input != nullptr && focused == m_input->inputArea());
    const bool inResults = (m_grid != nullptr && isDescendantOf(focused, m_grid));
    if (!onInput && !inResults) {
      return false;
    }
  }

  if (m_categoryFilter != nullptr && m_categoryFilter->visible()) {
    if (focused == m_categoryFilter->focusArea()) {
      return false;
    }
  }

  return handleKeyEvent(sym, modifiers);
}

void LauncherPanel::onInputChanged(const std::string& text) {
  const auto desktopVersion = desktopEntriesVersion();
  if (desktopVersion != m_desktopEntriesVersion) {
    m_iconResolver.invalidateMissingCache();
    m_desktopEntriesVersion = desktopVersion;
  }

  m_query = text;
  m_allResults.clear();
  m_mixedResults = false;
  m_previewProvider = nullptr;

  if (m_form.has_value()) {
    buildFormRows(text);
    return;
  }

  if (m_aliasTarget.has_value()) {
    // "Set Alias…": the field takes the alias; one row says what Return will do.
    const std::string alias = AliasStore::normalize(text);
    LauncherResult editor;
    editor.id = "alias";
    editor.providerId = std::string(kAliasEditorProviderId);
    editor.title = alias.empty() ? i18n::tr("launcher.aliases.type", "name", m_aliasTarget->title)
                                 : i18n::tr("launcher.aliases.set", "alias", alias, "name", m_aliasTarget->title);
    editor.subtitle = alias.contains(' ') ? i18n::tr("launcher.aliases.no-spaces") : std::string();
    editor.glyphName = "keyboard";
    m_allResults.push_back(std::move(editor));
    if (m_categoryFilter != nullptr) {
      setCategoryFilterVisible(false);
    }
    m_activeCategoryType = All;
    m_anyProviderLoading = false;
    applyActiveCategory();
    return;
  }

  std::vector<LauncherCategory> newCategories;
  bool hasRecentlyUsed = false;
  bool anyProviderLoading = false;

  if (!m_scopedProviderId.empty()) {
    for (auto& provider : m_providers) {
      if (provider->id() != m_scopedProviderId) {
        continue;
      }
      m_allResults = provider->query(text);
      anyProviderLoading = provider->isLoading();
      for (auto& result : m_allResults) {
        result.providerId = provider->id();
      }
      sortResultsByScore(m_allResults);
      if (provider->showsPreview()) {
        m_previewProvider = provider.get();
      }
      break;
    }
  } else {
    // Route query to providers
    LauncherProvider* activeProvider = nullptr;
    std::string_view queryText = text;

    // Check for prefix match (longest first)
    for (auto& provider : m_providers) {
      auto prefix = provider->prefix();
      if (prefix.empty()) {
        continue;
      }
      if (text.size() >= prefix.size()
          && std::string_view(text).starts_with(prefix)
          && (activeProvider == nullptr || prefix.size() > activeProvider->prefix().size())) {
        activeProvider = provider.get();
        queryText = std::string_view(text).substr(prefix.size());
      }
    }
    // Trim leading space after prefix
    if (activeProvider != nullptr && !queryText.empty() && queryText.front() == ' ') {
      queryText = queryText.substr(1);
    }

    const bool typedQuery = !queryText.empty();
    const bool sortByUsage = m_config != nullptr && m_config->config().shell.launcher.sortByUsage;

    auto applyUsageBoost = [&](std::vector<LauncherResult>& results, const LauncherProvider& provider) {
      if (!sortByUsage) {
        return;
      }
      for (auto& result : results) {
        const int usageCount = m_usageTracker.getCount(provider.id(), result.id);
        result.score += usageBoostForScore(result.score, usageCount, typedQuery);
        result.recentlyUsedIndex = m_usageTracker.getRecentlyUsedIndex(provider.id(), result.id);
      }
    };

    if (activeProvider != nullptr) {
      m_allResults = activeProvider->queryPrefixed(queryText);
      anyProviderLoading = activeProvider->isLoading();
      if (activeProvider->trackUsage()) {
        applyUsageBoost(m_allResults, *activeProvider);
        if (sortByUsage && m_usageTracker.getRecentlyUsedCount(activeProvider->id()) > 0) {
          hasRecentlyUsed = true;
        }
      }
      for (auto& result : m_allResults) {
        result.providerId = activeProvider->id();
      }
      sortResultsByScore(m_allResults);
      newCategories = activeProvider->categories();
      if (activeProvider->showsPreview()) {
        m_previewProvider = activeProvider;
      }
    } else if (startsWithLauncherPrefix(text)) {
      m_allResults = providerOverviewResults(text);
    } else {
      m_mixedResults = true;
      // Query default providers (empty prefix), plus prefixed providers that opt into global search.
      // Prefixed opt-in providers (e.g. Session) only contribute once the query is long enough,
      // so opening the launcher with no/short input does not flood it with their entries.
      const bool allowGlobalOptIn =
          StringUtils::trimRightView(StringUtils::trimLeftView(queryText)).size() >= kGlobalOptInMinChars;
      for (auto& provider : m_providers) {
        const bool isDefault = provider->prefix().empty();
        if (!isDefault && (!provider->includeInGlobalSearch() || !allowGlobalOptIn)) {
          continue;
        }
        auto results = provider->query(queryText);
        if (provider->isLoading()) {
          anyProviderLoading = true;
        }
        if (provider->trackUsage()) {
          applyUsageBoost(results, *provider);
          if (sortByUsage && m_usageTracker.getRecentlyUsedCount(provider->id()) > 0) {
            hasRecentlyUsed = true;
          }
        }
        for (auto& result : results) {
          result.providerId = provider->id();
        }
        m_allResults.insert(
            m_allResults.end(), std::make_move_iterator(results.begin()), std::make_move_iterator(results.end())
        );
        auto providerCats = provider->categories();
        for (auto& cat : providerCats) {
          newCategories.push_back(std::move(cat));
        }
      }
      sortResultsByScore(m_allResults);
      applyAliases(queryText);
    }
  }

  const int iconTargetSize = static_cast<int>(
      std::round(launcherIconSize(launcherListStyleFrom(m_config, contentScale(), panelCardOpacity())))
  );
  for (auto& result : m_allResults) {
    if (result.iconPath.empty() && !result.iconName.empty()) {
      const std::string& resolved = m_iconResolver.resolve(result.iconName, iconTargetSize);
      if (!resolved.empty()) {
        result.iconPath = resolved;
      } else if (result.iconName != "application-x-executable") {
        const std::string& fallback = m_iconResolver.resolve("application-x-executable", iconTargetSize);
        if (!fallback.empty()) {
          result.iconPath = fallback;
        }
      }
      result.iconName.clear();
    }
  }

  updatePinnedApplicationState();

  for (auto& result : m_allResults) {
    const LauncherProvider* provider = providerFor(result.providerId);
    if (provider != nullptr && provider->supportsAliases()) {
      result.alias = aliasForResult(result);
    }
  }

  bool categoriesChanged = newCategories.size() != m_currentCategories.size();
  if (!categoriesChanged) {
    for (std::size_t i = 0; i < newCategories.size(); ++i) {
      if (newCategories[i].label != m_currentCategories[i].label) {
        categoriesChanged = true;
        break;
      }
    }
  }

  if (hasRecentlyUsed != m_hasRecentlyUsed) {
    m_hasRecentlyUsed = hasRecentlyUsed;
    categoriesChanged = true;
  }

  if (categoriesChanged) {
    m_activeCategoryType = All;
    m_activeCategory.clear();
    rebuildCategoryFilter(newCategories);
  }

  if (text.empty() && m_scopedProviderId.empty()) {
    applyPinnedApplicationOrder();
  }

  m_anyProviderLoading = anyProviderLoading;

  applyActiveCategory();
}

void LauncherPanel::applyPinnedApplicationOrder() {
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  if (pinnedEntries.empty()) {
    return;
  }

  std::vector<std::string> pinnedPaths;
  pinnedPaths.reserve(pinnedEntries.size());
  for (const DesktopEntry& entry : pinnedEntries) {
    if (!entry.path.empty()) {
      pinnedPaths.push_back(entry.path);
    }
  }

  std::ranges::stable_sort(m_allResults, [&pinnedPaths](const LauncherResult& a, const LauncherResult& b) {
    const auto rank = [&pinnedPaths](const LauncherResult& result) {
      if (result.providerId != kApplicationsProviderId) {
        return pinnedPaths.size();
      }
      const auto it = std::ranges::find(pinnedPaths, result.desktopEntryPath);
      return it == pinnedPaths.end() ? pinnedPaths.size() : static_cast<std::size_t>(it - pinnedPaths.begin());
    };
    return rank(a) < rank(b);
  });
}

void LauncherPanel::updatePinnedApplicationState() {
  for (LauncherResult& result : m_allResults) {
    result.pinned = false;
  }
  if (m_config == nullptr || m_config->config().shell.launcher.pinned.empty()) {
    return;
  }

  const auto pinnedEntries = shell::dock::pinned_apps::resolveEntries(m_config->config().shell.launcher.pinned);
  for (LauncherResult& result : m_allResults) {
    if (result.providerId != kApplicationsProviderId) {
      continue;
    }
    result.pinned = std::ranges::any_of(pinnedEntries, [&result](const DesktopEntry& entry) {
      return entry.path == result.desktopEntryPath;
    });
  }
}

void LauncherPanel::reorderPinnedApplication(std::string_view sourcePath, std::string_view targetPath) {
  if (m_config == nullptr || sourcePath.empty() || targetPath.empty() || sourcePath == targetPath) {
    return;
  }

  std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
  const auto entries = shell::dock::pinned_apps::resolveEntries(pinned);
  const auto indexForPath = [&entries](std::string_view path) -> std::optional<std::size_t> {
    const auto it = std::ranges::find(entries, path, &DesktopEntry::path);
    if (it == entries.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(it - entries.begin());
  };

  const auto sourceIndex = indexForPath(sourcePath);
  const auto targetIndex = indexForPath(targetPath);
  if (!sourceIndex.has_value() || !targetIndex.has_value() || *sourceIndex == *targetIndex) {
    return;
  }

  std::string moved = std::move(pinned[*sourceIndex]);
  pinned.erase(pinned.begin() + static_cast<std::ptrdiff_t>(*sourceIndex));
  pinned.insert(pinned.begin() + static_cast<std::ptrdiff_t>(*targetIndex), std::move(moved));

  if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
    reapplyCurrentQuery();
  }
}

void LauncherPanel::rebuildCategoryFilter(const std::vector<LauncherCategory>& categories) {
  m_currentCategories = categories;
  m_categoryFilterSlots.clear();
  if (categories.empty() && !m_hasRecentlyUsed) {
    if (m_categoryFilter != nullptr) {
      m_categoryFilter->clearOptions();
      setCategoryFilterVisible(false);
    }
    return;
  }

  m_categoryFilterSlots.push_back({All, 0});
  if (m_hasRecentlyUsed) {
    m_categoryFilterSlots.push_back({RecentlyUsed, 0});
  }
  for (std::size_t i = 0; i < categories.size(); ++i) {
    m_categoryFilterSlots.push_back({Category, i});
  }

  if (m_categoryFilter == nullptr) {
    return;
  }

  m_categoryFilter->clearOptions();
  for (std::size_t i = 0; i < m_categoryFilterSlots.size(); ++i) {
    const auto& slot = m_categoryFilterSlots[i];
    switch (slot.type) {
    case All:
      m_categoryFilter->addOption("", "layout-grid");
      m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.all"));
      break;
    case RecentlyUsed:
      m_categoryFilter->addOption("", "history");
      m_categoryFilter->setOptionTooltip(i, i18n::tr("launcher.categories.recently-used"));
      break;
    case Category:
      m_categoryFilter->addOption("", categories[slot.categoryIndex].glyphName);
      m_categoryFilter->setOptionTooltip(i, categories[slot.categoryIndex].label);
      break;
    }
  }
  m_categoryFilter->setSelectedIndex(0);
  m_categoryFilter->setOnChange([this](std::size_t idx) { setActiveCategorySlot(idx); });
  setCategoryFilterVisible(m_categoryFilterVisible);
}

void LauncherPanel::setActiveCategorySlot(std::size_t slotIndex) {
  if (slotIndex >= m_categoryFilterSlots.size()) {
    return;
  }

  const auto& slot = m_categoryFilterSlots[slotIndex];
  m_activeCategoryType = slot.type;
  if (slot.type == Category && slot.categoryIndex < m_currentCategories.size()) {
    m_activeCategory = m_currentCategories[slot.categoryIndex].label;
  } else {
    m_activeCategory.clear();
  }
  applyActiveCategory();
}

void LauncherPanel::setCategoryFilterVisible(bool visible) {
  if (m_categoryFilter == nullptr) {
    return;
  }
  const bool show = visible && !m_categoryFilterSlots.empty();
  m_categoryFilter->setVisible(show);
  m_categoryFilter->setParticipatesInLayout(show);
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}

bool LauncherPanel::startsWithLauncherPrefix(std::string_view text) const {
  const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
  return text.starts_with(prefix);
}

std::vector<LauncherResult> LauncherPanel::providerOverviewResults(std::string_view text) const {
  std::string filter;
  if (startsWithLauncherPrefix(text)) {
    const std::string& prefix = m_config != nullptr ? m_config->config().shell.launcher.providerPrefix : "/";
    filter = StringUtils::toLower(StringUtils::trim(text.substr(prefix.size())));
  }

  std::vector<LauncherResult> results;
  results.reserve(m_providers.size());
  for (const auto& provider : m_providers) {
    const std::string_view prefix = provider->prefix();
    if (prefix.empty()) {
      continue;
    }

    const std::string title(provider->displayName());
    const std::string prefixText(prefix);
    const std::string searchable = StringUtils::toLower(title + " " + prefixText);
    const double score = filter.empty() ? 0.0 : FuzzyMatch::score(filter, searchable);
    if (!filter.empty() && !FuzzyMatch::isMatch(score)) {
      continue;
    }

    LauncherResult result;
    result.id = providerOverviewId(prefix);
    result.providerId = std::string(kProviderOverviewProviderId);
    result.title = title;
    result.subtitle = prefixText;
    result.glyphName = std::string(provider->defaultGlyphName());
    result.score = score;
    results.push_back(std::move(result));
  }

  if (!filter.empty()) {
    sortResultsByScore(results);
  }
  return results;
}

void LauncherPanel::applyActiveCategory() {
  m_results.clear();
  switch (m_activeCategoryType) {
  case All:
    m_results = m_allResults;
    break;
  case RecentlyUsed:
    std::ranges::copy_if(m_allResults, std::back_inserter(m_results), [](const LauncherResult& r) {
      return r.recentlyUsedIndex > 0;
    });
    std::ranges::sort(m_results, [](const LauncherResult& a, const LauncherResult& b) {
      return a.recentlyUsedIndex > b.recentlyUsedIndex
          || (a.recentlyUsedIndex == b.recentlyUsedIndex
              && std::tie(a.providerId, a.id) < std::tie(b.providerId, b.id));
    });
    break;
  case Category:
    for (const auto& r : m_allResults) {
      if (r.category == m_activeCategory) {
        m_results.push_back(r);
      }
    }
    break;
  }
  assignSections();
  m_selectedIndex = 0;
  refreshResults();
}

void LauncherPanel::refreshResults() {
  uiAssertNotRendering("LauncherPanel::refreshResults");
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }

  syncLauncherViewLayout(nullptr);
  m_grid->notifyDataChanged();
  if (m_results.empty()) {
    m_grid->setSelectedIndex(std::nullopt);
    m_grid->scrollView().setScrollOffset(0.0F);
  } else {
    m_grid->setSelectedIndex(m_selectedIndex);
  }
  bindDetailResult();
  applyEmptyState();
  syncFooter();
  m_previewKey.clear();
  syncPreview();
}

void LauncherPanel::applyEmptyState() {
  if (m_grid == nullptr || m_emptyLabel == nullptr) {
    return;
  }
  const bool empty = m_results.empty();
  const bool detail = !empty && shouldUseDetailPresentation();
  m_grid->setVisible(!empty && !detail);
  m_grid->setParticipatesInLayout(!empty && !detail);
  if (m_detailScroll != nullptr) {
    m_detailScroll->setVisible(detail);
    m_detailScroll->setParticipatesInLayout(detail);
  }
  m_emptyLabel->setVisible(empty);
  m_emptyLabel->setParticipatesInLayout(empty);
  if (empty) {
    if (m_anyProviderLoading && !m_query.empty()) {
      m_emptyLabel->setText(i18n::tr("launcher.empty.loading"));
    } else {
      m_emptyLabel->setText(
          m_query.empty() ? i18n::tr("launcher.empty.type-to-search") : i18n::tr("launcher.empty.no-results")
      );
    }
  }
}

bool LauncherPanel::shouldUseDetailPresentation() const {
  return m_results.size() == 1 && isDetailPresentation(m_results.front());
}

void LauncherPanel::bindDetailResult() {
  if (!shouldUseDetailPresentation()
      || m_detailScroll == nullptr
      || m_detailSubtitle == nullptr
      || m_detailBody == nullptr) {
    return;
  }

  const LauncherResult& result = m_results.front();
  const bool hasSubtitle = !result.subtitle.empty();
  m_detailSubtitle->setVisible(hasSubtitle);
  m_detailSubtitle->setParticipatesInLayout(hasSubtitle);
  m_detailSubtitle->setText(singleLinePreview(result.subtitle));
  m_detailBody->setText(result.title.empty() ? result.id : result.title);
  m_detailScroll->setScrollOffset(0.0F);
}

LauncherProvider* LauncherPanel::providerFor(std::string_view providerId) const {
  for (const auto& provider : m_providers) {
    if (provider->id() == providerId) {
      return provider.get();
    }
  }
  return nullptr;
}

std::string LauncherPanel::primaryActionLabelFor(const LauncherResult& result) const {
  if (const LauncherProvider* provider = providerFor(result.providerId)) {
    std::string label = provider->primaryActionLabel(result);
    if (!label.empty()) {
      return label;
    }
  }
  return i18n::tr(result.desktopEntryPath.empty() ? "launcher.footer.open" : "launcher.footer.open-application");
}

bool LauncherPanel::hasActions(const LauncherResult& result) const {
  if (!result.desktopEntryPath.empty()) {
    return true;
  }
  const LauncherProvider* provider = providerFor(result.providerId);
  return provider != nullptr && (provider->supportsAliases() || !provider->actions(result).empty());
}

std::string LauncherPanel::specFor(const LauncherResult& result) {
  if (!result.alias.empty()) {
    return result.alias;
  }
  return result.providerId + ":" + result.id;
}

std::optional<LauncherResult> LauncherPanel::resolveAliasTarget(const AliasStore::Target& target) const {
  LauncherProvider* provider = providerFor(target.providerId);
  if (provider == nullptr) {
    return std::nullopt;
  }
  std::string resultId = target.resultId;
  if (target.providerId == kApplicationsProviderId && !resultId.starts_with('/')) {
    // A desktop entry id from config ("firefox" or "firefox.desktop").
    const std::string withSuffix = resultId.ends_with(".desktop") ? resultId : resultId + ".desktop";
    const auto& entries = desktopEntries();
    const auto it = std::ranges::find_if(entries, [&](const DesktopEntry& entry) {
      return entry.id == resultId || entry.id == withSuffix;
    });
    if (it == entries.end()) {
      return std::nullopt;
    }
    resultId = it->path;
  }
  auto result = provider->resultForId(resultId);
  if (result.has_value()) {
    result->providerId = std::string(provider->id());
  }
  return result;
}

std::string LauncherPanel::aliasForResult(const LauncherResult& result) {
  std::string alias = m_aliases.aliasFor(result.providerId, result.id);
  if (!alias.empty() || result.providerId != kApplicationsProviderId) {
    return alias;
  }
  // Config aliases name apps by desktop id ("firefox" or "firefox.desktop") rather than by path.
  const auto& entries = desktopEntries();
  const auto it = std::ranges::find(entries, result.id, &DesktopEntry::path);
  if (it == entries.end()) {
    return {};
  }
  alias = m_aliases.aliasFor(result.providerId, it->id);
  if (alias.empty()) {
    alias = m_aliases.aliasFor(result.providerId, it->id + ".desktop");
  }
  return alias;
}

void LauncherPanel::applyAliases(std::string_view queryText) {
  const auto target = m_aliases.find(queryText);
  if (!target.has_value()) {
    return;
  }
  auto result = resolveAliasTarget(*target);
  if (!result.has_value()) {
    return;
  }
  std::erase_if(m_allResults, [&](const LauncherResult& existing) {
    return existing.providerId == result->providerId && existing.id == result->id;
  });
  result->score = kAliasScore;
  m_allResults.insert(m_allResults.begin(), std::move(*result));
}

void LauncherPanel::beginAliasEdit(const LauncherResult& result) {
  m_aliasTarget = result;
  if (m_input != nullptr) {
    m_input->setPlaceholder(i18n::tr("launcher.aliases.placeholder", "name", result.title));
  }
  setQuery(result.alias);
}

void LauncherPanel::endAliasEdit() {
  m_aliasTarget.reset();
  m_currentCategories.clear();
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
  setQuery({});
}

void LauncherPanel::beginForm(LauncherForm form, LauncherProvider* provider) {
  if (form.fields.empty()) {
    return;
  }
  m_aliasTarget.reset();
  m_formReturnQuery = m_form.has_value() ? m_formReturnQuery : m_query;
  m_form = std::move(form);
  m_formProvider = provider;
  m_formError.clear();
  m_formField = 0;
  m_currentCategories.clear();
  if (m_categoryFilter != nullptr) {
    setCategoryFilterVisible(false);
  }
  showFormField(0);
}

void LauncherPanel::endForm(bool saved) {
  if (!m_form.has_value()) {
    return;
  }
  // After a save, list the provider's items so the new one shows; a cancel goes back to the search.
  std::string query = m_formReturnQuery;
  if (saved && m_formProvider != nullptr && !m_formProvider->prefix().empty()) {
    query = std::string(m_formProvider->prefix()) + " ";
  }
  m_form.reset();
  m_formProvider = nullptr;
  m_formError.clear();
  m_formReturnQuery.clear();
  m_currentCategories.clear();
  if (m_input != nullptr) {
    m_input->setPlaceholder(
        m_scopedPlaceholder.empty() ? i18n::tr("launcher.search-placeholder") : m_scopedPlaceholder
    );
  }
  setQuery(query);
}

void LauncherPanel::showFormField(std::size_t index) {
  if (!m_form.has_value()) {
    return;
  }
  m_formField = std::min(index, m_form->fields.size());
  if (m_input != nullptr) {
    const bool onField = m_formField < m_form->fields.size();
    m_input->setPlaceholder(onField ? m_form->fields[m_formField].placeholder : std::string());
  }
  // The submit row keeps the field empty; typing there goes nowhere.
  if (m_formField < m_form->fields.size()) {
    const auto& field = m_form->fields[m_formField];
    setQuery(field.multiline ? escapeNewlines(field.value) : field.value);
  } else {
    setQuery({});
  }
}

void LauncherPanel::buildFormRows(const std::string& text) {
  if (m_formField < m_form->fields.size()) {
    auto& field = m_form->fields[m_formField];
    const std::string value = field.multiline ? unescapeNewlines(text) : text;
    if (value != field.value) {
      field.value = value;
      m_formError.clear();
    }
  }
  for (std::size_t i = 0; i < m_form->fields.size(); ++i) {
    const auto& field = m_form->fields[i];
    LauncherResult row;
    row.id = "field:" + std::to_string(i);
    row.providerId = std::string(kFormProviderId);
    row.title = field.label;
    row.subtitle = field.multiline ? escapeNewlines(field.value) : field.value;
    row.kind = field.required ? i18n::tr("launcher.forms.required") : i18n::tr("launcher.forms.optional");
    row.glyphName = i == m_formField ? "pencil" : (field.value.empty() ? "circle" : "circle-check");
    m_allResults.push_back(std::move(row));
  }
  LauncherResult submit;
  submit.id = std::string(kFormSubmitId);
  submit.providerId = std::string(kFormProviderId);
  submit.title = m_form->submitLabel;
  submit.subtitle = m_formError;
  submit.glyphName = m_form->glyph.empty() ? "device-floppy" : m_form->glyph;
  m_allResults.push_back(std::move(submit));

  m_activeCategoryType = All;
  m_anyProviderLoading = false;
  applyActiveCategory();
  m_selectedIndex = std::min(m_formField, m_results.size() - 1);
  if (m_grid != nullptr) {
    m_grid->setSelectedIndex(m_selectedIndex);
  }
  syncFooter();
}

void LauncherPanel::submitForm() {
  if (!m_form.has_value()) {
    return;
  }
  for (std::size_t i = 0; i < m_form->fields.size(); ++i) {
    const auto& field = m_form->fields[i];
    if (field.required && StringUtils::isBlank(field.value)) {
      m_formError = i18n::tr("launcher.forms.errors.required", "field", field.label);
      showFormField(i);
      return;
    }
  }
  m_formError = m_form->submit ? m_form->submit(m_form->fields) : std::string();
  if (!m_formError.empty()) {
    showFormField(m_form->fields.size());
    return;
  }
  endForm(true);
}

void LauncherPanel::runProviderAction(const LauncherResult& result, std::string_view actionId) {
  LauncherProvider* provider = providerFor(result.providerId);
  if (provider == nullptr) {
    return;
  }
  switch (provider->runAction(result, actionId)) {
  case LauncherActionOutcome::Failed:
    return;
  case LauncherActionOutcome::Done:
    finishActivation(*provider, result.id, false);
    return;
  case LauncherActionOutcome::Pasted:
    finishActivation(*provider, result.id, true);
    return;
  case LauncherActionOutcome::KeepOpen:
    reapplyCurrentQuery();
    return;
  }
}

bool LauncherPanel::runSecondaryAction() {
  if (m_selectedIndex >= m_results.size()) {
    return false;
  }
  const LauncherResult result = m_results[m_selectedIndex];
  const LauncherProvider* provider = providerFor(result.providerId);
  if (provider == nullptr) {
    return false;
  }
  const auto actions = provider->actions(result);
  if (actions.empty()) {
    return false;
  }
  runProviderAction(result, actions.front().id);
  return true;
}

bool LauncherPanel::openSelectedActionsMenu() {
  float anchorX = 0.0F;
  float anchorY = 0.0F;
  if (m_grid == nullptr || !m_grid->absoluteAnchorForIndex(m_selectedIndex, anchorX, anchorY)) {
    if (m_grid != nullptr) {
      Node::absolutePosition(m_grid, anchorX, anchorY);
      anchorX += m_grid->width() * 0.5F;
      anchorY += m_grid->height() * 0.5F;
    }
  }
  return openActionsMenu(m_selectedIndex, anchorX, anchorY);
}

bool LauncherPanel::openActionsMenu(std::size_t index, float anchorX, float anchorY) {
  if (index >= m_results.size() || m_aliasTarget.has_value()) {
    return false;
  }
  const LauncherResult base = m_results[index];
  LauncherProvider* provider = providerFor(base.providerId);
  if (provider == nullptr) {
    return false;
  }

  const DesktopEntry* match = nullptr;
  if (!base.desktopEntryPath.empty()) {
    for (const auto& e : desktopEntries()) {
      if (e.path == base.desktopEntryPath) {
        match = &e;
        break;
      }
    }
  }

  WaylandConnection* wl = PanelManager::instance().wayland();
  RenderContext* rc = PanelManager::instance().renderContext();
  if (wl == nullptr || rc == nullptr) {
    return false;
  }

  const auto parentCtx = PanelManager::instance().fallbackPopupParentContext();
  if (!parentCtx.has_value()) {
    return false;
  }

  if (m_actionsMenu == nullptr) {
    m_actionsMenu = std::make_unique<ContextMenuPopup>(*wl, *rc);
  }

  // Ids: desktop actions use their index; provider actions start at kProviderActionBase; the rest are
  // the launcher's own entries below.
  constexpr std::int32_t kActionOpen = -1;
  constexpr std::int32_t kActionPin = -2;
  constexpr std::int32_t kActionUnpin = -3;
  constexpr std::int32_t kActionSetAlias = -4;
  constexpr std::int32_t kActionRemoveAlias = -5;
  constexpr std::int32_t kActionCopyHotkey = -6;
  constexpr std::int32_t kProviderActionBase = 1000;

  std::vector<DesktopAction> desktopActions = match != nullptr ? match->actions : std::vector<DesktopAction>{};
  const std::vector<LauncherAction> providerActions = provider->actions(base);
  const bool launcherPinned = match != nullptr
      && m_config != nullptr
      && shell::dock::pinned_apps::containsEntry(m_config->config().shell.launcher.pinned, *match);

  const auto entry = [](std::int32_t id, std::string label) {
    return ContextMenuControlEntry{.id = id, .label = std::move(label), .enabled = true};
  };
  const auto separator = []() { return ContextMenuControlEntry{.id = 0, .enabled = false, .separator = true}; };

  // Raycast's action panel: the result's name, its primary action, then everything else.
  std::vector<ContextMenuControlEntry> entries;
  entries.push_back(ContextMenuControlEntry{.id = 0, .label = base.title, .enabled = false, .header = true});
  entries.push_back(entry(kActionOpen, primaryActionLabelFor(base)));
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(providerActions.size()); ++i) {
    entries.push_back(entry(kProviderActionBase + i, providerActions[static_cast<std::size_t>(i)].label));
  }
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(desktopActions.size()); ++i) {
    entries.push_back(entry(i, desktopActions[static_cast<std::size_t>(i)].name));
  }
  const bool canPin = match != nullptr && m_config != nullptr;
  if (canPin || provider->supportsAliases()) {
    entries.push_back(separator());
  }
  if (canPin) {
    entries.push_back(
        launcherPinned ? entry(kActionUnpin, i18n::tr("launcher.context-menu.unpin"))
                       : entry(kActionPin, i18n::tr("launcher.context-menu.pin"))
    );
  }
  if (provider->supportsAliases()) {
    entries.push_back(entry(
        kActionSetAlias, i18n::tr(base.alias.empty() ? "launcher.actions.set-alias" : "launcher.actions.change-alias")
    ));
    if (!base.alias.empty()) {
      entries.push_back(entry(kActionRemoveAlias, i18n::tr("launcher.actions.remove-alias")));
    }
    if (m_copyText) {
      entries.push_back(entry(kActionCopyHotkey, i18n::tr("launcher.actions.copy-hotkey-command")));
    }
  }

  const float scale = contentScale();
  constexpr float kMenuWidth = 240.0F;
  const float minMenuWidth = kMenuWidth * scale;

  if (m_config != nullptr) {
    m_actionsMenu->setShadowConfig(m_config->config().shell.shadow);
  }
  PanelManager::instance().beginAttachedPopup(parentCtx->surface);
  PanelManager::instance().setActivePopup(m_actionsMenu.get());

  m_actionsMenu->setOnDismissed([parentSurface = parentCtx->surface]() {
    PanelManager::instance().clearActivePopup();
    PanelManager::instance().endAttachedPopup(parentSurface);
  });

  std::optional<DesktopEntry> entryForPin = match != nullptr ? std::optional<DesktopEntry>(*match) : std::nullopt;
  m_actionsMenu->setOnActivate([this, base, desktopActions = std::move(desktopActions), providerActions,
                                entryForPin = std::move(entryForPin)](const ContextMenuControlEntry& chosen) {
    LauncherResult result = base;
    if (chosen.id >= kProviderActionBase) {
      const auto actionIndex = static_cast<std::size_t>(chosen.id - kProviderActionBase);
      if (actionIndex < providerActions.size()) {
        runProviderAction(result, providerActions[actionIndex].id);
      }
      return;
    }
    if ((chosen.id == kActionPin || chosen.id == kActionUnpin) && entryForPin.has_value() && m_config != nullptr) {
      std::vector<std::string> pinned = m_config->config().shell.launcher.pinned;
      if (chosen.id == kActionPin) {
        if (entryForPin->id.empty() || shell::dock::pinned_apps::containsEntry(pinned, *entryForPin)) {
          return;
        }
        pinned.push_back(entryForPin->id);
      } else {
        shell::dock::pinned_apps::removeEntry(pinned, *entryForPin);
      }
      if (m_config->setOverride({"shell", "launcher", "pinned"}, std::move(pinned))) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (chosen.id == kActionSetAlias) {
      beginAliasEdit(result);
      return;
    }
    if (chosen.id == kActionRemoveAlias) {
      if (m_aliases.removeFor(result.providerId, result.id)) {
        reapplyCurrentQuery();
      }
      return;
    }
    if (chosen.id == kActionCopyHotkey) {
      if (m_copyText) {
        m_copyText("noctalia msg launcher-run " + specFor(result));
        notify::info("Noctalia", i18n::tr("launcher.hotkey.copied"), i18n::tr("launcher.hotkey.copied-body"));
      }
      PanelManager::instance().closePanel(false);
      return;
    }
    if (chosen.id >= 0 && chosen.id < static_cast<std::int32_t>(desktopActions.size())) {
      const DesktopAction& action = desktopActions[static_cast<std::size_t>(chosen.id)];
      result.id = AppProvider::actionResultId(result.desktopEntryPath, action.id);
      result.desktopActionId = action.id;
    } else if (chosen.id != kActionOpen) {
      return;
    }

    LauncherProvider* target = providerFor(result.providerId);
    if (target == nullptr || !target->activate(result)) {
      return;
    }
    finishActivation(*target, result.id, target->supportsAutoPaste());
  });

  const float inset = std::round(std::max(4.0F, Style::spaceXs * scale));
  const auto ax = static_cast<std::int32_t>(std::round(anchorX - inset));
  const auto ay = static_cast<std::int32_t>(std::round(anchorY - inset));
  const auto aw = static_cast<std::int32_t>(std::round(inset * 2.0F));
  const auto ah = static_cast<std::int32_t>(std::round(inset * 2.0F));

  m_actionsMenu->open(
      ContextMenuPopupRequest{
          .entries = std::move(entries),
          .minMenuWidth = minMenuWidth,
          .maxMenuWidth = Style::menuAutoMaxWidth * scale,
          .maxVisible = 14,
          .anchor =
              PopupAnchorRect{
                  .x = ax,
                  .y = ay,
                  .width = std::max(1, aw),
                  .height = std::max(1, ah),
              },
          .parent = PopupSurfaceParent{
              .layerSurface = parentCtx->layerSurface,
              .output = parentCtx->output,
          },
      }
  );
  return true;
}

std::string LauncherPanel::runFromSpec(std::string_view spec) {
  const std::string trimmed = StringUtils::trim(spec);
  if (trimmed.empty()) {
    return "error: launcher-run requires an alias or <provider>:<id>\n";
  }
  for (auto& provider : m_providers) {
    applyProviderConfig(*provider);
  }
  if (m_config != nullptr) {
    m_aliases.setConfigAliases(m_config->config().shell.launcher.aliases);
  }
  std::optional<AliasStore::Target> target = m_aliases.find(trimmed);
  if (!target.has_value()) {
    target = AliasStore::parseSpec(trimmed);
  }
  if (!target.has_value()) {
    return "error: unknown alias\n";
  }
  const auto result = resolveAliasTarget(*target);
  if (!result.has_value()) {
    return "error: no launcher result for \"" + trimmed + "\"\n";
  }
  LauncherProvider* provider = providerFor(result->providerId);
  if (provider == nullptr || !provider->activate(*result)) {
    return "error: could not run \"" + result->title + "\"\n";
  }
  if (shouldTrackUsage() && provider->trackUsage()) {
    m_usageTracker.record(provider->id(), result->id);
  }
  if (PanelManager::instance().isOpenPanel("launcher")) {
    PanelManager::instance().closePanel(false);
  }
  return "ok\n";
}

void LauncherPanel::activateAt(std::size_t index) {
  if (index >= m_results.size()) {
    return;
  }
  m_selectedIndex = index;
  activateSelected();
}

void LauncherPanel::activateSelected() {
  if (m_selectedIndex >= m_results.size()) {
    return;
  }

  const auto& result = m_results[m_selectedIndex];
  if (result.providerId == kFormProviderId && m_form.has_value()) {
    if (result.id == kFormSubmitId) {
      submitForm();
    } else if (m_selectedIndex != m_formField) {
      showFormField(m_selectedIndex); // a click on another field
    } else {
      showFormField(m_formField + 1); // Return: the next field, then the submit row
    }
    return;
  }
  if (result.providerId == kAliasEditorProviderId) {
    if (m_aliasTarget.has_value()) {
      const std::string alias = AliasStore::normalize(m_query);
      if (alias.empty() || alias.contains(' ')) {
        return;
      }
      (void)m_aliases.set(
          alias, AliasStore::Target{.providerId = m_aliasTarget->providerId, .resultId = m_aliasTarget->id}
      );
    }
    endAliasEdit();
    return;
  }
  if (result.providerId == kProviderOverviewProviderId && result.id.starts_with(kProviderOverviewResultPrefix)) {
    std::string prefix = result.id.substr(kProviderOverviewResultPrefix.size());
    if (!prefix.empty()) {
      prefix += ' ';
    }
    if (m_input != nullptr) {
      m_input->setValue(prefix);
    }
    if (m_grid != nullptr) {
      m_grid->scrollView().setScrollOffset(0.0F);
    }
    onInputChanged(prefix);
    return;
  }

  // Dispatch only to the provider that produced this result. Providers can use
  // overlapping id shapes, so probing every provider risks side effects.
  for (auto& provider : m_providers) {
    if (provider->id() != std::string_view(result.providerId)) {
      continue;
    }

    if (!provider->activate(result)) {
      return;
    }

    finishActivation(*provider, result.id, provider->supportsAutoPaste());
    return;
  }
}

bool LauncherPanel::handleKeyEvent(std::uint32_t sym, std::uint32_t modifiers) {
  const bool gridNav = m_usingAppGrid && m_grid != nullptr;
  const int columns = gridNav ? static_cast<int>(std::max<std::size_t>(1, m_grid->layoutColumnCount())) : 1;

  const auto moveSelection = [this](int delta) {
    if (m_results.empty()) {
      return;
    }
    const int last = static_cast<int>(m_results.size() - 1);
    const int next = std::clamp(static_cast<int>(m_selectedIndex) + delta, 0, last);
    if (next == static_cast<int>(m_selectedIndex)) {
      return;
    }
    m_selectedIndex = static_cast<std::size_t>(next);
    if (m_grid != nullptr) {
      m_grid->setSelectedIndex(m_selectedIndex);
    }
  };

  const auto cycleCategory = [this](bool reverse) {
    if (m_categoryFilter == nullptr) {
      return false;
    }
    const std::size_t total = m_categoryFilterSlots.size();
    if (total == 0) {
      return false;
    }

    const bool wasVisible = m_categoryFilter->visible();
    m_categoryFilterVisible = true;
    setCategoryFilterVisible(true);
    if (!wasVisible) {
      return true;
    }

    const std::size_t selected = std::min(m_categoryFilter->selectedIndex(), total - 1);
    const std::size_t next =
        reverse ? (selected == 0 ? total - 1 : selected - 1) : (selected + 1 < total ? selected + 1 : 0);
    m_categoryFilter->setSelectedIndex(next);
    return true;
  };

  if (m_form.has_value()) {
    const bool back = (sym == XKB_KEY_Tab && (modifiers & KeyMod::Shift) != 0)
        || sym == XKB_KEY_ISO_Left_Tab
        || KeybindMatcher::matches(KeybindAction::Up, sym, modifiers);
    const bool forward =
        (sym == XKB_KEY_Tab && modifiers == 0) || KeybindMatcher::matches(KeybindAction::Down, sym, modifiers);
    if (back) {
      if (m_formField > 0) {
        showFormField(m_formField - 1);
      }
      return true;
    }
    if (forward) {
      showFormField(m_formField + 1);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
      endForm(false);
      return true;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
      activateSelected();
      return true;
    }
    // Shift/Ctrl+Return and the actions menu don't apply inside a form.
    if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers & ~(KeyMod::Shift | KeyMod::Ctrl))) {
      return true;
    }
  }

  if (sym == XKB_KEY_F6 && (modifiers & ~(KeyMod::Shift)) == 0) {
    return cycleCategory((modifiers & KeyMod::Shift) != 0);
  }

  if (KeySymbol::isPageUp(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(-stride);
    return true;
  }

  if (KeySymbol::isPageDown(sym)) {
    const int stride = m_grid != nullptr ? static_cast<int>(m_grid->pageItemStride()) : 1;
    moveSelection(stride);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    moveSelection(gridNav ? -columns : -1);
    return true;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    moveSelection(gridNav ? columns : 1);
    return true;
  }

  if (gridNav && KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
    moveSelection(-1);
    return true;
  }

  if (gridNav && KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
    moveSelection(1);
    return true;
  }

  if (m_aliasTarget.has_value() && KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    endAliasEdit();
    return true;
  }

  // Validate+Shift opens the actions menu (Shift layered on the configured Validate chord; Raycast's
  // Cmd+K is the field's kill-line). Menu navigation uses the Up/Down/Validate/Cancel keybinds.
  if ((modifiers & KeyMod::Shift) != 0
      && KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers & ~KeyMod::Shift)) {
    return openSelectedActionsMenu();
  }

  // Validate+Ctrl runs the first extra action, like Raycast's Cmd+Return.
  if ((modifiers & KeyMod::Ctrl) != 0
      && KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers & ~KeyMod::Ctrl)) {
    return runSecondaryAction();
  }

  if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    activateSelected();
    return true;
  }

  return false;
}

std::unique_ptr<Node> LauncherPanel::buildFooter(float scale) {
  const float captionSize = Style::fontSizeCaption * scale;
  const ColorSpec muted = colorSpecFromRole(ColorRole::OnSurfaceVariant);
  const auto keycap = [&](Label** out, std::string text) {
    auto cap = ui::row({
        .align = FlexAlign::Center,
        .paddingV = 1.0F * scale,
        .paddingH = Style::spaceXs * 1.5F * scale,
        .fill = colorSpecFromRole(ColorRole::OnSurface, Style::hoverFillAlpha * 1.5F),
        .radius = Style::radiusSm * scale,
    });
    cap->addChild(
        ui::label({
            .out = out,
            .text = std::move(text),
            .fontSize = Style::fontSizeMini * scale,
            .fontWeight = FontWeight::Medium,
            .color = muted,
        })
    );
    return cap;
  };
  const auto hint = [&](Label** labelOut, std::string label) {
    return ui::label({
        .out = labelOut,
        .text = std::move(label),
        .fontSize = captionSize,
        .fontWeight = FontWeight::Medium,
        .color = colorSpecFromRole(ColorRole::OnSurface),
    });
  };

  // Raycast-style action bar: what is selected on the left, its keys on the right.
  auto footer = ui::column({
      .out = &m_footer,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * 1.5F * scale,
      .visible = false,
      .participatesInLayout = false,
  });
  footer->addChild(
      ui::separator({
          .color = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .thickness = 1.0F,
          .spacing = 0.0F,
      })
  );
  auto bar = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .gap = Style::spaceSm * scale,
      .paddingH = Style::spaceSm * scale,
  });
  bar->addChild(
      ui::label({
          .out = &m_footerKind,
          .fontSize = captionSize,
          .color = muted,
          .maxLines = 1,
          .flexGrow = 1.0F,
      })
  );
  auto keys = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * 1.5F * scale});
  keys->addChild(hint(&m_footerPrimary, i18n::tr("launcher.footer.open")));
  keys->addChild(keycap(nullptr, "↵"));
  keys->addChild(
      ui::separator({
          .out = &m_footerActionsSeparator,
          .color = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .thickness = 1.0F,
          .spacing = Style::spaceXs * scale,
          .height = captionSize,
      })
  );
  auto actions = ui::row({.out = &m_footerActions, .align = FlexAlign::Center, .gap = Style::spaceXs * 1.5F * scale});
  actions->addChild(hint(nullptr, i18n::tr("launcher.footer.actions")));
  // Shift+Return rather than Raycast's Cmd/Ctrl+K, which the field keeps for kill-to-end-of-line.
  actions->addChild(keycap(nullptr, "Shift"));
  actions->addChild(keycap(nullptr, "↵"));
  keys->addChild(std::move(actions));
  bar->addChild(std::move(keys));
  footer->addChild(std::move(bar));
  return footer;
}

void LauncherPanel::syncFooter() {
  if (m_footer == nullptr || m_footerKind == nullptr) {
    return;
  }
  const bool show = !m_results.empty() && !shouldUseDetailPresentation();
  m_footer->setVisible(show);
  m_footer->setParticipatesInLayout(show);
  if (!show) {
    return;
  }
  const LauncherResult& selected = m_results[std::min(m_selectedIndex, m_results.size() - 1)];
  if (selected.providerId == kAliasEditorProviderId) {
    m_footerKind->setText(i18n::tr("launcher.aliases.footer"));
    m_footerPrimary->setText(i18n::tr("launcher.actions.save-alias"));
  } else if (selected.providerId == kFormProviderId && m_form.has_value()) {
    m_footerKind->setText(i18n::tr("launcher.forms.footer"));
    m_footerPrimary->setText(
        selected.id == kFormSubmitId ? m_form->submitLabel : i18n::tr("launcher.forms.next-field")
    );
  } else {
    m_footerKind->setText(selected.kind.empty() ? sectionTitleFor(selected.providerId) : selected.kind);
    m_footerPrimary->setText(primaryActionLabelFor(selected));
  }
  const bool actions =
      selected.providerId != kAliasEditorProviderId && selected.providerId != kFormProviderId && hasActions(selected);
  m_footerActions->setVisible(actions);
  m_footerActions->setParticipatesInLayout(actions);
  m_footerActionsSeparator->setVisible(actions);
  m_footerActionsSeparator->setParticipatesInLayout(actions);
}

std::string LauncherPanel::sectionTitleFor(std::string_view providerId) const {
  if (providerId == "Applications") {
    return i18n::tr("launcher.sections.applications");
  }
  if (providerId == "Files") {
    return i18n::tr("launcher.sections.files");
  }
  if (providerId == "Windows") {
    return i18n::tr("launcher.sections.windows");
  }
  if (providerId == "Session" || providerId == "Panels") {
    return i18n::tr("launcher.sections.commands");
  }
  for (const auto& provider : m_providers) {
    if (provider->id() == providerId) {
      return provider->displayName();
    }
  }
  return std::string(providerId);
}

std::string LauncherPanel::kindFor(const LauncherResult& result) const {
  if (!result.kind.empty()) {
    return result.kind;
  }
  if (result.providerId == "Applications") {
    return i18n::tr("launcher.kinds.application");
  }
  if (result.providerId == "Windows") {
    return i18n::tr("launcher.kinds.window");
  }
  if (result.providerId == "Session" || result.providerId == "Panels") {
    return i18n::tr("launcher.kinds.command");
  }
  return {};
}

void LauncherPanel::assignSections() {
  for (LauncherResult& result : m_results) {
    result.section.clear();
    result.kind = kindFor(result);
  }
  if (m_form.has_value() && !m_results.empty()) {
    m_results.front().section = m_form->title;
    return;
  }
  if (!m_mixedResults || m_activeCategoryType != All || m_results.empty()) {
    return;
  }

  if (StringUtils::isBlank(m_query)) {
    // Nothing typed: pinned apps lead as Favourites, everything else follows as Applications.
    const auto firstUnpinned = std::ranges::find_if(m_results, [](const LauncherResult& r) { return !r.pinned; });
    if (firstUnpinned == m_results.begin()) {
      m_results.front().section = sectionTitleFor(m_results.front().providerId);
      return;
    }
    m_results.front().section = i18n::tr("launcher.sections.favourites");
    if (firstUnpinned != m_results.end()) {
      firstUnpinned->section = sectionTitleFor(firstUnpinned->providerId);
    }
    return;
  }

  // Raycast: one ranked list of results, files after it in their own section, and the fallbacks
  // ("Search the web for …") last under "Use “query” with…".
  std::vector<LauncherResult> ordered;
  ordered.reserve(m_results.size());
  for (LauncherResult& result : m_results) {
    if (result.providerId != "Files" && !result.fallback) {
      ordered.push_back(std::move(result));
    }
  }
  const std::size_t fileStart = ordered.size();
  for (LauncherResult& result : m_results) {
    if (result.providerId == "Files" && !result.fallback) {
      ordered.push_back(std::move(result));
    }
  }
  const std::size_t fallbackStart = ordered.size();
  for (LauncherResult& result : m_results) {
    if (result.fallback) {
      ordered.push_back(std::move(result));
    }
  }
  if (fileStart > 0) {
    ordered.front().section = i18n::tr("launcher.sections.results");
  }
  if (fileStart < fallbackStart) {
    ordered[fileStart].section = i18n::tr("launcher.sections.files");
  }
  if (fallbackStart < ordered.size()) {
    ordered[fallbackStart].section = i18n::tr("launcher.sections.fallback", "query", StringUtils::trim(m_query));
  }
  m_results = std::move(ordered);
}

std::unique_ptr<Node> LauncherPanel::buildPreviewPane(float scale) {
  constexpr std::size_t kMetadataRows = 6;
  const float captionSize = Style::fontSizeCaption * scale;
  auto pane = ui::column({
      .out = &m_previewPane,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .paddingV = Style::spaceXs * scale,
      .paddingH = Style::spaceSm * scale,
      .fillHeight = true,
      .visible = false,
      .participatesInLayout = false,
  });
  pane->addChild(
      ui::image({
          .out = &m_previewImage,
          .fit = ImageFit::Contain,
          .radius = Style::radiusSm * scale,
          .height = 168.0F * scale,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  pane->addChild(
      ui::label({
          .out = &m_previewBadge,
          .fontSize = 64.0F * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .textAlign = TextAlign::Center,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  pane->addChild(
      ui::label({
          .out = &m_previewTitle,
          .fontSize = Style::fontSizeBody * scale,
          .fontWeight = FontWeight::SemiBold,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 2,
          .ellipsize = TextEllipsize::End,
          .visible = false,
          .participatesInLayout = false,
      })
  );
  auto bodyScroll = ui::scrollView({
      .contentScale = scale,
      .scrollbarVisible = true,
      .flexGrow = 1.0F,
  });
  auto* bodyContent = bodyScroll->content();
  bodyContent->setDirection(FlexDirection::Vertical);
  bodyContent->setAlign(FlexAlign::Stretch);
  bodyContent->addChild(
      ui::label({
          .out = &m_previewBody,
          .fontSize = Style::fontSizeCaption * scale,
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 0,
      })
  );
  pane->addChild(std::move(bodyScroll));
  pane->addChild(
      ui::separator({
          .out = &m_previewMetaDivider,
          .color = colorSpecFromRole(ColorRole::Outline, Style::hairlineAlpha),
          .thickness = 1.0F,
          .spacing = 0.0F,
      })
  );
  m_previewMetaRows.clear();
  m_previewMetaLabels.clear();
  m_previewMetaValues.clear();
  for (std::size_t i = 0; i < kMetadataRows; ++i) {
    Flex* row = nullptr;
    Label* label = nullptr;
    Label* value = nullptr;
    auto metaRow = ui::row({
        .out = &row,
        .align = FlexAlign::Center,
        .justify = FlexJustify::SpaceBetween,
        .gap = Style::spaceSm * scale,
        .visible = false,
        .participatesInLayout = false,
    });
    metaRow->addChild(
        ui::label({
            .out = &label,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 1,
        })
    );
    metaRow->addChild(
        ui::label({
            .out = &value,
            .fontSize = captionSize,
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::Middle,
        })
    );
    pane->addChild(std::move(metaRow));
    m_previewMetaRows.push_back(row);
    m_previewMetaLabels.push_back(label);
    m_previewMetaValues.push_back(value);
  }
  return pane;
}

void LauncherPanel::syncPreview() {
  if (m_previewPane == nullptr || m_previewDivider == nullptr) {
    return;
  }
  const bool show = m_previewProvider != nullptr && !m_results.empty() && m_aliasTarget == std::nullopt;
  const bool wasShown = m_previewPane->visible();
  m_previewPane->setVisible(show);
  m_previewPane->setParticipatesInLayout(show);
  m_previewDivider->setVisible(show);
  m_previewDivider->setParticipatesInLayout(show);
  if (wasShown != show && m_container != nullptr) {
    m_container->markLayoutDirty();
  }
  if (!show) {
    m_previewKey.clear();
    return;
  }

  const LauncherResult& selected = m_results[std::min(m_selectedIndex, m_results.size() - 1)];
  const std::string key = selected.providerId + "\n" + selected.id;
  if (key == m_previewKey) {
    return;
  }
  m_previewKey = key;

  const std::optional<LauncherPreview> preview = m_previewProvider->preview(selected);
  const LauncherPreview empty{};
  const LauncherPreview& data = preview.has_value() ? *preview : empty;

  // Images decode in doLayout, where the renderer is at hand.
  const bool hasImage = !data.imagePath.empty() || !data.imageBytes.empty();
  m_previewImage->setVisible(false);
  m_previewImage->setParticipatesInLayout(hasImage);
  m_pendingPreviewImagePath = data.imagePath;
  m_pendingPreviewImageBytes = data.imageBytes;
  m_previewImageDirty = hasImage;

  const bool hasBadge = !data.badge.empty();
  m_previewBadge->setVisible(hasBadge);
  m_previewBadge->setParticipatesInLayout(hasBadge);
  m_previewBadge->setText(data.badge);

  const bool hasTitle = !data.title.empty();
  m_previewTitle->setVisible(hasTitle);
  m_previewTitle->setParticipatesInLayout(hasTitle);
  m_previewTitle->setText(data.title);

  m_previewBody->setText(data.body);

  const bool hasMeta = !data.metadata.empty();
  m_previewMetaDivider->setVisible(hasMeta);
  m_previewMetaDivider->setParticipatesInLayout(hasMeta);
  for (std::size_t i = 0; i < m_previewMetaRows.size(); ++i) {
    const bool used = i < data.metadata.size();
    m_previewMetaRows[i]->setVisible(used);
    m_previewMetaRows[i]->setParticipatesInLayout(used);
    if (used) {
      m_previewMetaLabels[i]->setText(data.metadata[i].first);
      m_previewMetaValues[i]->setText(data.metadata[i].second);
    }
  }
  if (m_container != nullptr) {
    m_container->markLayoutDirty();
  }
}
