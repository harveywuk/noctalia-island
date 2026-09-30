#pragma once

#include "config/config_types.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Flex;

namespace settings {
  class SettingsControlFactory;
  inline constexpr std::array<std::string_view, 3> kHoverWidgetKeys{
      "hover_widgets", "hover_widgets_center", "hover_widgets_right"
  };
  inline constexpr std::array<std::string_view, 7> kHoverSectionKeys{"hover_show_clock",  "hover_show_calendar",
                                                                     "hover_show_media",  "hover_show_downloads",
                                                                     "hover_show_timers", "hover_show_batteries",
                                                                     "hover_show_unread"};
  using HoverWidgetGroups = std::array<std::vector<std::string>, 3>;
  struct HoverLayout {
    HoverWidgetGroups groups;
    std::array<bool, 7> sections;
    bool operator==(const HoverLayout&) const = default;
  };

  inline HoverLayout hoverLayout(const IslandConfig& cfg) {
    return {
        {cfg.hoverWidgets, cfg.hoverWidgetsCenter, cfg.hoverWidgetsRight},
        {cfg.hoverShowClock, cfg.hoverShowCalendar, cfg.hoverShowMedia, cfg.hoverShowDownloads, cfg.hoverShowTimers,
         cfg.hoverShowBatteries, cfg.hoverShowUnread}
    };
  }

  inline std::optional<std::size_t> hoverWidgetGroup(const std::vector<std::string>& path) {
    if (path.size() != 2 || path[0] != "island")
      return std::nullopt;
    for (std::size_t i = 0; i < kHoverWidgetKeys.size(); ++i)
      if (path[1] == kHoverWidgetKeys[i])
        return i;
    return std::nullopt;
  }

  // The insertion position is measured before removing the source. Address by
  // index so repeated references move independently and retain their order.
  inline bool moveHoverWidget(
      HoverWidgetGroups& groups, std::size_t source, std::size_t index, std::size_t target, std::size_t insertion
  ) {
    if (source >= groups.size()
        || target >= groups.size()
        || index >= groups[source].size()
        || insertion > groups[target].size())
      return false;
    if (source == target && (insertion == index || insertion == index + 1))
      return false;
    auto name = groups[source][index];
    groups[source].erase(groups[source].begin() + static_cast<std::ptrdiff_t>(index));
    if (source == target && insertion > index)
      --insertion;
    groups[target].insert(groups[target].begin() + static_cast<std::ptrdiff_t>(insertion), std::move(name));
    return true;
  }

  inline HoverLayout hoverLayoutPreset(std::size_t preset) {
    HoverLayout layout{{}, {false, false, false, false, false, false, false}};
    if (preset == 0) { // Minimal
      layout.groups[1] = {"clock"};
    } else if (preset == 1) { // Media
      layout.groups[0] = {"volume"};
      layout.groups[1] = {"media"};
      layout.groups[2] = {"audio_visualizer"};
      layout.sections[2] = true;
    } else { // System Monitor
      layout.groups[0] = {"sysmon"};
      layout.groups[1] = {"network"};
      layout.groups[2] = {"battery"};
      layout.sections[3] = true;
      layout.sections[5] = true;
    }
    return layout;
  }

  inline std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
  hoverLayoutOverrides(const HoverLayout& layout) {
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> result;
    for (std::size_t i = 0; i < layout.groups.size(); ++i)
      result.push_back({{"island", std::string(kHoverWidgetKeys[i])}, layout.groups[i]});
    for (std::size_t i = 0; i < layout.sections.size(); ++i)
      result.push_back({{"island", std::string(kHoverSectionKeys[i])}, layout.sections[i]});
    return result;
  }

  void addIslandWidgetEditor(Flex& section, SettingsControlFactory& factory);
} // namespace settings
