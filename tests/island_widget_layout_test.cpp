#include "shell/island/island_widget_layout.h"

#include <cassert>
#include <cmath>

int main() {
  using island::HoverWidgetSize;
  using island::layoutHoverWidgets;
  const auto check = [](const std::vector<HoverWidgetSize>& sizes, float width, float gap) {
    auto result = layoutHoverWidgets(sizes, width, gap);
    for (std::size_t i = 0; i < result.items.size(); ++i) {
      const auto& a = result.items[i];
      assert(std::isfinite(a.x) && std::isfinite(a.y));
      assert(a.x >= 0 && a.y >= 0 && a.x + a.width <= width && a.y + a.height <= result.height);
      for (std::size_t j = i + 1; j < result.items.size(); ++j) {
        const auto& b = result.items[j];
        if (a.width == 0 || b.width == 0) continue;
        assert(a.x + a.width <= b.x || b.x + b.width <= a.x || a.y + a.height <= b.y || b.y + b.height <= a.y);
      }
    }
    return result;
  };
  auto row = check({{40, 36, 0}, {50, 36, 1}, {60, 36, 2}}, 300, 8);
  assert(row.height == 36);
  assert(row.items[0].x == 0 && row.items[1].x == 125 && row.items[2].x == 240);
  // A wide left group nudges the middle group while keeping it between its neighbours.
  row = check({{160, 36, 0}, {50, 36, 1}, {60, 36, 2}}, 300, 8);
  assert(row.items[1].x == 168 && row.height == 36);
  // Crowded groups get independent rows, aligned to the same edges and centre.
  row = check({{160, 36, 0}, {90, 36, 1}, {90, 36, 2}}, 300, 8);
  assert(row.height == 124);
  assert(row.items[1].x == 105 && row.items[1].y == 44);
  assert(row.items[2].x == 210 && row.items[2].y == 88);
  row = check({{200, 36, 1}, {200, 36, 1}, {30, 36, 1}}, 300, 8);
  assert(row.height == 80 && row.items[0].x == 50 && row.items[1].x == 31);
  row = check({{220, 36, 2}, {120, 48, 2}}, 300, 8);
  assert(row.height == 92 && row.items[0].x == 80 && row.items[1].x == 180);
  // Invisible widgets occupy no space; oversize widgets are clipped to the available width.
  row = check({{0, 36, 0}, {0, 36, 1}, {400, 36, 2}}, 300, 8);
  assert(row.height == 36 && row.items[2].width == 300);
  assert(check({}, 300, 8).height == 0);
  assert(check({{50, 36, 0}}, 0, 8).height == 0);
  // Exercise narrow outputs, scaling and different visible group combinations.
  for (float scale : {0.75F, 1.0F, 1.5F, 2.0F})
    for (float width : {100.0F, 220.0F, 304.0F, 500.0F})
      for (unsigned mask = 0; mask < 8; ++mask) {
        std::vector<HoverWidgetSize> sizes;
        for (std::size_t group = 0; group < 3; ++group)
          if (mask & (1U << group))
            for (float size : {40.0F, 130.0F, 28.0F, 80.0F})
              sizes.push_back({size * scale, 36 * scale, group});
        check(sizes, width * scale, 8 * scale);
      }
}
