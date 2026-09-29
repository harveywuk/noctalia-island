#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace island {
  struct HoverWidgetSize {
    float width = 0;
    float height = 0;
    std::size_t group = 0; // left, centre, right
  };

  struct HoverWidgetRect {
    float x = 0, y = 0, width = 0, height = 0;
  };

  struct HoverWidgetLayout {
    std::vector<HoverWidgetRect> items;
    float height = 0;
  };

  // Groups share a row when they fit. Otherwise stack them, preserving each
  // group's alignment and wrapping its widgets without overlap or truncating rows.
  inline HoverWidgetLayout layoutHoverWidgets(const std::vector<HoverWidgetSize>& items, float width, float gap) {
    HoverWidgetLayout result{.items = std::vector<HoverWidgetRect>(items.size())};
    width = std::max(0.0F, width);
    if (width == 0)
      return result;
    struct Row {
      std::vector<std::size_t> items;
      float width = 0, height = 0;
    };
    struct Group {
      std::vector<Row> rows;
      float width = 0, height = 0, x = 0, y = 0;
    };
    std::array<Group, 3> groups;
    for (std::size_t i = 0; i < items.size(); ++i) {
      const auto& item = items[i];
      if (item.width <= 0 || item.height <= 0 || item.group >= groups.size())
        continue;
      auto& group = groups[item.group];
      const float itemWidth = std::min(width, item.width);
      if (group.rows.empty() || group.rows.back().width + gap + itemWidth > width)
        group.rows.emplace_back();
      auto& row = group.rows.back();
      if (!row.items.empty())
        row.width += gap;
      row.items.push_back(i);
      row.width += itemWidth;
      row.height = std::max(row.height, item.height);
      result.items[i].width = itemWidth;
      result.items[i].height = item.height;
    }
    float totalWidth = 0;
    unsigned activeGroups = 0;
    for (auto& group : groups) {
      for (const auto& row : group.rows) {
        group.width = std::max(group.width, row.width);
        if (group.height > 0)
          group.height += gap;
        group.height += row.height;
      }
      if (group.rows.empty())
        continue;
      totalWidth += group.width;
      if (activeGroups++)
        totalWidth += gap;
    }
    if (totalWidth <= width) {
      groups[2].x = width - groups[2].width;
      const float minCenter = groups[0].rows.empty() ? 0 : groups[0].width + gap;
      const float maxCenter = width - groups[1].width - (groups[2].rows.empty() ? 0 : groups[2].width + gap);
      if (!groups[1].rows.empty())
        groups[1].x = std::clamp((width - groups[1].width) / 2, minCenter, std::max(minCenter, maxCenter));
      for (const auto& group : groups)
        result.height = std::max(result.height, group.height);
    } else {
      for (std::size_t i = 0; i < groups.size(); ++i) {
        auto& group = groups[i];
        if (group.rows.empty())
          continue;
        if (result.height > 0)
          result.height += gap;
        group.x = (width - group.width) * static_cast<float>(i) / 2;
        group.y = result.height;
        result.height += group.height;
      }
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const auto& group = groups[i];
      float y = group.y;
      for (const auto& row : group.rows) {
        float x = group.x + (group.width - row.width) * static_cast<float>(i) / 2;
        for (const auto index : row.items) {
          auto& rect = result.items[index];
          rect.x = x;
          rect.y = y + (row.height - rect.height) / 2;
          x += rect.width + gap;
        }
        y += row.height + gap;
      }
    }
    return result;
  }
} // namespace island
