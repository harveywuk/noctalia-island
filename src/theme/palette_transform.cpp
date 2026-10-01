#include "theme/palette_transform.h"

#include "cpp/cam/hct.h"
#include "cpp/utils/utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace noctalia::theme {

  namespace {

    namespace mcu = material_color_utilities;

    // The dark elevation ladder, plus the two terminal tokens that are the background
    // itself. terminal_selection_bg and terminal_normal_black are deliberately absent:
    // a terminal needs them to stay visible against the background, not to follow it.
    constexpr std::array<std::string_view, 12> kSurfaceRamp = {{
        "background",
        "surface",
        "surface_variant",
        "surface_dim",
        "surface_bright",
        "surface_container_lowest",
        "surface_container_low",
        "surface_container",
        "surface_container_high",
        "surface_container_highest",
        "terminal_background",
        "terminal_cursor_text",
    }};

    [[nodiscard]] std::uint32_t lowerTone(std::uint32_t argb, double shift) {
      mcu::Hct hct(argb);
      hct.set_tone(std::max(0.0, hct.get_tone() - shift));
      return hct.ToInt();
    }

    [[nodiscard]] bool isSurfaceToken(std::string_view key) {
      return std::ranges::find(kSurfaceRamp, key) != kSurfaceRamp.end();
    }

    // HCT tone is L*, so WCAG contrast between two tokens depends on their tones alone.
    [[nodiscard]] double toneContrast(double a, double b) {
      const double ya = mcu::YFromLstar(a) / 100.0;
      const double yb = mcu::YFromLstar(b) / 100.0;
      return (std::max(ya, yb) + 0.05) / (std::min(ya, yb) + 0.05);
    }

    // Moves `tone` toward `limit` only as far as needed to reach `target` against every
    // tone in `against`. Stops at `limit` when the target is out of reach.
    [[nodiscard]] double toneReaching(double tone, double limit, std::initializer_list<double> against, double target) {
      const auto meets = [&](double t) {
        return std::ranges::all_of(against, [&](double other) { return toneContrast(t, other) >= target; });
      };
      if (meets(tone)) {
        return tone;
      }
      if (!meets(limit)) {
        return limit;
      }
      double lo = tone;
      double hi = limit;
      for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2.0;
        (meets(mid) ? hi : lo) = mid;
      }
      return hi;
    }

    [[nodiscard]] std::uint32_t withTone(std::uint32_t argb, double tone) {
      mcu::Hct hct(argb);
      hct.set_tone(tone);
      return hct.ToInt();
    }

    [[nodiscard]] double toneOf(std::uint32_t argb) { return mcu::Hct(argb).get_tone(); }

    constexpr double kHighContrastText = 7.0;   // WCAG AAA body text
    constexpr double kHighContrastAccent = 4.5; // accents double as text colours

  } // namespace

  void applyPureBlackDark(TokenMap& darkTokens) {
    const auto base = darkTokens.find("surface");
    if (base == darkTokens.end()) {
      return;
    }
    // Shifting by the base tone puts `surface` on tone 0 and drops everything above it
    // by the same amount; tokens already below the base clamp to black.
    const double shift = mcu::Hct(base->second).get_tone();
    if (shift <= 0.0) {
      return;
    }
    for (const std::string_view key : kSurfaceRamp) {
      if (const auto it = darkTokens.find(std::string(key)); it != darkTokens.end()) {
        it->second = lowerTone(it->second, shift);
      }
    }
  }

  void applyPureBlackDark(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyPureBlackDark(palette.dark);
    }
  }

  void applyHighContrast(TokenMap& tokens, bool isDark) {
    // Work by role rather than by each token's own tone: a foreground and its background
    // can start on the same side of mid-tone, and stretching both the same way would
    // shrink the gap between them.
    const double away = isDark ? 100.0 : 0.0; // the direction foregrounds move from surfaces

    // 1. Surfaces recede further from the reading direction.
    for (const std::string_view key : kSurfaceRamp) {
      if (const auto it = tokens.find(std::string(key)); it != tokens.end()) {
        const double tone = toneOf(it->second);
        it->second = withTone(it->second, isDark ? std::max(0.0, tone - 20.0) : std::min(100.0, tone + 20.0));
      }
    }
    const auto surfaceIt = tokens.find("surface");
    const double surface = surfaceIt != tokens.end() ? toneOf(surfaceIt->second) : (isDark ? 0.0 : 100.0);

    // 2. Borders become strongly visible.
    for (const std::string_view key : {"outline", "outline_variant"}) {
      if (const auto it = tokens.find(std::string(key)); it != tokens.end()) {
        const double tone = toneOf(it->second);
        it->second = withTone(it->second, isDark ? std::max(tone, 80.0) : std::min(tone, 20.0));
      }
    }

    // 3. Accents that carry their own on_* foreground stay readable against the surface,
    //    keeping hue and as much of their tone as possible.
    for (auto& [key, value] : tokens) {
      if (key.starts_with("on_") || isSurfaceToken(key) || !tokens.contains("on_" + key)) {
        continue;
      }
      value = withTone(value, toneReaching(toneOf(value), away, {surface}, kHighContrastAccent));
    }

    // 4. Every on_* foreground moves toward whichever extreme separates it most from its
    //    background. Secondary surface text also has to read on the base surface.
    for (auto& [key, value] : tokens) {
      if (!key.starts_with("on_")) {
        continue;
      }
      const auto base = tokens.find(key.substr(3));
      if (base == tokens.end()) {
        continue;
      }
      const double bg = toneOf(base->second);
      const double limit = toneContrast(0.0, bg) >= toneContrast(100.0, bg) ? 0.0 : 100.0;
      const double tone = toneOf(value);
      const double target = key == "on_surface_variant" ? toneReaching(tone, limit, {bg, surface}, kHighContrastText)
                                                        : toneReaching(tone, limit, {bg}, kHighContrastText);
      value = withTone(value, target);
    }
  }

  void applyHighContrast(GeneratedPalette& palette) {
    if (!palette.dark.empty()) {
      applyHighContrast(palette.dark, true);
    }
    if (!palette.light.empty()) {
      applyHighContrast(palette.light, false);
    }
  }

} // namespace noctalia::theme
