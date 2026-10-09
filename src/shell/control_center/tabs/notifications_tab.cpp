#include "shell/control_center/tabs/notifications_tab.h"

#include "compositors/compositor_platform.h"
#include "core/log.h"
#include "i18n/i18n.h"
#include "net/uri.h"
#include "notification/notification.h"
#include "notification/notification_display_name.h"
#include "notification/notification_manager.h"
#include "render/animation/animation.h"
#include "render/core/renderer.h"
#include "render/core/texture_manager.h"
#include "shell/control_center/control_center_panel.h"
#include "shell/panel/panel_button_style.h"
#include "shell/panel/panel_manager.h"
#include "time/time_format.h"
#include "ui/builders.h"
#include "ui/controls/scroll_view.h"
#include "ui/node_motion.h"
#include "ui/palette.h"
#include "ui/scroll_into_view.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace control_center;

namespace {

  constexpr Logger kLog("control-center-notifications");

  constexpr float kHistoryIconSize = 36.0F;
  constexpr float kHistoryIconGlyphSize = 22.0F;
  constexpr float kHistoryIconReferenceSize = 36.0F;

  float notificationIconRadius(float iconSize, float localScale) {
    const float baseRadius = Style::radiusMd * (iconSize / kHistoryIconReferenceSize);
    return std::min(iconSize * 0.5F, Style::scaledRadius(baseRadius, localScale));
  }
  constexpr float kNotificationActionButtonSize = Style::controlHeightSm;
  // Keep dismissal visible alongside the time, including for keyboard users.
  constexpr float kHistoryTimeWidth = 56.0F;

  std::string historyActionLabel(std::string_view actionKey, std::string_view actionLabel) {
    if (!StringUtils::isBlank(actionLabel)) {
      return std::string(actionLabel);
    }
    if (actionKey == "default") {
      return i18n::tr("notifications.actions.open");
    }
    if (actionKey == "inline-reply") {
      return i18n::tr("notifications.inline-reply.button");
    }
    return i18n::tr("notifications.actions.fallback");
  }

