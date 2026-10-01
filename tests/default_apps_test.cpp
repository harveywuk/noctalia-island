#include "config/config_service.h"
#include "system/default_apps.h"
#include "system/terminal_launch.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <print>
#include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  int failures = 0;
  void check(bool value, const std::string& message) {
    if (!value) {
      ++failures;
      std::println(stderr, "FAIL: {}", message);
    }
  }
  void put(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  }
  std::string get(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  void settle() {
    for (int i = 0; i < 8; ++i) {
      while (g_main_context_iteration(nullptr, false)) {
      }
      usleep(100000);
    }
  }
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("noctalia-default-apps-" + std::to_string(getpid()));
  fs::remove_all(root);
  for (const auto& [key, path] : std::vector<std::pair<const char*, fs::path>>{
           {"HOME", root},
           {"XDG_CONFIG_HOME", root / "config"},
           {"NOCTALIA_CONFIG_HOME", root / "config"},
           {"XDG_DATA_HOME", root / "data"},
           {"XDG_DATA_DIRS", root / "system-data"},
           {"XDG_CONFIG_DIRS", root / "system-config"},
           {"XDG_CACHE_HOME", root / "cache"},
           {"XDG_STATE_HOME", root / "state"}
       })
    setenv(key, path.c_str(), 1);
  setenv("XDG_CURRENT_DESKTOP", "Hyprland", 1);
  const auto apps = root / "data/applications";
  const std::string types = "x-scheme-handler/http;x-scheme-handler/https;text/html;application/xhtml+xml;text/"
                            "plain;application/pdf;inode/directory;x-scheme-handler/mailto;";
  for (const auto* id : {"old", "new", "external"})
    put(apps / (std::string(id) + ".desktop"),
        "[Desktop Entry]\nType=Application\nName=" + std::string(id) + "\nExec=/bin/true %U\nMimeType=" + types + "\n");
  put(apps / "hidden.desktop",
      "[Desktop Entry]\nType=Application\nName=Hidden\nHidden=true\nExec=/bin/true %U\nMimeType=" + types + "\n");
  // Supply the association cache without invoking any installed desktop app.
  std::string cache = "[MIME Cache]\n";
  for (const auto& role : default_apps::roles())
    for (const auto& type : role.types)
      cache += type + "=old.desktop;new.desktop;external.desktop;\n";
  put(apps / "mimeinfo.cache", cache);
  const auto mime = root / "config/mimeapps.list", desktop = root / "config/hyprland-mimeapps.list";
  const std::string baseline =
      "# keep this comment\n[Default "
      "Applications]\ntext/plain=old.desktop;\napplication/pdf=old.desktop;\n[Other]\nkeep=value\n";
  const std::string desktopBaseline =
      "# advanced override\n[Default "
      "Applications]\nx-scheme-handler/http=old.desktop;\nx-scheme-handler/https=old.desktop;\n";
  put(mime, baseline);
  put(desktop, desktopBaseline);
  std::string error;
  auto choices = default_apps::choices(default_apps::roles().front());
  check(choices.size() == 3, "installed compatible apps only");
  check(default_apps::set("browser", "new.desktop", error), "set browser: " + error);
  settle();
  for (const auto& type : default_apps::roles().front().types)
    check(default_apps::current(type).id == "new.desktop", "GIO resolves browser for " + type);
  check(get(mime).find("# keep this comment") != std::string::npos, "comments preserved");
  check(default_apps::canUndo(), "persistent undo exists");
  check(default_apps::undo(error), "undo browser: " + error);
  check(get(mime) == baseline && get(desktop) == desktopBaseline, "exact original files restored");
  check(default_apps::set("browser", "new.desktop", error), "set before unrelated edit");
  auto edited = get(mime);
  auto position = edited.find("text/plain=old.desktop;");
  check(position != std::string::npos, "unrelated text default retained");
  if (position != std::string::npos)
    edited.replace(position, std::string("text/plain=old.desktop;").size(), "text/plain=external.desktop;");
  put(mime, edited);
  check(default_apps::undo(error), "undo merges unrelated edits: " + error);
  check(get(mime).find("text/plain=external.desktop;") != std::string::npos, "external edit preserved");
  check(default_apps::set("browser", "new.desktop", error), "set before conflicting edit");
  edited = get(desktop);
  position = edited.find("x-scheme-handler/http=new.desktop;");
  if (position != std::string::npos)
    edited.replace(
        position, std::string("x-scheme-handler/http=new.desktop;").size(), "x-scheme-handler/http=external.desktop;"
    );
  put(desktop, edited);
  const auto untouched = get(mime);
  check(!default_apps::undo(error), "conflicting external edit blocks undo");
  check(get(mime) == untouched && get(desktop) == edited, "conflict does not partially undo");
  check(!default_apps::set("browser", "hidden.desktop", error), "hidden app refused");
  check(!default_apps::set("browser", "missing.desktop", error), "removed app refused");
  put(mime, "[broken\n");
  check(!default_apps::set("pdf", "new.desktop", error) && get(mime) == "[broken\n", "malformed file left intact");
  fs::remove(mime);
  fs::remove(desktop);
  check(default_apps::set("pdf", "new.desktop", error), "create missing MIME file");
  check(default_apps::undo(error) && !fs::exists(mime), "undo removes newly created file");

  const auto ghostty = root / "bin/ghostty", gnome = root / "bin/gnome-terminal", wezterm = root / "bin/wezterm";
  for (const auto& binary : {ghostty, gnome, wezterm}) {
    put(binary, "#!/bin/sh\nexit 0\n");
    fs::permissions(binary, fs::perms::owner_all);
  }
  put(apps / "terminal.desktop",
      "[Desktop Entry]\nType=Application\nName=Test Terminal\nCategories=System;TerminalEmulator;\nExec="
          + ghostty.string()
          + " --title \"Two Words\"\n");
  put(
      apps / "gnome.desktop",
      "[Desktop Entry]\nType=Application\nName=Test Gnome\nCategories=TerminalEmulator;\nExec=" + gnome.string() + "\n"
  );
  put(apps / "wezterm.desktop",
      "[Desktop Entry]\nType=Application\nName=Test Wezterm\nCategories=TerminalEmulator;\nExec="
          + wezterm.string()
          + " start\n");
  put(root / "config/noctalia/config.toml", "[shell]\nsetup_wizard_enabled=false\n");
  settle();
  ConfigService config;
  config.addReloadCallback([&config] {
    terminal_launch::setPreferredDesktopId(config.config().shell.preferredTerminal);
  });
  check(default_apps::setTerminal(config, "terminal.desktop", error), "set preferred terminal: " + error);
  auto command = terminal_launch::prepareCommand("printf '%s' 'quoted data'");
  check(
      command
          && *command
              == std::vector<
                  std::
                      string>{ghostty.string(), "--title", "Two Words", "-e", "sh", "-lc", "printf '%s' 'quoted data'"},
      "preferred terminal preserves arguments"
  );
  check(
      terminal_launch::prepareOpen() == std::vector<std::string>{ghostty.string(), "--title", "Two Words"},
      "open terminal uses its desktop command"
  );
  check(default_apps::canUndoTerminal(config) && default_apps::undoTerminal(config, error), "terminal undo: " + error);
  check(
      config.config().shell.preferredTerminal.empty() && !config.hasOverride({"shell", "preferred_terminal"}),
      "terminal undo restores inheritance"
  );
  terminal_launch::setPreferredDesktopId("gnome.desktop");
  command = terminal_launch::prepareCommand("true");
  check(
      command && *command == std::vector<std::string>{gnome.string(), "--", "sh", "-lc", "true"},
      "absolute GNOME terminal uses --"
  );
  terminal_launch::setPreferredDesktopId("wezterm.desktop");
  command = terminal_launch::prepareCommand("true");
  check(
      command && *command == std::vector<std::string>{wezterm.string(), "start", "--", "sh", "-lc", "true"},
      "WezTerm uses start -- once"
  );
  terminal_launch::setPreferredDesktopId("missing.desktop");
  check(!terminal_launch::prepareCommand("true"), "unavailable preference fails instead of launching another terminal");
  terminal_launch::setPreferredDesktopId("");
  fs::remove_all(root);
  std::println(
      "{}: system defaults, desktop overrides, precise undo and terminal preparation", failures ? "FAIL" : "PASS"
  );
  return failures ? 1 : 0;
}
