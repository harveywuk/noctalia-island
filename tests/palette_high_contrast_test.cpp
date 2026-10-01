// Accessibility high contrast must bring every shell foreground to at least its WCAG
// minimum against the background it is drawn on, for every built-in palette and mode.
// A pair may lose some contrast it had to spare when its background is itself raised
// to stay readable against the surface.

#include "tests/test_check.h"
#include "theme/builtin_palettes.h"
#include "theme/palette_transform.h"

#include <algorithm>
#include <cmath>
#include <print>

namespace {

  double channel(float c) { return c <= 0.04045F ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

  double luminance(const Color& c) { return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b); }

  double contrast(const Color& a, const Color& b) {
    const double x = luminance(a);
    const double y = luminance(b);
    return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
  }

  struct Pair {
    const char* name;
    Color Palette::* fg;
    Color Palette::* bg;
    double minimum;
  };

  // Rounding to 8-bit channels can shave a little off a target that was met exactly.
  constexpr double kTolerance = 0.05;

  constexpr Pair kPairs[] = {
      {"on_surface/surface", &Palette::onSurface, &Palette::surface, 7.0},
      {"on_surface_variant/surface", &Palette::onSurfaceVariant, &Palette::surface, 7.0},
      {"on_surface_variant/surface_variant", &Palette::onSurfaceVariant, &Palette::surfaceVariant, 7.0},
      {"on_primary/primary", &Palette::onPrimary, &Palette::primary, 4.5},
      {"on_secondary/secondary", &Palette::onSecondary, &Palette::secondary, 4.5},
      {"on_tertiary/tertiary", &Palette::onTertiary, &Palette::tertiary, 4.5},
      {"on_error/error", &Palette::onError, &Palette::error, 4.5},
      {"primary/surface", &Palette::primary, &Palette::surface, 4.5},
      {"error/surface", &Palette::error, &Palette::surface, 4.5},
      {"outline/surface", &Palette::outline, &Palette::surface, 3.0},
  };

  void check(std::string_view name, const char* mode, const Palette& before, const Palette& after) {
    for (const Pair& pair : kPairs) {
      const double was = contrast(before.*pair.fg, before.*pair.bg);
      const double now = contrast(after.*pair.fg, after.*pair.bg);
      if (now + kTolerance < pair.minimum) {
        std::println(
            stderr, "{}/{} {}: {:.2f} -> {:.2f} (needs {:.1f})", name, mode, pair.name, was, now, pair.minimum
        );
        TEST_CHECK(false);
      }
    }
  }

} // namespace

int main() {
  using namespace noctalia::theme;
  for (const BuiltinPalette& builtin : builtinPalettes()) {
    const GeneratedPalette original = expandBuiltinPalette(builtin);
    GeneratedPalette boosted = original;
    applyHighContrast(boosted);
    check(builtin.name, "dark", mapGeneratedPaletteMode(original.dark), mapGeneratedPaletteMode(boosted.dark));
    check(builtin.name, "light", mapGeneratedPaletteMode(original.light), mapGeneratedPaletteMode(boosted.light));
  }
  return 0;
}