  float measureHistoryActionsRowHeight(
      Renderer& renderer, const std::vector<std::string>& actions, float cardTextWidth, float scale
  ) {
    if (actions.empty()) {
      return 0.0F;
    }
    auto container = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
    });

    std::vector<std::unique_ptr<Button>> buttons;
    const std::size_t limit = std::min(actions.size(), kMaxNotificationActions * 2);
    for (std::size_t i = 0; i + 1 < limit; i += 2) {
      const std::string& actionKey = actions[i];
      if (actionKey.empty()) {
        continue;
      }
      buttons.push_back(
          ui::button({
              .text = historyActionLabel(actionKey, actions[i + 1]),
              .fontSize = Style::fontSizeCaption * scale,
              .variant = ButtonVariant::Default,
          })
      );
    }

    auto rows = wrapButtonsIntoRows(renderer, buttons, cardTextWidth, Style::spaceXs * scale);
    populateRowContainer(*container, std::move(rows), cardTextWidth, Style::spaceXs * scale);

    if (container->children().empty()) {
      return 0.0F;
    }

    container->setSize(cardTextWidth, 0.0F);
    container->layout(renderer);
    return container->height();
  }
  constexpr int kSummaryMaxLines = 2;
  constexpr int kBodyMaxLines = 3;
  constexpr int kExpandedMaxLines = 500;

  std::filesystem::path remoteNotificationIconCachePath(std::string_view url) {
    return std::filesystem::path("/tmp")
        / "noctalia-notification-icons"
        / (std::to_string(std::hash<std::string_view>{}(url)) + ".img");
  }

  std::string normalizeLocalIconPath(std::string_view iconValue) { return uri::normalizeFileUrl(iconValue); }

  // The sending app's own icon, looked up by desktop entry, then by its lowercased name, as the
  // Island does: many apps send a generic theme icon (or none) and would otherwise show a bell.
  std::string resolveAppIconPath(const Notification& n, IconResolver& resolver, int targetSize) {
    std::vector<std::string> names;
    if (n.desktopEntry.has_value() && !n.desktopEntry->empty()) {
      names.push_back(*n.desktopEntry);
    }
    if (!n.appName.empty()) {
      std::string lower = StringUtils::toLower(n.appName);
      std::ranges::replace(lower, ' ', '-');
      names.push_back(std::move(lower));
    }
    for (const auto& name : names) {
      if (const std::string& resolved = resolver.resolve(name, targetSize); !resolved.empty()) {
        return resolved;
      }
    }
    return {};
  }

  std::string resolveNotificationIconPath(const Notification& n, IconResolver& resolver, int targetSize) {
    if (!n.icon.has_value() || n.icon->empty()) {
      return {};
    }
    const std::string& iconValue = *n.icon;
    if (uri::isRemoteUrl(iconValue)) {
      const auto cached = remoteNotificationIconCachePath(iconValue);
      std::error_code ec;
      if (std::filesystem::exists(cached, ec) && std::filesystem::file_size(cached, ec) > 0) {
        return cached.string();
      }
      return {};
    }

    const std::string localPath = normalizeLocalIconPath(iconValue);
    if (!localPath.empty() && localPath.front() == '/') {
      if (access(localPath.c_str(), R_OK) == 0) {
        return localPath;
      }
      return {};
    }
    if (localPath.empty()) {
      return {};
    }

    const std::string& resolved = resolver.resolve(localPath, targetSize);
    return resolved.empty() ? std::string() : resolved;
  }

  void applyNotificationCardStyle(Flex& card, float scale, float fillOpacity) {
    applySectionCardStyle(card, scale, fillOpacity);
  }

  std::string relativeMetaLine(const Notification& n) {
    if (n.receivedWallClock.has_value()) {
      return formatNotificationTime(*n.receivedWallClock);
    }
    const auto age = std::chrono::duration_cast<WallClock::duration>(Clock::now() - n.receivedTime);
    return formatNotificationTime(WallClock::now() - age);
  }

  // Notifications stack per app, as in macOS Notification Center.
  std::string historyGroupKey(const Notification& n) { return notificationDisplayAppName(n); }

  constexpr float kStackPlatePeek = 7.0F;
  constexpr float kStackPlateInset = 10.0F;
  constexpr std::size_t kMaxStackPlates = 2;
  constexpr float kGroupHeaderHeight = Style::controlHeightSm;

  std::size_t stackPlateCount(std::size_t groupSize) {
    return groupSize > 1 ? std::min(groupSize - 1, kMaxStackPlates) : 0;
  }

  float stackPlatesHeight(std::size_t groupSize, float scale) {
    return static_cast<float>(stackPlateCount(groupSize)) * kStackPlatePeek * scale;
  }

  std::int64_t currentRelativeTimeSlot() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
        / 15;
  }

  float measuredTextHeight(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth, int maxLines
  ) {
    if (text.empty()) {
      return 0.0F;
    }
    const auto bounds = renderer.measureText(text, fontSize, fontWeight, maxWidth, maxLines);
    return std::max(0.0F, bounds.bottom - bounds.top);
  }

  bool canExpandText(
      Renderer& renderer, std::string_view text, float fontSize, FontWeight fontWeight, float maxWidth,
      int collapsedMaxLines
  ) {
    if (text.empty()) {
      return false;
    }

    const float collapsedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, collapsedMaxLines);
    const float expandedHeight = measuredTextHeight(renderer, text, fontSize, fontWeight, maxWidth, kExpandedMaxLines);
    return expandedHeight > collapsedHeight + 0.5F;
  }

  struct NotificationCardMetrics {
    std::string summaryText;
    std::string bodyText;
    std::string metaLine; // the app's name
    std::string timeText; // "now", "5m ago", …
    bool canExpand = false;
    bool expanded = false;
    float height = 0.0F;
    float cardTextWidth = 0.0F;
    float metaTextWidth = 0.0F;
  };

  NotificationCardMetrics measureNotificationCard(
      Renderer& renderer, const NotificationHistoryEntry& entry, float scale, float width, bool expandedRequested,
      bool showHistoryActions, std::size_t stackSize = 1
  ) {
    NotificationCardMetrics metrics;
    const float cardWidth = std::max(0.0F, width);
    const float cardHorizontalPadding = Style::spaceLg * scale * 2.0F;
    metrics.cardTextWidth = std::max(0.0F, cardWidth - cardHorizontalPadding);
    const std::string summaryText = StringUtils::trimLeadingBlankLines(
        entry.notification.summary.empty() ? i18n::tr("control-center.notifications.untitled")
                                           : entry.notification.summary
    );
    const std::string bodyText = StringUtils::trimLeadingBlankLines(entry.notification.body);
    metrics.summaryText = summaryText;

    const bool summaryExpandable = canExpandText(
        renderer, summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth, kSummaryMaxLines
    );
    bool bodyLineTruncated = false;
    const std::string collapsedBodyText = StringUtils::truncateByLines(bodyText, kBodyMaxLines, &bodyLineTruncated);
    const bool bodyExpandable = bodyLineTruncated
        || canExpandText(renderer, bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
                         kBodyMaxLines);
    metrics.canExpand = summaryExpandable || bodyExpandable;
    metrics.expanded = metrics.canExpand && expandedRequested;
    metrics.bodyText = metrics.expanded ? bodyText : collapsedBodyText;

    const float iconPx = kHistoryIconSize * scale;
    const float iconColumn = iconPx + Style::spaceSm * scale;
    const float actionButtonSize = kNotificationActionButtonSize * scale;
    const float actionButtonsGap = Style::spaceXs * scale;
    const float headerActionsWidth = kHistoryTimeWidth * scale
        + actionButtonSize
        + actionButtonsGap
        + (metrics.canExpand || stackSize > 1 ? (actionButtonsGap + actionButtonSize) : 0.0F);
    const float leftClusterWidth = metrics.cardTextWidth - headerActionsWidth;
    metrics.metaTextWidth = std::max(0.0F, leftClusterWidth - iconColumn);

    metrics.metaLine = notificationDisplayAppName(entry.notification);
    if (stackSize > 1)
      metrics.metaLine += " (" + std::to_string(stackSize) + ")";
    metrics.timeText = relativeMetaLine(entry.notification);

    const float metaHeight = measuredTextHeight(
        renderer, metrics.metaLine, Style::fontSizeMini * scale, FontWeight::Normal, metrics.metaTextWidth, 0
    );
    const float headerHeight = std::max({iconPx, actionButtonSize, metaHeight});
    const float summaryHeight = measuredTextHeight(
        renderer, metrics.summaryText, Style::fontSizeBody * scale, FontWeight::Bold, metrics.cardTextWidth,
        metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines
    );
    const float bodyHeight = metrics.bodyText.empty()
        ? 0.0F
        : measuredTextHeight(
              renderer, metrics.bodyText, Style::fontSizeBody * scale, FontWeight::Normal, metrics.cardTextWidth,
              metrics.expanded ? kExpandedMaxLines : kBodyMaxLines
          );

    const float actionsRowHeight = showHistoryActions
        ? measureHistoryActionsRowHeight(renderer, entry.notification.actions, metrics.cardTextWidth, scale)
        : 0.0F;

    const float paddingY = Style::spaceLg * scale * 2.0F;
    int visibleSegments = 2;
    if (!metrics.bodyText.empty()) {
      ++visibleSegments;
    }
    if (actionsRowHeight > 0.5F) {
      ++visibleSegments;
    }
    const float gaps = Style::spaceSm * scale * static_cast<float>(std::max(0, visibleSegments - 1));
    metrics.height = paddingY + headerHeight + summaryHeight + bodyHeight + actionsRowHeight + gaps;
    return metrics;
  }

  std::uint64_t rawImageKey(const NotificationHistoryEntry& entry) {
    return (static_cast<std::uint64_t>(entry.notification.id) << 32U) ^ entry.eventSerial;
  }

  std::uint64_t revisionForEntry(const NotificationHistoryEntry& entry, bool expanded, std::int64_t relativeSlot) {
    std::uint64_t revision = entry.eventSerial;
    revision ^= static_cast<std::uint64_t>(relativeSlot < 0 ? 0 : relativeSlot) * 0x9E3779B185EBCA87ULL;
    if (expanded) {
      revision ^= 0xD1B54A32D192ED03ULL;
    }
    return revision;
  }

  class NotificationHistoryRow final : public Flex {
  public:
    explicit NotificationHistoryRow(float scale, float fillOpacity) : m_scale(scale) {
      applyNotificationCardStyle(*this, scale, fillOpacity);
      setFillWidth(true);

      m_header = static_cast<Flex*>(addChild(
          ui::row({
              .align = FlexAlign::Center,
              .justify = FlexJustify::SpaceBetween,
              .gap = Style::spaceSm * scale,
          })
      ));

      m_leftCluster = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_iconSlot = static_cast<Box*>(m_leftCluster->addChild(
          ui::box({
              .fill = colorSpecFromRole(ColorRole::SurfaceVariant),
              .radius = notificationIconRadius(kHistoryIconSize, scale),
              .width = kHistoryIconSize * scale,
              .height = kHistoryIconSize * scale,
          })
      ));

      m_image = static_cast<Image*>(m_iconSlot->addChild(
          ui::image({
              .visible = false,
          })
      ));

      m_fallback = static_cast<Glyph*>(m_iconSlot->addChild(
          ui::glyph({
              .glyph = "bell",
              .visible = false,
          })
      ));

      m_meta = static_cast<Label*>(m_leftCluster->addChild(
          ui::label({
              .fontSize = Style::fontSizeMini * scale,
              .flexGrow = 1.0F,
          })
      ));

      m_headerActions = static_cast<Flex*>(m_header->addChild(
          ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * scale,
          })
      ));

      m_expand = static_cast<Button*>(m_headerActions->addChild(makeActionButton("chevron-down", scale)));
      m_time = static_cast<Label*>(m_headerActions->addChild(
          ui::label({
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .minWidth = kHistoryTimeWidth * scale,
              .maxWidth = kHistoryTimeWidth * scale,
              .maxLines = 1,
              .textAlign = TextAlign::End,
          })
      ));
      m_dismiss = static_cast<Button*>(m_headerActions->addChild(makeActionButton("close", scale)));
      m_dismiss->setRadius(kNotificationActionButtonSize * scale * 0.5F);

      m_summary = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::Bold,
          })
      ));

      m_body = static_cast<Label*>(addChild(
          ui::label({
              .fontSize = Style::fontSizeBody * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .visible = false,
          })
      ));

      m_actionsRow = static_cast<Flex*>(addChild(
          ui::column({
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * scale,
              .fillWidth = true,
              .visible = false,
          })
      ));
    }

    void bind(
        Renderer& renderer, const NotificationHistoryEntry& entry, float width, bool expanded, bool showHistoryActions,
        std::size_t stackSize, IconResolver& iconResolver, std::function<void(uint32_t)> onToggleExpanded,
        std::function<void(uint32_t, bool)> onRemove, const std::function<void(uint32_t, const std::string&)>& onAction,
        std::function<void()> onExpandStack
    ) {
      const NotificationCardMetrics metrics =
          measureNotificationCard(renderer, entry, m_scale, width, expanded, showHistoryActions, stackSize);
      setMinWidth(width);
      setSize(width, metrics.height);

      const float iconPx = kHistoryIconSize * m_scale;
      m_iconSlot->setSize(iconPx, iconPx);

      bindIcon(renderer, entry, iconResolver);

      m_meta->setText(metrics.metaLine);
      m_meta->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_meta->setMaxWidth(metrics.metaTextWidth);
      m_meta->measure(renderer);

      m_time->setText(metrics.timeText);
      m_time->setVisible(true);
      m_time->measure(renderer);
      m_dismiss->setVisible(true);
      m_dismiss->setTooltip(
          stackSize > 1 ? i18n::tr("control-center.notifications.clear-group", "app", metrics.metaLine)
                        : i18n::tr("notifications.dismiss")
      );
      m_dismiss->inputArea()->setTabFocusKey("notification-dismiss-" + std::to_string(entry.notification.id));

      m_expand->setVisible(stackSize > 1 || metrics.canExpand);
      m_expand->setEnabled(stackSize > 1 || metrics.canExpand);
      m_expand->setGlyph(stackSize == 1 && metrics.expanded ? "chevron-up" : "chevron-down");
      m_expand->setTooltip(
          stackSize > 1 ? i18n::trp("notifications.expand-group", stackSize)
                        : i18n::tr(metrics.expanded ? "notifications.collapse" : "notifications.expand")
      );
      m_expand->inputArea()->setTabFocusKey("notification-expand-" + std::to_string(entry.notification.id));
      m_expand->setOnClick([onToggleExpanded = std::move(onToggleExpanded), onExpandStack = std::move(onExpandStack),
                            stackSize, id = entry.notification.id]() {
        if (stackSize > 1)
          onExpandStack();
        else
          onToggleExpanded(id);
      });

      m_dismiss->setOnClick([onRemove = std::move(onRemove), id = entry.notification.id, active = entry.active]() {
        onRemove(id, active);
      });

      m_summary->setText(metrics.summaryText);
      m_summary->setMaxWidth(metrics.cardTextWidth);
      m_summary->setMaxLines(metrics.expanded ? kExpandedMaxLines : kSummaryMaxLines);
      m_summary->measure(renderer);

      if (metrics.bodyText.empty()) {
        m_body->setVisible(false);
        m_body->setText("");
      } else {
        m_body->setVisible(true);
        m_body->setText(metrics.bodyText);
        m_body->setMaxWidth(metrics.cardTextWidth);
        m_body->setMaxLines(metrics.expanded ? kExpandedMaxLines : kBodyMaxLines);
        m_body->measure(renderer);
      }

      while (!m_actionsRow->children().empty()) {
        m_actionsRow->removeChild(m_actionsRow->children().back().get());
      }
      m_actionsRow->setVisible(false);
      if (showHistoryActions && !entry.notification.actions.empty()) {
        std::vector<std::unique_ptr<Button>> buttons;
        const std::size_t limit = std::min(entry.notification.actions.size(), kMaxNotificationActions * 2);
        for (std::size_t i = 0; i + 1 < limit; i += 2) {
          const std::string& actionKey = entry.notification.actions[i];
          if (actionKey.empty()) {
            continue;
          }
          auto button = ui::button({
              .text = historyActionLabel(actionKey, entry.notification.actions[i + 1]),
              .fontSize = Style::fontSizeCaption * m_scale,
              .variant = ButtonVariant::Default,
          });
          button->setOnClick([onAction, id = entry.notification.id, key = std::string(actionKey)]() {
            onAction(id, key);
          });
          button->inputArea()->setTabFocusKey(
              "notification-action-" + std::to_string(entry.notification.id) + "-" + actionKey
          );
          buttons.push_back(std::move(button));
        }

        auto rows = wrapButtonsIntoRows(renderer, buttons, metrics.cardTextWidth, Style::spaceXs * m_scale);
        populateRowContainer(*m_actionsRow, std::move(rows), metrics.cardTextWidth, Style::spaceXs * m_scale);

        m_actionsRow->setVisible(!m_actionsRow->children().empty());
      }
    }

  private:
    enum class ImageKind {
      None,
      File,
      Raw,
    };

    static std::unique_ptr<Button> makeActionButton(std::string_view glyph, float scale) {
      return ui::button({
          .glyph = std::string(glyph),
          .glyphSize = Style::fontSizeBody * scale,
          .variant = ButtonVariant::Ghost,
          .minWidth = kNotificationActionButtonSize * scale,
          .minHeight = kNotificationActionButtonSize * scale,
          .padding = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
      });
    }

    void showFallbackIcon(Renderer& renderer) {
      if (m_imageKind != ImageKind::None) {
        m_image->clear(renderer);
      }
      m_imageKind = ImageKind::None;
      m_rawImageKey = 0;
      m_image->setVisible(false);

      const float iconPx = kHistoryIconSize * m_scale;
      m_fallback->setGlyph("bell");
      m_fallback->setGlyphSize(kHistoryIconGlyphSize * m_scale);
      m_fallback->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
      m_fallback->measure(renderer);
      m_fallback->setPosition(
          std::round((iconPx - m_fallback->width()) * 0.5F), std::round((iconPx - m_fallback->height()) * 0.5F)
      );
      m_fallback->setVisible(true);
    }

    void bindIcon(Renderer& renderer, const NotificationHistoryEntry& entry, IconResolver& iconResolver) {
      const float iconPx = kHistoryIconSize * m_scale;
      const float iconRadius = notificationIconRadius(iconPx, m_scale);
      m_iconSlot->setRadius(iconRadius);
      m_image->setSize(iconPx, iconPx);
      m_image->setPosition(0.0F, 0.0F);
      m_image->setRadius(iconRadius);
      m_image->setFit(ImageFit::Cover);

      const int targetSize = static_cast<int>(std::round(iconPx));
      if (entry.notification.imageData.has_value()) {
        const auto& image = *entry.notification.imageData;
        if (image.width > 0 && image.height > 0 && !image.data.empty()) {
          const bool validImageMetadata = image.bitsPerSample == 8
              && ((image.channels == 4 && image.hasAlpha) || (image.channels == 3 && !image.hasAlpha));
          const PixmapFormat format = image.channels == 3 ? PixmapFormat::RGB : PixmapFormat::RGBA;
          const std::uint64_t key = rawImageKey(entry);
          bool ready = m_imageKind == ImageKind::Raw && m_rawImageKey == key && m_image->hasImage();
          if (!ready && validImageMetadata) {
            ready = m_image->setSourceRaw(
                renderer, image.data.data(), image.data.size(), image.width, image.height, image.rowStride, format, true
            );
          }
          if (ready) {
            m_imageKind = ImageKind::Raw;
            m_rawImageKey = key;
            m_image->setVisible(true);
            m_fallback->setVisible(false);
            return;
          }
        }
      }

      // Fallback: if no snapshot pixels are available, load from the live icon path.
      // This avoids showing stale HyprCap screenshots when the file gets overwritten later.
      std::string iconPath = resolveNotificationIconPath(entry.notification, iconResolver, targetSize);
      if (iconPath.empty()) {
        iconPath = resolveAppIconPath(entry.notification, iconResolver, targetSize);
      }
      if (!iconPath.empty()) {
        const bool ready = m_image->setSourceFile(renderer, iconPath, targetSize);
        if (ready) {
          m_imageKind = ImageKind::File;
          m_rawImageKey = 0;
          m_image->setVisible(true);
          m_fallback->setVisible(false);
          return;
        }
      }

      showFallbackIcon(renderer);
    }

    float m_scale = 1.0F;
    Flex* m_header = nullptr;
    Flex* m_leftCluster = nullptr;
    Box* m_iconSlot = nullptr;
    Image* m_image = nullptr;
    Glyph* m_fallback = nullptr;
    Label* m_meta = nullptr;
    Flex* m_headerActions = nullptr;
    Button* m_expand = nullptr;
    Label* m_time = nullptr;
    Button* m_dismiss = nullptr;
    Label* m_summary = nullptr;
    Label* m_body = nullptr;
    Flex* m_actionsRow = nullptr;
    ImageKind m_imageKind = ImageKind::None;
    std::uint64_t m_rawImageKey = 0;
  };

  // A list slot: either a card (optionally stacked on plates for a collapsed group) or an
  // expanded group's header with "Show less" and a clear button.
  class NotificationHistoryItem final : public Node {
  public:
    NotificationHistoryItem(float scale, float fillOpacity) : m_scale(scale), m_fillOpacity(fillOpacity) {
      for (auto*& plate : m_plates) {
        plate = static_cast<Box*>(addChild(ui::box({.visible = false})));
      }
      m_card =
          static_cast<NotificationHistoryRow*>(addChild(std::make_unique<NotificationHistoryRow>(scale, fillOpacity)));

      m_header = static_cast<Flex*>(addChild(
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceSm * scale,
                  .visible = false,
              },
              ui::label({
                  .out = &m_headerTitle,
                  .fontSize = Style::fontSizeTitle * scale,
                  .fontWeight = FontWeight::SemiBold,
                  .color = colorSpecFromRole(ColorRole::OnSurface),
                  .maxLines = 1,
                  .flexGrow = 1.0F,
              }),
              ui::button({
                  .out = &m_showLess,
                  .text = i18n::tr("control-center.notifications.show-less"),
                  .fontSize = Style::fontSizeCaption * scale,
                  .variant = ButtonVariant::Default,
                  .minHeight = kGroupHeaderHeight * scale * 0.75F,
                  .paddingH = Style::spaceSm * scale,
                  .radius = kGroupHeaderHeight * scale * 0.375F,
              }),
              ui::button({
                  .out = &m_clearGroup,
                  .glyph = "close",
                  .glyphSize = Style::fontSizeCaption * scale,
                  .variant = ButtonVariant::Default,
                  .minWidth = kGroupHeaderHeight * scale * 0.75F,
                  .minHeight = kGroupHeaderHeight * scale * 0.75F,
                  .padding = 0.0F,
                  .radius = kGroupHeaderHeight * scale * 0.375F,
              })
          )
      ));
    }

    [[nodiscard]] NotificationHistoryRow& card() noexcept { return *m_card; }

    // Call after card().bind(): sits the card on top and peeks `groupSize - 1` plates (at most
    // two, each narrower) out from under its bottom edge.
    void showCard(std::size_t groupSize, float width) {
      m_header->setVisible(false);
      m_card->setVisible(true);
      m_card->setPosition(0.0F, 0.0F);

      const float cardHeight = m_card->height();
      const float peek = kStackPlatePeek * m_scale;
      const std::size_t plates = stackPlateCount(groupSize);
      for (std::size_t i = 0; i < m_plates.size(); ++i) {
        // m_plates is in paint order (farthest first), so plate i counts back from the end.
        Box* plate = m_plates[m_plates.size() - 1 - i];
        plate->setVisible(i < plates);
        if (i >= plates) {
          continue;
        }
        const float inset = kStackPlateInset * m_scale * static_cast<float>(i + 1);
        const float radius = std::min(Style::scaledRadiusXl(m_scale), peek * 1.5F);
        // Each plate tucks under the one above by `radius`, so only a rounded lip shows.
        const float top = cardHeight + peek * static_cast<float>(i) - radius;
        // Same fill as the card: a fainter plate would show the one behind it through its lip.
        plate->setCardStyle(m_scale, m_fillOpacity);
        plate->setRadii(Radii(0.0F, 0.0F, radius, radius));
        plate->setPosition(inset, top);
        plate->setSize(std::max(0.0F, width - inset * 2.0F), radius + peek);
      }
    }

    void showHeader(
        Renderer& renderer, const std::string& title, float width, std::function<void()> onShowLess,
        std::function<void()> onClear
    ) {
      m_card->setVisible(false);
      for (Box* plate : m_plates) {
        plate->setVisible(false);
      }
      m_header->setVisible(true);
      m_headerTitle->setText(title);
      m_showLess->setOnClick(std::move(onShowLess));
      m_clearGroup->setOnClick(std::move(onClear));
      m_clearGroup->setTooltip(i18n::tr("control-center.notifications.clear-group", "app", title));
      m_header->setPosition(0.0F, 0.0F);
      m_header->setSize(width, kGroupHeaderHeight * m_scale);
      m_header->layout(renderer);
    }

  private:
    float m_scale = 1.0F;
    float m_fillOpacity = 1.0F;
    std::array<Box*, kMaxStackPlates> m_plates{};
    NotificationHistoryRow* m_card = nullptr;
    Flex* m_header = nullptr;
    Label* m_headerTitle = nullptr;
    Button* m_showLess = nullptr;
    Button* m_clearGroup = nullptr;
  };

} // namespace

