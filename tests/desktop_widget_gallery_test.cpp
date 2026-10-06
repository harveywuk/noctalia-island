#include "shell/desktop/desktop_widget_gallery.h"
#include "shell/desktop/desktop_widget_settings_registry.h"

#include <cstdlib>
#include <print>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using desktop_gallery::Category;
  using desktop_gallery::matches;
  expect(
      desktop_gallery::favoriteKey("plugin:a-b") != desktop_gallery::favoriteKey("plugin:a_b"),
      "plugin favorite keys cannot collide after encoding"
  );
  expect(desktop_gallery::favoriteKey("clock") == "favorite_636c6f636b", "favorite keys use valid state identifiers");
  for (const auto& spec : desktop_settings::desktopWidgetTypeSpecs())
    expect(desktop_gallery::category(spec.type) != Category::Plugins, "every built-in has a browsing category");
  expect(
      matches("media_player", "Now Playing", "Media", "PLAYING media", Category::All, false),
      "search combines label and category words without case sensitivity"
  );
  expect(
      matches("media_player", "Now Playing", "Media", "media_player", Category::Media, false),
      "config IDs remain searchable"
  );
  expect(
      !matches("media_player", "Now Playing", "Media", "Playing", Category::Home, true),
      "search respects the selected category"
  );
  expect(
      !matches("clock", "Clock", "Productivity", "", Category::Favorites, false), "favorites excludes unstarred widgets"
  );
  expect(
      matches("clock", "Clock", "Productivity", "  CLOCK  ", Category::Favorites, true),
      "favorites and trimmed search combine"
  );
  expect(
      !matches("clock", "Clock", "Productivity", "moon", Category::Favorites, true),
      "favorites does not bypass the query"
  );
  expect(
      matches("custom:clock", "Horloge ÉTÉ", "Plugins", "e\u0301te\u0301", Category::Plugins, false),
      "Unicode case folding and normalization support translated names"
  );
  expect(
      matches("custom:home", "Home Power", "Plugins", "home", Category::Plugins, true),
      "installed plugin IDs use the Plugins category"
  );
  expect(
      !matches("clock", "Clock", "Productivity", "weather clock", Category::All, false), "every search term must match"
  );
  std::println("PASS: gallery categories, combined filters, favorites, plugin IDs and Unicode search");
}
