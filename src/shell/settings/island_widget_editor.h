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
    std::vector<std::string> scope;
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
    const bool islandPath = (path.size() == 2 && path[0] == "island")
        || (path.size() == 4 && path[0] == "bar" && path[2] == "island")
        || (path.size() == 6 && path[0] == "bar" && path[2] == "monitor" && path[4] == "island");
    if (!islandPath)
      return std::nullopt;
    for (std::size_t i = 0; i < kHoverWidgetKeys.size(); ++i)
      if (path.back() == kHoverWidgetKeys[i])
        return i;
    return std::nullopt;
  }

  inline IslandConfig islandConfigForPath(const Config& cfg, const std::vector<std::string>& path) {
    if (path.size() >= 3 && path[0] == "bar") {
      const auto bar = std::ranges::find(cfg.bars, path[1], &BarConfig::name);
      if (bar != cfg.bars.end()) {
        if (path.size() >= 5 && path[2] == "monitor") {
          const auto monitor = std::ranges::find(bar->monitorOverrides, path[3], &BarMonitorOverride::match);
          if (monitor != bar->monitorOverrides.end())
            return applyIslandOverride(bar->island, monitor->island);
        }
        return bar->island;
      }
    }
    return cfg.island;
  }

  inline std::vector<std::string> islandPath(std::vector<std::string> root, std::string_view key) {
    root.emplace_back(key);
    return root;
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
  hoverLayoutOverrides(const HoverLayout& layout, const std::vector<std::string>& root = {"island"}) {
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> result;
    for (std::size_t i = 0; i < layout.groups.size(); ++i)
      result.push_back({islandPath(root, kHoverWidgetKeys[i]), layout.groups[i]});
    for (std::size_t i = 0; i < layout.sections.size(); ++i)
      result.push_back({islandPath(root, kHoverSectionKeys[i]), layout.sections[i]});
    return result;
  }

  void addIslandWidgetEditor(Flex& section, SettingsControlFactory& factory, const std::vector<std::string>& root);
} // namespace settings