class NotificationHistoryAdapter final : public VirtualListAdapter {
public:
  NotificationHistoryAdapter(NotificationsTab& owner, float scale, float fillOpacity)
      : m_owner(owner), m_scale(scale), m_fillOpacity(fillOpacity) {}

  [[nodiscard]] std::size_t itemCount() const override { return m_owner.m_items.size(); }

  [[nodiscard]] std::uint64_t itemKey(std::size_t index) const override {
    const auto* item = itemAt(index);
    if (item == nullptr) {
      return static_cast<std::uint64_t>(index);
    }
    if (item->kind == NotificationsTab::HistoryItem::Kind::GroupHeader) {
      return std::hash<std::string>{}(item->groupKey) | (1ULL << 63U);
    }
    return item->entry->notification.id;
  }

  [[nodiscard]] std::uint64_t itemRevision(std::size_t index) const override {
    const auto* item = itemAt(index);
    if (item == nullptr) {
      return 0;
    }
    const auto& entry = *item->entry;
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    std::uint64_t revision = revisionForEntry(entry, expanded, m_owner.m_lastRelativeTimeSlot);
    revision ^= static_cast<std::uint64_t>(Style::cornerRadiusScale() * 10000.0F) * 0xC2B2AE3D27D4EB4FULL;
    revision ^= static_cast<std::uint64_t>(item->collapsedStack ? item->groupSize : 0) * 0x94D049BB133111EBULL;
    if (item->kind == NotificationsTab::HistoryItem::Kind::GroupHeader) {
      revision ^= std::hash<std::string>{}(item->groupKey) ^ 0xBF58476D1CE4E5B9ULL;
    }
    return revision;
  }

