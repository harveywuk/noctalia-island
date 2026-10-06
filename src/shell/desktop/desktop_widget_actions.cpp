#include "shell/desktop/desktop_widget_actions.h"

#include "shell/desktop/desktop_card_layout.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "util/string_utils.h"

#include <algorithm>
#include <unordered_map>

namespace desktop_widgets {
  bool applyQuickAction(DesktopWidgetsConfig& snapshot, const std::string& id, QuickAction action) {
    auto it = std::ranges::find(snapshot.widgets, id, &DesktopWidgetState::id);
    if (it == snapshot.widgets.end())
      return false;
    if (action == QuickAction::Small || action == QuickAction::Medium || action == QuickAction::Large) {
      if (!desktop_cards::supportsSizePresets(it->type))
        return false;
      const std::string size = action == QuickAction::Small ? "small"
          : action == QuickAction::Medium                   ? "medium"
                                                            : "large";
      const auto previous = *it;
      it->settings["card_size"] = size;
      it->boxWidth = it->boxHeight = 0;
      return previous != *it;
    }
    if (action == QuickAction::SmartRotate && it->type == "stack") {
      const auto value = it->settings.find("smart_rotate");
      const auto* enabled = value == it->settings.end() ? nullptr : std::get_if<bool>(&value->second);
      it->settings["smart_rotate"] = !(enabled && *enabled);
      return true;
    }
    if (action == QuickAction::Remove) {
      // Removing the container releases its cards at their saved positions.
      snapshot.widgets.erase(it);
      return true;
    }
    if (action != QuickAction::Duplicate)
      return false;
    std::vector<DesktopWidgetState> copies{*it};
    if (it->type == "stack") {
      const auto cards = desktop_stacks::cards(snapshot.widgets, id);
      copies.insert(copies.end(), cards.begin(), cards.end());
    }
    std::unordered_map<std::string, std::string> ids;
    const float offset =
        snapshot.grid.visible && snapshot.grid.cellSize > 0 ? static_cast<float>(snapshot.grid.cellSize) : 24.0F;
    for (auto& copy : copies) {
      const auto uuid = StringUtils::generateUuid();
      if (uuid.empty())
        return false;
      const auto newId = "desktop-widget-" + uuid;
      ids[copy.id] = newId;
      copy.id = newId;
      copy.cx += offset;
      copy.cy += offset;
    }
    for (auto& copy : copies) {
      if (copy.type == "stack") {
        auto members = desktop_stacks::members(copy);
        std::erase_if(members, [&](const auto& member) { return !ids.contains(member); });
        for (auto& member : members)
          member = ids.at(member);
        copy.settings["members"] = members;
      }
      snapshot.widgets.push_back(std::move(copy));
    }
    return true;
  }
} // namespace desktop_widgets
