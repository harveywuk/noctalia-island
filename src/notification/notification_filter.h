#pragma once

#include "config/config_types.h"
#include "notification.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct NotificationFilterFields {
  std::string_view appName;
  std::optional<std::string_view> category;
  std::optional<std::string_view> desktopEntry;
  std::string_view summary;
  std::string_view body;
};

struct ResolvedNotificationFilter {
  bool showToast = true;
  bool saveHistory = true;
  bool playSound = true;
  bool bypassDnd = false;
  bool allowPermanent = true;
  std::optional<std::int32_t> overrideDuration;
  /// Empty = all urgencies allowed for this filter.
  std::unordered_set<Urgency> allowedUrgencies;
  bool matched = false;
};

enum class NotificationDelivery : std::uint8_t { Normal, Silent, HistoryOnly, Hidden, Custom };

[[nodiscard]] inline NotificationDelivery notificationDelivery(const NotificationFilterConfig& filter) {
  if (filter.showToast && filter.saveHistory)
    return filter.playSound ? NotificationDelivery::Normal : NotificationDelivery::Silent;
  if (!filter.showToast && !filter.playSound)
    return filter.saveHistory ? NotificationDelivery::HistoryOnly : NotificationDelivery::Hidden;
  return NotificationDelivery::Custom;
}

inline void setNotificationDelivery(NotificationFilterConfig& filter, NotificationDelivery delivery) {
  if (delivery == NotificationDelivery::Custom)
    return;
  filter.showToast = delivery == NotificationDelivery::Normal || delivery == NotificationDelivery::Silent;
  filter.saveHistory = delivery != NotificationDelivery::Hidden;
  filter.playSound = delivery == NotificationDelivery::Normal;
}

[[nodiscard]] std::string normalizeNotificationMatchToken(std::string token);

[[nodiscard]] bool notificationMatchesToken(std::string_view token, const NotificationFilterFields& fields);

[[nodiscard]] std::vector<std::string> normalizeNotificationMatchTokens(std::vector<std::string> tokens);

[[nodiscard]] bool
notificationMatchesTokens(const std::vector<std::string>& tokens, const NotificationFilterFields& fields);

[[nodiscard]] std::vector<NotificationFilterConfig>
normalizeNotificationFilters(std::vector<NotificationFilterConfig> filters);

[[nodiscard]] ResolvedNotificationFilter
resolveNotificationFilter(const std::vector<NotificationFilterConfig>& filters, const NotificationFilterFields& fields);

void normalizeNotificationFilterNames(std::vector<NotificationFilterConfig>& filters);

/// Empty result means all urgencies are allowed.
[[nodiscard]] std::unordered_set<Urgency> normalizeAllowedUrgencies(std::vector<std::string> values);

/// Canonical config form: empty vector when all three levels are allowed.
[[nodiscard]] std::vector<std::string> normalizeFilterAllowedUrgencyStrings(std::vector<std::string> values);

[[nodiscard]] bool urgencyIsAllowed(const std::unordered_set<Urgency>& allowed, Urgency urgency) noexcept;

// Deprecated aliases kept for existing call sites/tests.
[[nodiscard]] inline std::vector<std::string> normalizeNotificationBlacklist(std::vector<std::string> blacklist) {
  return normalizeNotificationMatchTokens(std::move(blacklist));
}

[[nodiscard]] inline bool
notificationMatchesBlacklist(const std::vector<std::string>& blacklist, const NotificationFilterFields& fields) {
  return notificationMatchesTokens(blacklist, fields);
}