  // A click fans a collapsed stack out, as on macOS,
  // and opens a lone card's app when it still offers a default action.
  [[nodiscard]] bool itemInteractive(std::size_t index) const override {
    const auto* item = itemAt(index);
    return item != nullptr && item->kind == NotificationsTab::HistoryItem::Kind::Card;
  }

  void onActivate(std::size_t index) override {
    const auto* item = itemAt(index);
    if (item == nullptr) {
      return;
    }
    if (item->collapsedStack) {
      m_owner.setGroupExpanded(item->groupKey, true);
      return;
    }
    const auto& actions = item->entry->notification.actions;
    if (std::ranges::find(actions, std::string("default")) != actions.end()) {
      m_owner.invokeNotificationAction(item->entry->notification.id, "default");
    }
  }

  [[nodiscard]] float measureItem(Renderer& renderer, std::size_t index, float width) override {
    const auto* item = itemAt(index);
    if (item == nullptr) {
      return 1.0F;
    }
    if (item->kind == NotificationsTab::HistoryItem::Kind::GroupHeader) {
      return kGroupHeaderHeight * m_scale;
    }
    const auto& entry = *item->entry;
    const bool expanded = m_owner.m_expandedIds.contains(entry.notification.id);
    const bool showHistoryActions = m_owner.m_notifications != nullptr
        && (entry.active || m_owner.m_notifications->hasPendingDBusClose(entry.notification.id));
    return measureNotificationCard(
               renderer, entry, m_scale, width, expanded, showHistoryActions, item->collapsedStack ? item->groupSize : 1
           )
               .height
        + (item->collapsedStack ? stackPlatesHeight(item->groupSize, m_scale) : 0.0F);
  }

