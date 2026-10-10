#include "shell/dock/pinned_apps.h"

#include "core/log.h"
#include "system/internal_app_metadata.h"

#include <algorithm>

namespace shell::dock::pinned_apps {
  namespace {

    constexpr Logger kLog("dock");

    [[nodiscard]] DesktopEntry placeholderEntry(std::string_view pinnedId) {
      DesktopEntry placeholder;
      placeholder.id = std::string(pinnedId);
      placeholder.name = std::string(pinnedId);
      placeholder.nameLower = std::string(pinnedId);
      return placeholder;
    }

  } // namespace

  namespace {

    // Desktop entry ids are file stems ("firefox"); a pin written as "firefox.desktop" means the same file.
    [[nodiscard]] std::string_view stripDesktopSuffix(std::string_view id) {
      constexpr std::string_view kSuffix = ".desktop";
      if (id.size() > kSuffix.size() && id.ends_with(kSuffix)) {
        id.remove_suffix(kSuffix.size());
      }
      return id;
    }

  } // namespace

  bool matchesEntry(const DesktopEntry& entry, std::string_view pinnedId) {
    if (pinnedId.empty()) {
      return false;
    }
    if (entry.id == pinnedId || (!entry.path.empty() && entry.path == pinnedId)) {
      return true;
    }
    return stripDesktopSuffix(entry.id) == stripDesktopSuffix(pinnedId);
  }

  bool containsEntry(const std::vector<std::string>& pinned, const DesktopEntry& entry) {
    return std::ranges::any_of(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  void removeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry) {
    std::erase_if(pinned, [&](const std::string& pinnedId) { return matchesEntry(entry, pinnedId); });
  }

  void placeEntry(std::vector<std::string>& pinned, const DesktopEntry& entry, const DesktopEntry* before) {
    if (entry.id.empty() || (before && matchesEntry(entry, before->id)))
      return;
    const auto existing = std::ranges::find_if(pinned, [&](const auto& id) { return matchesEntry(entry, id); });
    const std::string id = existing == pinned.end() ? entry.id : *existing;
    removeEntry(pinned, entry);
    const auto target = before
        ? std::ranges::find_if(pinned, [&](const auto& value) { return matchesEntry(*before, value); })
        : pinned.end();
    pinned.insert(target, id);
  }

  std::vector<DesktopEntry> resolveEntries(const std::vector<std::string>& pinned) {
    std::vector<DesktopEntry> resolved;
    resolved.reserve(pinned.size());

    const auto& entries = desktopEntries();
    for (const auto& pinnedId : pinned) {
      const auto match = std::ranges::find_if(entries, [&](const DesktopEntry& entry) {
        return !entry.hidden && !entry.noDisplay && matchesEntry(entry, pinnedId);
      });

      DesktopEntry entry = match != entries.end() ? *match : [&]() {
        kLog.debug("pinned app not found: {}", pinnedId);
        return placeholderEntry(pinnedId);
      }();
      internal_apps::applyMetadataToDesktopEntry(entry);
      resolved.push_back(std::move(entry));
    }

    return resolved;
  }

} // namespace shell::dock::pinned_apps
