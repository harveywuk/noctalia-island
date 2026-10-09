#include "core/files/resource_paths.h"
#include "dbus/bluetooth/bluetooth_glyphs.h"
#include "dbus/upower/upower_service.h"
#include "render/text/cairo_glyph_renderer.h"
#include "render/text/glyph_font.h"
#include "render/text/glyph_registry.h"
#include "shell/desktop/desktop_widget_gallery.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/settings_registry.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <cmath>
#include <cstdlib>
#include <format>
#include <print>
#include <set>

namespace {
  void expect(bool condition, const std::string& message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  FT_Library library;
  FT_Face face;
  FT_Face cupertinoFace;
  expect(FT_Init_FreeType(&library) == 0, "FreeType initializes");
  expect(
      FT_New_Face(library, paths::assetPath("fonts/noctalia-tabler.ttf").c_str(), 0, &face) == 0,
      "bundled icon font loads"
  );
  expect(
      FT_New_Face(library, paths::assetPath("fonts/noctalia-cupertino.ttf").c_str(), 0, &cupertinoFace) == 0,
      "bundled Cupertino font loads"
  );
  CairoGlyphRenderer renderer;
  renderer.initialize(
      paths::assetPath("fonts/noctalia-tabler.ttf").string(), paths::assetPath("fonts/noctalia-cupertino.ttf").string(),
      nullptr, nullptr
  );
  const auto covered = [&](char32_t cp) {
    return FT_Get_Char_Index(GlyphFont::isCupertino(cp) ? cupertinoFace : face, GlyphFont::nativeCodepoint(cp)) != 0;
  };
  const auto hasInk = [&](char32_t cp) {
    for (const float scale : {1.0F, 1.5F, 1.875F}) {
      const auto metrics = renderer.measureGlyph(scale, cp, 16);
      if (!(metrics.right > metrics.left && metrics.bottom > metrics.top))
        return false;
    }
    return true;
  };
  const auto& icons = GlyphRegistry::tablerIcons();
  for (const auto& [alias, target] : GlyphRegistry::aliases()) {
    expect(icons.contains(std::string(target)), std::format("alias {} names a native glyph", alias));
    const auto cp = GlyphRegistry::lookup(alias);
    expect(GlyphFont::isCupertino(cp), std::format("shared shell alias {} uses the Cupertino family", alias));
    expect(cp == GlyphRegistry::lookup(target), std::format("alias {} uses the shared symbol", alias));
    expect(covered(cp) && hasInk(cp), std::format("alias {} renders in its bundled font", alias));
    expect(GlyphRegistry::categoryFor(alias).has_value(), std::format("alias {} has a picker category", alias));
  }
  for (const auto& [name, cp] : icons) {
    const auto shared = GlyphRegistry::lookup(name);
    if (shared != cp)
      expect(covered(shared) && hasInk(shared), std::format("replacement for {} renders", name));
    expect(GlyphRegistry::lookup("tabler:" + name) == cp, "explicit Tabler names preserve their font");
    expect(
        GlyphRegistry::lookup(std::format("U+{:X}", static_cast<unsigned>(cp))) == cp,
        "saved Tabler codepoints remain unchanged"
    );
    const auto active = GlyphRegistry::emphasized(cp);
    if (active != cp)
      expect(FT_Get_Char_Index(face, active) != 0, std::format("active variant of {} exists in the font", name));
  }
  expect(GlyphRegistry::cupertinoIcons().size() > 1000, "Cupertino catalog is available offline");
  for (const auto& [name, cp] : GlyphRegistry::cupertinoIcons()) {
    expect(GlyphRegistry::contains("cupertino:" + name), "Cupertino names are discoverable");
    expect(GlyphRegistry::lookup("cupertino:" + name) == cp, "explicit Cupertino names select their font");
    expect(GlyphFont::isCupertino(cp), "Cupertino IDs cannot collide with Tabler codepoints");
    expect(covered(cp) && hasInk(cp), std::format("Cupertino {} has ink at normal and fractional scales", name));
    expect(covered(GlyphRegistry::emphasized(cp)), "Cupertino filled variants exist in the same font");
  }
  std::set<char32_t> upstream;
  for (const auto& [name, cp] : GlyphRegistry::cupertinoIcons())
    upstream.insert(cp);
  expect(!GlyphRegistry::noctaliaIcons().empty(), "companion symbols are bundled");
  for (const auto& [name, cp] : GlyphRegistry::noctaliaIcons()) {
    expect(!upstream.contains(cp), "companion codepoints cannot replace upstream symbols");
    expect(GlyphRegistry::lookup("noctalia:" + name) == cp, "qualified companion names resolve");
    expect(covered(cp) && hasInk(cp), std::format("companion {} renders at all tested scales", name));
    expect(covered(GlyphRegistry::emphasized(cp)), "companion filled variants exist");
  }
  unsigned long previousInk = 0;
  for (const auto name : {"battery-0", "battery-1", "battery-2", "battery-3", "battery-4"}) {
    const auto cp = GlyphRegistry::lookup(name);
    FT_Set_Pixel_Sizes(cupertinoFace, 0, 96);
    expect(
        FT_Load_Char(cupertinoFace, GlyphFont::nativeCodepoint(cp), FT_LOAD_RENDER) == 0,
        "battery charge state rasterizes"
    );
    const auto& bitmap = cupertinoFace->glyph->bitmap;
    unsigned long ink = 0;
    for (unsigned y = 0; y < bitmap.rows; ++y)
      for (unsigned x = 0; x < bitmap.width; ++x)
        ink += bitmap.buffer[y * static_cast<unsigned>(bitmap.pitch) + x];
    expect(ink > previousInk, "each battery step visibly increases the fill");
    previousInk = ink;
  }
  expect(
      GlyphRegistry::lookup("battery-charging") != GlyphRegistry::lookup("battery-4")
          && GlyphRegistry::lookup("battery-exclamation") != GlyphRegistry::lookup("battery-charging"),
      "charging and warnings keep distinct symbols"
  );
  for (const auto name : {
           "home",
           "settings",
           "bell",
           "media-play",
           "microphone",
           "wifi",
           "download",
           "dots-circle-horizontal",
           "world-off",
           "keyboard-off",
           "photo-off",
           "calendar-off",
           "devices",
           "calculator",
           "code",
           "script",
           "text-recognition",
           "circle-dot",
           "sort-a-z",
           "sort-z-a",
           "bolt-off",
       })
    expect(GlyphFont::isCupertino(GlyphRegistry::lookup(name)), "core shell symbols use Cupertino");
  const auto islandMetrics = renderer.measureGlyph(1, GlyphRegistry::lookup("capsule"), 24);
  expect(
      islandMetrics.right - islandMetrics.left > 2 * (islandMetrics.bottom - islandMetrics.top),
      "Island identity is a horizontal capsule"
  );
  for (const auto name : {"brand-steam", "brand-google"})
    expect(GlyphRegistry::lookup(name) == icons.at(name), "brand identities retain their original symbol");
  expect(
      !GlyphRegistry::contains("cupertino:not-a-symbol")
          && !GlyphRegistry::contains("tabler:not-a-symbol")
          && !GlyphRegistry::contains("noctalia:not-a-symbol"),
      "unknown qualified glyphs are rejected"
  );
  // These two fonts really share codepoints. Verify measurement chooses the face,
  // rather than merely asserting that our remapped IDs are different.
  const auto bell = GlyphRegistry::lookup("cupertino:bell_fill");
  const auto nativeBell = GlyphFont::nativeCodepoint(bell);
  expect(covered(nativeBell), "collision fixture exists in both fonts");
  const auto cupertinoMetrics = renderer.measureGlyph(1, bell, 24);
  const auto tablerMetrics = renderer.measureGlyph(1, nativeBell, 24);
  expect(
      std::abs(cupertinoMetrics.right - tablerMetrics.right) > .01F
          || std::abs(cupertinoMetrics.top - tablerMetrics.top) > .01F,
      "overlapping native codepoints are measured with distinct fonts"
  );
  expect(
      GlyphRegistry::emphasized(GlyphRegistry::lookup("home")) == GlyphRegistry::lookup("home-filled"),
      "navigation has an active silhouette"
  );
  expect(
      GlyphRegistry::emphasized(GlyphRegistry::lookup("search")) == GlyphRegistry::lookup("search"),
      "icons without a filled counterpart retain their shape"
  );
  expect(GlyphRegistry::emphasized(0) == 0, "unset glyph remains unset");
  expect(
      GlyphRegistry::lookup("U+0041") == U'A' && GlyphRegistry::lookup("0x0041") == U'A',
      "explicit codepoints remain available"
  );
  for (const auto& spec : desktop_settings::desktopWidgetTypeSpecs())
    expect(
        GlyphRegistry::contains(desktop_gallery::glyph(spec.type))
            && GlyphFont::isCupertino(GlyphRegistry::lookup(desktop_gallery::glyph(spec.type))),
        std::format("{} has a matching gallery symbol", spec.type)
    );
  std::set<settings::SettingsSection> sections;
  for (const auto& pane : settings::settingsSectionDescriptors()) {
    expect(!pane.id.empty(), "settings pane has a stable ID");
    expect(sections.insert(pane.section).second, "settings panes are not duplicated");
    expect(
        GlyphRegistry::contains(pane.glyph) && GlyphFont::isCupertino(GlyphRegistry::lookup(pane.glyph)),
        std::format("{} has a matching settings symbol", pane.id)
    );
  }
  for (unsigned kind = 0; kind <= static_cast<unsigned>(BluetoothDeviceKind::Tv); ++kind)
    expect(
        GlyphRegistry::contains(bluetoothDeviceGlyphName(static_cast<BluetoothDeviceKind>(kind))),
        "Bluetooth device icon resolves"
    );
  expect(
      std::string_view(bluetoothDeviceGlyphName(BluetoothDeviceKind::Keyboard))
          == batteryDeviceGlyphName(UPowerDeviceType::Keyboard),
      "keyboard uses the same identity for Bluetooth and UPower"
  );
  expect(
      std::string_view(bluetoothDeviceGlyphName(BluetoothDeviceKind::Headphones))
          == batteryDeviceGlyphName(UPowerDeviceType::Headphones),
      "headphones use the same identity for Bluetooth and UPower"
  );
  FT_Done_Face(face);
  FT_Done_Face(cupertinoFace);
  FT_Done_FreeType(library);
  std::println(
      "PASS: mixed-font rendering, codepoint compatibility, active variants, device identities and gallery symbols"
  );
}
