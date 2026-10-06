#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/desktop/editor/desktop_widget_placement.h"

#include <cmath>
#include <cstdlib>
#include <print>
#include <vector>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "desktop_widget_placement: {}", message);
      std::exit(1);
    }
  }
  bool near(float a, float b) { return std::abs(a - b) < 0.01F; }
} // namespace

int main() {
  using namespace desktop_placement;
  const std::vector<Rect> neighbours{{16.0F, 16.0F, 208.0F, 208.0F}};
  auto result = snap({244.0F, 19.0F, 208.0F, 208.0F}, neighbours, 1280, 1024, 16, 16, 8);
  expect(near(result.x.offset, -4) && near(result.y.offset, -3), "adjacent cards align with a 16px gap");
  expect(result.x.guide && near(*result.x.guide, 240), "spacing has a visible guide");
  expect(result.y.guide && near(*result.y.guide, 16), "alignment has a visible guide");
  result = snap({18.0F, 244.0F, 432.0F, 208.0F}, neighbours, 1280, 1024, 16, 16, 8);
  expect(near(result.x.offset, -2) && near(result.y.offset, -4), "wide cards align below square cards");

  // The moving left edge already lies on the grid. Its right edge must still align with the neighbour.
  const std::vector<Rect> alignment{{50.0F, 300.0F, 208.0F, 208.0F}};
  result = snap({48.0F, 600.0F, 208.0F, 208.0F}, alignment, 1280, 1024, 16, 16, 8);
  expect(near(result.x.offset, 2), "nearby alignment takes precedence over a coincident grid line");
  result = snap({701.0F, 701.0F, 208.0F, 208.0F}, {}, 1280, 1024, 16, 16, 8);
  expect(near(result.x.offset, 3) && !result.x.guide, "unrelated widgets fall back to the grid");
  result = snap({537.0F, 200.0F, 208.0F, 208.0F}, {}, 1280, 1024, 16, 16, 8);
  expect(near(result.x.offset, -1) && result.x.guide, "monitor center is an alignment target");

  auto [x, y] = findSpace(208, 208, neighbours, 720, 480, 16, 16);
  expect(near(x, 344) && near(y, 120), "gallery fills the next free slot with consistent spacing");
  const Rect added{x - 104, y - 104, 208, 208};
  expect(!overlapsWithGap(added, neighbours.front(), 16), "new widget leaves existing widgets untouched");
  const std::vector<Rect> full{{0, 0, 720, 480}};
  auto fallback = findSpace(432, 432, full, 720, 480, 16, 16);
  expect(near(fallback.first, 360) && near(fallback.second, 240), "full outputs keep additions reachable");
  fallback = findSpace(1000, 1000, {}, 720, 480, 16, 16);
  expect(near(fallback.first, 360) && near(fallback.second, 240), "oversized widgets are centered");

  const auto weather = desktop_settings::newDesktopWidgetSettings("weather", "large");
  expect(std::get<std::string>(weather.at("card_size")) == "large", "gallery size is used by the created widget");
  const auto calendar = desktop_settings::newDesktopWidgetSettings("calendar");
  expect(std::get<std::string>(calendar.at("card_size")) == "medium", "calendar retains its editor default");
  expect(
      !desktop_settings::newDesktopWidgetSettings("button", "large").contains("card_size"),
      "other widget types do not acquire unsupported settings"
  );
}
