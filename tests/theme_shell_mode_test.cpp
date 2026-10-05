// [theme].mode is the app-facing mode: it is what the resolved callback carries to templates,
// the GTK color scheme, and theme_mode_changed hooks. [theme].shell_mode pins the palette
// Noctalia itself renders with, without moving the app-facing mode.

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "net/http_client.h"
#include "tests/test_check.h"
#include "theme/theme_service.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

  struct Resolution {
    std::string appliedMode; // mode handed to the resolved callback
    std::string mode;        // ThemeService::resolvedMode()
    std::string shellMode;   // ThemeService::resolvedShellMode()
    bool shellLight = false; // ThemeService::isLightMode()
  };

  Resolution resolve(const std::filesystem::path& overridesPath, std::string_view themeToml) {
    {
      std::ofstream out(overridesPath, std::ios::trunc);
      out << themeToml;
    }

    ConfigService config;
    HttpClient http;
    noctalia::theme::ThemeService theme(config, http);

    Resolution result;
    theme.setResolvedCallback([&result, &theme](const noctalia::theme::GeneratedPalette&, std::string_view mode) {
      result.appliedMode = std::string(mode);
      TEST_CHECK(isResolvedAppsLight() == (mode == "light"));
      TEST_CHECK(isResolvedLightTheme() == theme.isLightMode());
    });
    theme.apply();
    for (auto& call : DeferredCall::takePending()) {
      call();
    }

    result.mode = std::string(theme.resolvedMode());
    result.shellMode = std::string(theme.resolvedShellMode());
    result.shellLight = theme.isLightMode();
    return result;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-theme-shell-mode-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "config" / "noctalia");
  std::filesystem::create_directories(root / "state" / "noctalia");
  std::filesystem::create_directories(root / "data");
  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("NOCTALIA_STATE_HOME", (root / "state").c_str(), 1);
  ::setenv("NOCTALIA_DATA_HOME", (root / "data").c_str(), 1);

  // ConfigService reads its persisted overrides from <state dir>/settings.toml, the same file
  // the settings GUI writes.
  const std::filesystem::path overrides = root / "state" / "noctalia" / "settings.toml";
  // follow: one mode drives Noctalia and apps together.
  {
    const Resolution r = resolve(overrides, "[theme]\nmode = \"light\"\nshell_mode = \"follow\"\n");
    TEST_CHECK(r.mode == "light");
    TEST_CHECK(r.shellMode == "light");
    TEST_CHECK(r.shellLight);
    TEST_CHECK(r.appliedMode == "light");
  }

  // Pinned dark shell, light apps: only the shell palette is held back.
  {
    const Resolution r = resolve(overrides, "[theme]\nmode = \"light\"\nshell_mode = \"dark\"\n");
    TEST_CHECK(r.mode == "light");
    TEST_CHECK(r.shellMode == "dark");
    TEST_CHECK(!r.shellLight);
    TEST_CHECK(r.appliedMode == "light");
  }

  // The inverse pinning, so a light shell cannot leak into what apps are told.
  {
    const Resolution r = resolve(overrides, "[theme]\nmode = \"dark\"\nshell_mode = \"light\"\n");
    TEST_CHECK(r.mode == "dark");
    TEST_CHECK(r.shellMode == "light");
    TEST_CHECK(r.shellLight);
    TEST_CHECK(r.appliedMode == "dark");
  }

  // An unset shell_mode behaves like follow.
  {
    const Resolution r = resolve(overrides, "[theme]\nmode = \"dark\"\n");
    TEST_CHECK(r.mode == "dark");
    TEST_CHECK(r.shellMode == "dark");
    TEST_CHECK(!r.shellLight);
    TEST_CHECK(r.appliedMode == "dark");
  }

  // Changing apps alone must publish the new mode before notifying the editor, even though
  // the shell palette is identical and both the animated and immediate paths can return early.
  for (const bool shellLight : {false, true}) {
    ConfigService config;
    TEST_CHECK(config.setOverride({"theme", "shell_mode"}, std::string(shellLight ? "light" : "dark")));
    config.setThemeMode(ThemeMode::Dark);
    HttpClient http;
    noctalia::theme::ThemeService theme(config, http);
    int callbacks = 0;
    theme.setResolvedCallback([&](const noctalia::theme::GeneratedPalette&, std::string_view mode) {
      ++callbacks;
      TEST_CHECK(isResolvedAppsLight() == (mode == "light"));
      TEST_CHECK(isResolvedLightTheme() == shellLight);
    });
    theme.apply();
    const auto pinnedPalette = palette;
    for (const bool animate : {false, true}) {
      for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
        const auto before = callbacks;
        config.setThemeMode(mode);
        if (animate)
          theme.onConfigReload();
        else
          theme.apply();
        TEST_CHECK(callbacks == before + 1);
        TEST_CHECK(isResolvedAppsLight() == (mode == ThemeMode::Light));
        TEST_CHECK(palette == pinnedPalette);
      }
    }
  }

  std::filesystem::remove_all(root);
  return 0;
}