  [[nodiscard]] std::unique_ptr<Node> createItem() override {
    return std::make_unique<NotificationHistoryItem>(m_scale, m_fillOpacity);
  }

  void bindItem(Renderer& renderer, Node& node, std::size_t index, float width, bool /*hovered*/) override {
    const auto* item = itemAt(index);
    auto* slot = dynamic_cast<NotificationHistoryItem*>(&node);
    if (item == nullptr || slot == nullptr) {
      return;
    }
    if (item->kind == NotificationsTab::HistoryItem::Kind::GroupHeader) {
      slot->showHeader(
          renderer, item->groupKey, width, [this, key = item->groupKey]() { m_owner.setGroupExpanded(key, false); },
          [this, key = item->groupKey]() { m_owner.clearGroup(key); }
      );
      return;
    }

    const auto& entry = *item->entry;
    const bool showHistoryActions = m_owner.m_notifications != nullptr
        && (entry.active || m_owner.m_notifications->hasPendingDBusClose(entry.notification.id));
    // A collapsed stack's dismiss clears the whole stack, as its close button does on macOS.
    std::function<void(uint32_t, bool)> onRemove;
    if (item->collapsedStack) {
      onRemove = [this, key = item->groupKey](uint32_t, bool) { m_owner.clearGroup(key); };
    } else {
      onRemove = [this](uint32_t id, bool active) { m_owner.removeNotificationEntry(id, active); };
    }
    slot->card().bind(
        renderer, entry, width, m_owner.m_expandedIds.contains(entry.notification.id), showHistoryActions,
        item->collapsedStack ? item->groupSize : 1, m_owner.m_iconResolver,
        [this](uint32_t id) { m_owner.toggleNotificationExpanded(id); }, std::move(onRemove),
        [this](uint32_t id, const std::string& key) { m_owner.invokeNotificationAction(id, key); },
        [this, key = item->groupKey] { m_owner.setGroupExpanded(key, true); }
    );
    slot->showCard(item->collapsedStack ? item->groupSize : 1, width);
  }

private:
  [[nodiscard]] const NotificationsTab::HistoryItem* itemAt(std::size_t index) const {
    if (index >= m_owner.m_items.size() || m_owner.m_items[index].entry == nullptr) {
      return nullptr;
    }
    return &m_owner.m_items[index];
  }

