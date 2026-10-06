#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tray {
  inline constexpr std::string_view kIslandOverflowContext = "island-overflow:";

  struct OverflowItem {
    std::string id;
    bool pinned = false;
  };

  // Preserve surviving apps within each priority group instead of shuffling them
  // whenever a new service sorts ahead of an existing one on D-Bus.
  inline std::vector<std::string> selectInlineItems(
      const std::vector<OverflowItem>& items, const std::vector<std::string>& previous, std::size_t limit,
      bool drawerOpen = false
  ) {
    std::vector<std::string> selected;
    const auto append = [&](const std::string& id) {
      if (selected.size() < limit && !std::ranges::contains(selected, id))
        selected.push_back(id);
    };
    if (drawerOpen) {
      // The open drawer excludes these apps. Do not promote an app out of that
      // drawer until it closes, even if a visible app exits in the meantime.
      for (const auto& id : previous)
        if (std::ranges::any_of(items, [&](const auto& item) { return item.id == id; }))
          append(id);
      return selected;
    }
    for (const bool pinned : {true, false}) {
      for (const auto& id : previous)
        if (std::ranges::any_of(items, [&](const auto& item) { return item.id == id && item.pinned == pinned; }))
          append(id);
      for (const auto& item : items)
        if (item.pinned == pinned)
          append(item.id);
    }
    return selected;
  }
} // namespace tray
