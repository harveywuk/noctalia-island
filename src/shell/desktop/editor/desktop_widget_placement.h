#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace desktop_placement {

  struct Rect {
    float x, y, width, height;
    float right() const { return x + width; }
    float bottom() const { return y + height; }
  };

  struct AxisSnap {
    float offset = 0.0F;
    std::optional<float> guide;
  };

  struct Snap {
    AxisSnap x, y;
  };

  inline AxisSnap snapAxis(
      float start, float extent, float crossStart, float crossExtent, std::span<const Rect> neighbours, bool horizontal,
      float outputExtent, float cell, float gap, float threshold
  ) {
    AxisSnap result;
    float best = std::numeric_limits<float>::max();
    auto consider = [&](float from, float to) {
      const float distance = std::abs(to - from);
      if (distance <= threshold && distance < best) {
        best = distance;
        result = {to - from, to};
      }
    };
    const float end = start + extent;
    for (const auto& other : neighbours) {
      const float otherStart = horizontal ? other.x : other.y;
      const float otherExtent = horizontal ? other.width : other.height;
      const float otherCross = horizontal ? other.y : other.x;
      const float otherCrossExtent = horizontal ? other.height : other.width;
      // Gap targets only apply to neighbours alongside this widget, not distant rows or columns.
      if (crossStart <= otherCross + otherCrossExtent + threshold
          && crossStart + crossExtent >= otherCross - threshold) {
        consider(start, otherStart + otherExtent + gap);
        consider(end, otherStart - gap);
      }
      consider(start, otherStart);
      consider(end, otherStart + otherExtent);
      consider(start + extent * 0.5F, otherStart + otherExtent * 0.5F);
    }
    consider(start, gap);
    consider(end, outputExtent - gap);
    consider(start, 0.0F);
    consider(end, outputExtent);
    consider(start + extent * 0.5F, outputExtent * 0.5F);
    if (result.guide || cell <= 0.0F) {
      return result;
    }
    // Alignment and spacing guides take precedence over the background grid.
    for (const float line : std::array{start, start + extent * 0.5F, end}) {
      const float origin = outputExtent * 0.5F;
      const float offset = origin + std::round((line - origin) / cell) * cell - line;
      if (std::abs(offset) < best) {
        best = std::abs(offset);
        result.offset = offset;
      }
    }
    return result;
  }

  inline Snap snap(
      Rect moving, std::span<const Rect> neighbours, float outputWidth, float outputHeight, float cell, float gap,
      float threshold
  ) {
    return {
        snapAxis(moving.x, moving.width, moving.y, moving.height, neighbours, true, outputWidth, cell, gap, threshold),
        snapAxis(
            moving.y, moving.height, moving.x, moving.width, neighbours, false, outputHeight, cell, gap, threshold
        ),
    };
  }

  inline bool overlapsWithGap(Rect a, Rect b, float gap) {
    return a.x < b.right() + gap && a.right() + gap > b.x && a.y < b.bottom() + gap && a.bottom() + gap > b.y;
  }

  inline std::pair<float, float> findSpace(
      float width, float height, std::span<const Rect> occupied, float outputWidth, float outputHeight, float gap,
      float cell
  ) {
    const float step = std::max(1.0F, cell);
    for (float y = gap; y + height <= outputHeight - gap; y += step) {
      for (float x = gap; x + width <= outputWidth - gap; x += step) {
        const Rect candidate{x, y, width, height};
        if (std::none_of(occupied.begin(), occupied.end(), [&](Rect other) {
              return overlapsWithGap(candidate, other, gap);
            })) {
          return {x + width * 0.5F, y + height * 0.5F};
        }
      }
    }
    return {outputWidth * 0.5F, outputHeight * 0.5F};
  }

} // namespace desktop_placement