  NotificationsTab& m_owner;
  float m_scale = 1.0F;
  float m_fillOpacity = 1.0F;
};

NotificationsTab::NotificationsTab(NotificationManager* notifications, CompositorPlatform* platform)
    : m_notifications(notifications), m_platform(platform) {}

NotificationsTab::~NotificationsTab() = default;

float NotificationsTab::fittedHeight() const {
  if (m_list && m_list->visible())
    return m_list->scrollView().content()->height();
  return m_emptyCard ? m_emptyCard->height() : scaled(100.0F);
}

void NotificationsTab::scrollFocusedInputIntoView(InputArea* area) {
  if (!area || !m_list)
    return;
  if (auto* scroll = findEnclosingScrollView(area)) {
    scrollNodeIntoScrollView(*scroll, nullptr, *area, scaled(Style::spaceXs));
    PanelManager::instance().requestLayout();
  }
}

std::unique_ptr<Flex> NotificationsTab::create() {
  const float scale = contentScale();
  auto tab = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });

  m_adapter = std::make_unique<NotificationHistoryAdapter>(*this, scale, panelCardOpacity());

  tab->addChild(
      ui::virtualListView({
          .out = &m_list,
          .contentScale = scale,
          .itemGap = Style::spaceMd * scale,
          .overscanItems = 3,
          .adapter = m_adapter.get(),
          .flexGrow = 1.0F,
          .configure = [](VirtualListView& list) {
            list.setFillWidth(true);
            list.setFillHeight(true);
          },
      })
  );

  tab->addChild(
      ui::column(
          {
              .out = &m_emptyCard,
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * scale,
              .visible = false,
              .configure =
                  [scale, opacity = panelCardOpacity()](Flex& empty) {
                    applyNotificationCardStyle(empty, scale, opacity);
                    empty.setPadding(Style::spaceLg * scale, Style::spaceMd * scale);
                  },
          },
          ui::label({
              .out = &m_emptyTitle,
              .fontSize = Style::fontSizeBody * scale,
              .fontWeight = FontWeight::SemiBold,
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 2,
              .textAlign = TextAlign::Center,
          }),
          ui::label({
              .out = &m_emptyBody,
              .fontSize = Style::fontSizeCaption * scale,
              .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
              .maxLines = 3,
              .textAlign = TextAlign::Center,
          })
      )
  );

  return tab;
}

