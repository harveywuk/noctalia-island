#include "core/files/resource_paths.h"
#include "dbus/bluetooth/bluetooth_glyphs.h"
#include "dbus/upower/upower_service.h"
#include "render/text/glyph_registry.h"
#include "shell/desktop/desktop_widget_gallery.h"
#include "shell/desktop/desktop_widget_settings_registry.h"
#include "shell/settings/settings_registry.h"

#include <ft2build.h>
#include FT_FREETYPE_H
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
  expect(FT_Init_FreeType(&library) == 0, "FreeType initializes");
  expect(
      FT_New_Face(library, paths::assetPath("fonts/noctalia-tabler.ttf").c_str(), 0, &face) == 0,
      "bundled icon font loads"
  );
  const auto& icons = GlyphRegistry::tablerIcons();
  for (const auto& [alias, target] : GlyphRegistry::aliases()) {
    expect(icons.contains(std::string(target)), std::format("alias {} names a native glyph", alias));
    const auto cp = GlyphRegistry::lookup(alias);
    expect(cp == icons.at(std::string(target)), std::format("alias {} resolves directly", alias));
    expect(FT_Get_Char_Index(face, cp) != 0, std::format("alias {} exists in the bundled font", alias));
    expect(GlyphRegistry::categoryFor(alias).has_value(), std::format("alias {} has a picker category", alias));
  }
  for (const auto& [name, cp] : icons) {
    const auto active = GlyphRegistry::emphasized(cp);
    if (active != cp)
      expect(FT_Get_Char_Index(face, active) != 0, std::format("active variant of {} exists in the font", name));
  }
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
        GlyphRegistry::contains(desktop_gallery::glyph(spec.type)), std::format("{} has a gallery symbol", spec.type)
    );
  std::set<settings::SettingsSection> sections;
  for (const auto& pane : settings::settingsSectionDescriptors()) {
    expect(!pane.id.empty(), "settings pane has a stable ID");
    expect(sections.insert(pane.section).second, "settings panes are not duplicated");
    expect(GlyphRegistry::contains(pane.glyph), std::format("{} has a settings symbol", pane.id));
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
  FT_Done_FreeType(library);
  std::println("PASS: aliases, bundled font coverage, active variants, device identities and gallery symbols");
}
