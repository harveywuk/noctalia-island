#include "dbus/launcher/launcher_badge_service.h"

#include "dbus/session_bus.h"
#include "util/string_utils.h"

#include <algorithm>

namespace {

  constexpr std::size_t kMaxEntries = 128;

  [[nodiscard]] std::string normalizedDesktopId(std::string_view id) {
    constexpr std::string_view suffix = ".desktop";
    if (id.ends_with(suffix)) {
      id.remove_suffix(suffix.size());
    }
    return StringUtils::toLower(std::string(id));
  }

} // namespace

LauncherBadgeService::LauncherBadgeService(SessionBus& bus) {
  m_updateSlot = bus.connection().addMatch(
      "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'",
      [this](sdbus::Message message) {
        try {
          std::string uri;
          std::map<std::string, sdbus::Variant> properties;
          message >> uri >> properties;
          constexpr std::string_view prefix = "application://";
          if (!uri.starts_with(prefix)) {
            return;
          }
          const std::string_view id = std::string_view(uri).substr(prefix.size());
          if (id.empty() || id.size() > 256 || id.find('/') != std::string_view::npos) {
            return;
          }
          const bool hasCount = properties.contains("count") || properties.contains("count-visible");
          const auto key = std::pair{std::string(message.getSender()), normalizedDesktopId(id)};
          const auto existing = m_entries.find(key);
          if (!hasCount || (existing == m_entries.end() && m_entries.size() >= kMaxEntries)) {
            return;
          }
          Entry entry = existing != m_entries.end() ? existing->second : Entry{};
          if (const auto it = properties.find("count"); it != properties.end()) {
            entry.count = std::max<std::int64_t>(0, it->second.get<std::int64_t>());
          }
          if (const auto it = properties.find("count-visible"); it != properties.end()) {
            entry.visible = it->second.get<bool>();
          }
          if (existing != m_entries.end()
              && existing->second.count == entry.count
              && existing->second.visible == entry.visible) {
            return;
          }
          m_entries[key] = entry;
          if (changed) {
            changed();
          }
        } catch (const sdbus::Error&) {
          // A malformed update must not affect another application's badge.
        }
      },
      sdbus::return_slot
  );
  m_ownerSlot = bus.connection().addMatch(
      "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
      [this](sdbus::Message message) {
        std::string name;
        std::string oldOwner;
        std::string newOwner;
        message >> name >> oldOwner >> newOwner;
        if (!name.starts_with(':') || !newOwner.empty()) {
          return;
        }
        if (std::erase_if(m_entries, [&](const auto& pair) { return pair.first.first == name; }) > 0 && changed) {
          changed();
        }
      },
      sdbus::return_slot
  );
}

std::int64_t LauncherBadgeService::count(std::string_view desktopId) const {
  const std::string id = normalizedDesktopId(desktopId);
  std::int64_t total = 0;
  for (const auto& [key, entry] : m_entries) {
    if (entry.visible && key.second == id) {
      total += entry.count;
    }
  }
  return total;
}