std::unique_ptr<Flex> NotificationsTab::createHeaderActions() {
  // A quiet capsule alongside the history title and return control.
  const float scale = contentScale();
  return ui::row(
      {
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * scale,
      },
      ui::button({
          .out = &m_clearAllButton,
          .text = i18n::tr("control-center.notifications.clear-all-short"),
          .fontSize = Style::fontSizeCaption * scale,
          .variant = ButtonVariant::Default,
          .tooltip = i18n::tr("control-center.notifications.clear-all"),
          .minHeight = kGroupHeaderHeight * scale * 0.75F,
          .paddingH = Style::spaceSm * scale,
          .radius = kGroupHeaderHeight * scale * 0.375F,
          .onClick = [this]() { clearAllNotifications(); },
      })
  );
}

void NotificationsTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }

  refreshDataSnapshot();
  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
}

void NotificationsTab::doUpdate(Renderer& renderer) {
  if (refreshDataSnapshot() && m_root != nullptr) {
    m_root->layout(renderer);
  }
}

void NotificationsTab::onClose() {
  if (m_list != nullptr) {
    m_list->setAdapter(nullptr);
  }
  m_root = nullptr;
  m_list = nullptr;
  m_emptyCard = nullptr;
  m_emptyTitle = nullptr;
  m_emptyBody = nullptr;
  m_clearAllButton = nullptr;
  m_adapter.reset();
  m_filtered.clear();
  m_items.clear();
  m_expandedIds.clear();
  // Stacks fold back up whenever Notification Center closes, as on macOS.
  m_expandedGroups.clear();
  m_lastSerial = 0;
  m_lastRelativeTimeSlot = -1;
}

void NotificationsTab::clearAllNotifications() {
  if (m_notifications == nullptr) {
    return;
  }

  std::vector<uint32_t> activeIds;
  activeIds.reserve(m_notifications->all().size());
  for (const auto& notification : m_notifications->all()) {
    activeIds.push_back(notification.id);
  }
  for (const uint32_t id : activeIds) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->clearHistory();
  m_expandedIds.clear();
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  m_expandedGroups.clear();
  PanelManager::instance().refresh();
}

