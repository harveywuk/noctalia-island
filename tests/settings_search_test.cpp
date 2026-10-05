// Settings search: everyday queries find the setting a person means, ranked first.
#include "config/config_types.h"
#include "i18n/i18n_service.h"
#include "shell/settings/settings_registry.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

  std::vector<const settings::SettingEntry*>
  search(const std::vector<settings::SettingEntry>& registry, std::string_view query) {
    return settings::rankedSettingMatches(registry, query);
  }

} // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);
  i18n::Service::instance().init("en");

  Config cfg;
  settings::RegistryEnvironment env;
  env.hyprlandAppearanceSupported = true;
  env.screencopySupported = true;
  env.systemdUserManaged = true;
  env.gammaControlAvailable = true;
  env.batteryAvailable = true;
  env.systemBatteryAvailable = true;
  const auto registry = settings::buildSettingsRegistry(cfg, nullptr, nullptr, env);

  const auto first = [&](std::string_view query) -> std::string {
    const auto hits = search(registry, query);
    return hits.empty() ? std::string{} : hits.front()->title;
  };
  const auto inTop = [&](std::string_view query, std::string_view title, std::size_t n) {
    const auto hits = search(registry, query);
    for (std::size_t i = 0; i < hits.size() && i < n; ++i)
      if (hits[i]->title == title)
        return true;
    return false;
  };
  // Words match in any order, anywhere in the entry.
  assert(first("dock size") == "Icon Size");
  assert(first("size dock") == "Icon Size");
  assert(first("island height") == "Compact height");
  // Separators and spelling variants.
  assert(first("auto hide") == "Auto-Hide");
  assert(first("autohide") == "Auto-Hide");
  assert(first("wifi") == "Wi-Fi" && first("wi-fi") == "Wi-Fi");
  assert(search(registry, "wifi").size() == search(registry, "wi-fi").size());
  // Synonyms and plurals.
  assert(inTop("mouse speed", "Pointer speed", 2));
  assert(inTop("rounded corners", "Corner Roundness", 4));
  // The setting named by the query ranks first, not one that merely mentions it.
  assert(first("language") == "Language");
  assert(first("lock").starts_with("Lock"));
  assert(first("volume") == "Volume");
  assert(first("font") == "Font Family");
  // Partial words still match, and every word must match.
  assert(!search(registry, "opac").empty());
  assert(search(registry, "dock zzzz").empty());

  if (std::getenv("SETTINGS_SEARCH_REPORT") != nullptr) {
    for (const char* query :
         {"glass",     "blur",       "dock size",       "size dock",    "palette",       "dark mode",
          "wifi",      "wi-fi",      "night light",     "transparency", "opacity",       "font",
          "clock",     "seconds",    "animation speed", "wallpaper",    "notifications", "do not disturb",
          "volume",    "brightness", "keyboard",        "shortcut",     "mouse speed",   "scroll",
          "lock",      "screenshot", "rounded corners", "corner",       "island height", "auto hide",
          "hide dock", "battery",    "language",        "scale",        "icon size",     "gaps"}) {
      const auto hits = search(registry, query);
      std::cout << query << " (" << hits.size() << "):";
      for (std::size_t i = 0; i < hits.size() && i < 4; ++i)
        std::cout
            << " | "
            << hits[i]->title
            << " ["
            << settingsSectionId(hits[i]->section)
            << "/"
            << hits[i]->group
            << "]";
      std::cout << "\n";
    }
  }
  return 0;
}
