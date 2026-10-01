#pragma once
#include "compositors/hyprland/hyprland_displays.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace settings {
  struct DisplayRect {
    float x, y, width, height;
  };
  inline DisplayRect displayRect(const HyprlandDisplayConfig& config, const compositors::hyprland::DisplayInfo& live) {
    int width = 0, height = 0;
    if (config.mode.empty() || std::sscanf(config.mode.c_str(), "%dx%d", &width, &height) != 2)
      std::sscanf(live.mode.c_str(), "%dx%d", &width, &height);
    if ((config.transform < 0 ? live.transform : config.transform) % 2)
      std::swap(width, height);
    const float scale = config.scale > 0 ? config.scale : live.scale;
    return {
        float(config.positionManaged ? config.x : live.x), float(config.positionManaged ? config.y : live.y),
        std::round(float(width) / scale), std::round(float(height) / scale)
    };
  }
  inline bool overlaps(DisplayRect a, DisplayRect b) {
    return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
  }
  // Snap both adjacent edges and aligned edges, but only near a neighbouring monitor.
  inline DisplayRect snapDisplay(DisplayRect moving, const std::vector<DisplayRect>& others, float distance) {
    float dx = distance + 1, dy = distance + 1;
    for (const auto& other : others) {
      if (moving.y < other.y + other.height + distance && moving.y + moving.height > other.y - distance)
        for (float delta :
             {other.x - moving.x, other.x + other.width - moving.x, other.x - moving.width - moving.x,
              other.x + other.width - moving.width - moving.x})
          if (std::abs(delta) < std::abs(dx))
            dx = delta;
      if (moving.x < other.x + other.width + distance && moving.x + moving.width > other.x - distance)
        for (float delta :
             {other.y - moving.y, other.y + other.height - moving.y, other.y - moving.height - moving.y,
              other.y + other.height - moving.height - moving.y})
          if (std::abs(delta) < std::abs(dy))
            dy = delta;
    }
    if (std::abs(dx) <= distance)
      moving.x += dx;
    if (std::abs(dy) <= distance)
      moving.y += dy;
    moving.x = std::round(std::clamp(moving.x, -32768.F, 32768.F));
    moving.y = std::round(std::clamp(moving.y, -32768.F, 32768.F));
    return moving;
  }
} // namespace settings
