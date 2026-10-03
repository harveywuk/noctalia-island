#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <utility>

class SessionBus;

// Unread counts that applications publish through com.canonical.Unity.LauncherEntry
// ("count" and "count-visible"), as shown by the red badges on macOS dock icons.
class LauncherBadgeService {
public:
  explicit LauncherBadgeService(SessionBus& bus);

  // Visible count for a desktop entry id, matched case-insensitively with or without
  // ".desktop"; 0 when the application shows no badge.
  [[nodiscard]] std::int64_t count(std::string_view desktopId) const;
  std::function<void()> changed;

private:
  struct Entry {
    std::int64_t count = 0;
    bool visible = false;
  };
  // Keyed by (sender, normalized desktop id) so one application cannot clear another's badge.
  std::map<std::pair<std::string, std::string>, Entry> m_entries;
  sdbus::Slot m_updateSlot;
  sdbus::Slot m_ownerSlot;
};