void NotificationsTab::removeNotificationEntry(uint32_t id, bool wasActive) {
  if (m_notifications == nullptr) {
    return;
  }

  if (wasActive) {
    (void)m_notifications->close(id, CloseReason::Dismissed);
  }
  m_notifications->removeHistoryEntry(id);
  m_expandedIds.erase(id);
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::setGroupExpanded(const std::string& groupKey, bool expanded) {
  if (expanded) {
    m_expandedGroups.insert(groupKey);
  } else {
    m_expandedGroups.erase(groupKey);
  }
  rebuildItems();
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
    if (auto* animations = m_list->animationManager())
      animations->cancelForOwner(m_list);
    m_list->setOpacity(MotionService::instance().enabled() ? 0.65F : 1.0F);
    Motion::fadeNode(*m_list, 1.0F, Motion::contentMs);
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::clearGroup(const std::string& groupKey) {
  if (m_notifications == nullptr) {
    return;
  }

  std::vector<std::pair<uint32_t, bool>> members;
  for (const auto* entry : m_filtered) {
    if (entry != nullptr && historyGroupKey(entry->notification) == groupKey) {
      members.emplace_back(entry->notification.id, entry->active);
    }
  }
  for (const auto& [id, active] : members) {
    if (active) {
      (void)m_notifications->close(id, CloseReason::Dismissed);
    }
    m_notifications->removeHistoryEntry(id);
    m_expandedIds.erase(id);
  }
  m_expandedGroups.erase(groupKey);
  m_lastSerial = 0;
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::toggleNotificationExpanded(uint32_t id) {
  if (m_expandedIds.contains(id)) {
    m_expandedIds.erase(id);
  } else {
    m_expandedIds.insert(id);
  }

  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

void NotificationsTab::invokeNotificationAction(uint32_t id, const std::string& actionKey) {
  if (m_notifications == nullptr || actionKey.empty()) {
    return;
  }
  const std::string activationToken =
      m_platform != nullptr ? m_platform->requestActivationToken(m_platform->lastPointerSurface()) : std::string{};
  if (!m_notifications->invokeAction(id, actionKey, activationToken, true)) {
    kLog.warn("notification history: failed to invoke action '{}' for #{}", actionKey, id);
    return;
  }

  if (PanelManager::instance().isOpenPanel("notification-center")) {
    PanelManager::instance().close();
    return;
  }

  m_lastSerial = 0;
  if (m_list != nullptr) {
    if (const auto index = filteredIndexForId(id); index.has_value()) {
      m_list->notifyItemChanged(*index);
    } else {
      m_list->notifyDataChanged();
    }
  }
  PanelManager::instance().refresh();
}

bool NotificationsTab::refreshDataSnapshot() {
  const bool hasHistory = m_notifications != nullptr && !m_notifications->history().empty();
  if (m_clearAllButton != nullptr) {
    m_clearAllButton->setVisible(hasHistory);
  }

  const std::uint64_t serial = m_notifications != nullptr ? m_notifications->changeSerial() : 0;
  const std::int64_t relativeSlot = currentRelativeTimeSlot();
  const bool changed = serial != m_lastSerial || relativeSlot != m_lastRelativeTimeSlot;
  if (!changed) {
    updateEmptyState(hasHistory);
    return false;
  }

  m_filtered.clear();
  if (m_notifications != nullptr) {
    m_filtered.reserve(m_notifications->history().size());
    for (const auto& historyEntry : std::views::reverse(m_notifications->history())) {
      m_filtered.push_back(&historyEntry);
    }
  }

  rebuildItems();

  m_lastSerial = serial;
  m_lastRelativeTimeSlot = relativeSlot;

  updateEmptyState(hasHistory);
  if (m_list != nullptr) {
    m_list->notifyDataChanged();
  }
  return true;
}

void NotificationsTab::rebuildItems() {
  // Groups keep the order of their newest notification; members stay newest first.
  std::vector<std::string> order;
  std::unordered_map<std::string, std::vector<const NotificationHistoryEntry*>> groups;
  for (const auto* entry : m_filtered) {
    std::string key = historyGroupKey(entry->notification);
    auto [it, inserted] = groups.try_emplace(key);
    if (inserted) {
      order.push_back(std::move(key));
    }
    it->second.push_back(entry);
  }

  m_items.clear();
  m_items.reserve(m_filtered.size() + order.size());
  for (const auto& key : order) {
    const auto& members = groups[key];
    if (members.size() == 1) {
      m_items.push_back({.entry = members.front(), .groupKey = key});
      continue;
    }
    if (!m_expandedGroups.contains(key)) {
      m_items.push_back(
          {.entry = members.front(), .groupKey = key, .groupSize = members.size(), .collapsedStack = true}
      );
      continue;
    }
    m_items.push_back(
        {.kind = HistoryItem::Kind::GroupHeader, .entry = members.front(), .groupKey = key, .groupSize = members.size()}
    );
    for (const auto* member : members) {
      m_items.push_back({.entry = member, .groupKey = key, .groupSize = members.size()});
    }
  }
}

void NotificationsTab::updateEmptyState(bool hasHistory) {
  if (m_list != nullptr) {
    m_list->setVisible(hasHistory);
  }
  if (m_emptyCard != nullptr) {
    m_emptyCard->setVisible(!hasHistory);
  }
  if (m_emptyTitle != nullptr && m_emptyBody != nullptr) {
    m_emptyTitle->setText(i18n::tr("control-center.notifications.empty-title"));
    m_emptyBody->setText(i18n::tr("control-center.notifications.empty-body"));
  }
}

std::optional<std::size_t> NotificationsTab::filteredIndexForId(uint32_t id) const {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    if (m_items[i].kind == HistoryItem::Kind::Card && m_items[i].entry->notification.id == id) {
      return i;
    }
  }
  return std::nullopt;
}
