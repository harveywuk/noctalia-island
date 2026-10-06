#pragma once

#include "shell/control_center/tab.h"
#include "system/icon_resolver.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class NotificationManager;
class CompositorPlatform;
struct NotificationHistoryEntry;
class Button;
class VirtualListView;
class Label;
class InputArea;
class NotificationHistoryAdapter;

class NotificationsTab : public Tab {
public:
  NotificationsTab(NotificationManager* notifications, CompositorPlatform* platform);
  ~NotificationsTab() override;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onClose() override;
  [[nodiscard]] float fittedHeight() const;
  void scrollFocusedInputIntoView(InputArea* area);

private:
  // One row of the history list. Notifications from the same app share a group: a lone one is a
  // plain card, a collapsed group is its newest card stacked on plates (macOS), and an expanded
  // group is a header row followed by each of its cards.
  struct HistoryItem {
    enum class Kind : std::uint8_t { Card, GroupHeader };
    Kind kind = Kind::Card;
    const NotificationHistoryEntry* entry = nullptr; // the card's notification (a header: the group's newest)
    std::string groupKey;
    std::size_t groupSize = 1;
    bool collapsedStack = false;
  };

  friend class NotificationHistoryAdapter;

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void clearAllNotifications();
  void removeNotificationEntry(uint32_t id, bool wasActive);
  void toggleNotificationExpanded(uint32_t id);
  void setGroupExpanded(const std::string& groupKey, bool expanded);
  void clearGroup(const std::string& groupKey);
  void rebuildItems();
  void invokeNotificationAction(uint32_t id, const std::string& actionKey);
  bool refreshDataSnapshot();
  void updateEmptyState(bool hasHistory);
  std::optional<std::size_t> filteredIndexForId(uint32_t id) const;

  NotificationManager* m_notifications = nullptr;
  CompositorPlatform* m_platform = nullptr;
  IconResolver m_iconResolver;
  std::unique_ptr<NotificationHistoryAdapter> m_adapter;
  std::vector<const NotificationHistoryEntry*> m_filtered; // all history, newest first
  std::vector<HistoryItem> m_items;
  std::unordered_set<std::string> m_expandedGroups;
  Flex* m_root = nullptr;
  VirtualListView* m_list = nullptr;
  Flex* m_emptyCard = nullptr;
  Label* m_emptyTitle = nullptr;
  Label* m_emptyBody = nullptr;
  Button* m_clearAllButton = nullptr;
  std::unordered_set<uint32_t> m_expandedIds;
  std::uint64_t m_lastSerial = 0;
  /// Wall-clock coarse slot so relative times (e.g. "2 min ago") refresh without churning every frame.
  std::int64_t m_lastRelativeTimeSlot = -1;
};
