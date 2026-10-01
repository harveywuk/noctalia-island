#include "system/terminal_launch.h"

#include "core/process/process.h"
#include "system/desktop_entry_launch.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <gio/gdesktopappinfo.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  std::mutex preferredMutex;
  bool isExecutableOnPath(std::string_view binary);
  std::string preferredDesktopId;
  struct TerminalCommand {
    std::vector<std::string> args;
    std::string separator;
  };

  std::optional<TerminalCommand> desktopTerminal(const std::string& id) {
    auto* entry = g_desktop_app_info_new(id.c_str());
    if (!entry)
      return {};
    const char* categories = g_desktop_app_info_get_categories(entry);
    const bool terminal = categories
        && std::string_view(categories).find("TerminalEmulator;") != std::string_view::npos
        && !g_desktop_app_info_get_boolean(entry, "Terminal")
        && !g_desktop_app_info_get_is_hidden(entry);
    char* exec = g_desktop_app_info_get_string(entry, "Exec");
    auto prepared = terminal && exec ? desktop_entry_launch::prepareCommand(exec, false) : std::nullopt;
    g_free(exec);
    g_object_unref(entry);
    if (!prepared || prepared->args.empty())
      return {};
    auto args = std::move(prepared->args);
    if (!isExecutableOnPath(args.front()))
      return {};
    std::string program = std::filesystem::path(args.front()).filename().string();
    if (program == "flatpak") {
      for (const auto& arg : args)
        if (arg.starts_with("--command="))
          program = arg.substr(10);
      std::erase_if(args, [](const auto& arg) { return arg == "@@" || arg == "@@u" || arg == "--file-forwarding"; });
    }
    static constexpr std::array<std::string_view, 12> supported{"ghostty", "kitty",          "alacritty",
                                                                "wezterm", "foot",           "footclient",
                                                                "konsole", "gnome-terminal", "kgx",
                                                                "ptyxis",  "xterm",          "x-terminal-emulator"};
    if (std::ranges::find(supported, program) == supported.end())
      return {};
    if (program == "wezterm" && std::ranges::find(args, "start") == args.end())
      args.emplace_back("start");
    return TerminalCommand{
        std::move(args),
        program == "gnome-terminal" || program == "kgx" || program == "ptyxis" || program == "wezterm" ? "--" : "-e"
    };
  }

  std::string preferred() {
    std::lock_guard lock(preferredMutex);
    return preferredDesktopId;
  }

  std::vector<std::string> tokenize(std::string_view cmd) {
    std::vector<std::string> args;
    std::string current;
    bool inSingle = false;
    bool inDouble = false;

    for (const char c : cmd) {
      if (c == '\'' && !inDouble) {
        inSingle = !inSingle;
        continue;
      }
      if (c == '"' && !inSingle) {
        inDouble = !inDouble;
        continue;
      }
      if (c == ' ' && !inSingle && !inDouble) {
        if (!current.empty()) {
          args.push_back(std::move(current));
          current.clear();
        }
        continue;
      }
      current += c;
    }
    if (!current.empty()) {
      args.push_back(std::move(current));
    }
    return args;
  }

  std::string expandExecutablePath(std::string_view binary) {
    if (binary.empty() || binary.front() != '~') {
      return std::string(binary);
    }
    return FileUtils::expandUserPath(std::string(binary)).string();
  }

  bool isExecutableOnPath(std::string_view binary) {
    if (binary.empty()) {
      return false;
    }
    if (binary.contains('/')) {
      const std::string expanded = expandExecutablePath(binary);
      return access(expanded.c_str(), X_OK) == 0;
    }

    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return false;
    }

    std::string_view path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto sep = path.find(':', start);
      const auto segment = sep == std::string_view::npos ? path.substr(start) : path.substr(start, sep - start);
      if (!segment.empty()) {
        std::string candidate(segment);
        candidate.push_back('/');
        candidate.append(binary);
        if (access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (sep == std::string_view::npos) {
        break;
      }
      start = sep + 1;
    }
    return false;
  }

  std::vector<std::string> discoverTerminal(const terminal_launch::Options& options) {
    if (!options.terminalCandidates.empty()) {
      for (const auto& candidate : options.terminalCandidates) {
        std::vector<std::string> terminal = tokenize(candidate);
        if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
          return terminal;
        }
      }
      return {};
    }
    if (!options.useSystemTerminalDiscovery) {
      return {};
    }

    if (const char* envTerminal = std::getenv("TERMINAL"); envTerminal != nullptr && envTerminal[0] != '\0') {
      std::vector<std::string> terminal = tokenize(envTerminal);
      if (!terminal.empty() && isExecutableOnPath(terminal.front())) {
        return terminal;
      }
    }

    static constexpr std::array<std::string_view, 11> kTerminalCandidates = {
        "x-terminal-emulator", "ghostty", "kitty",  "alacritty", "wezterm", "foot", "konsole",
        "gnome-terminal",      "kgx",     "ptyxis", "xterm",
    };
    for (const auto candidate : kTerminalCandidates) {
      if (isExecutableOnPath(candidate)) {
        return {std::string(candidate)};
      }
    }
    return {};
  }

  bool usesCommandSeparator(std::string_view terminal) {
    return terminal == "gnome-terminal" || terminal == "kgx" || terminal == "ptyxis";
  }

} // namespace

namespace terminal_launch {

  void setPreferredDesktopId(std::string id) {
    std::lock_guard lock(preferredMutex);
    preferredDesktopId = std::move(id);
  }

  std::vector<TerminalApp> availableTerminals() {
    std::vector<TerminalApp> result;
    auto* apps = g_app_info_get_all();
    for (auto* it = apps; it; it = it->next) {
      auto* app = G_APP_INFO(it->data);
      const char* id = g_app_info_get_id(app);
      if (id && g_app_info_should_show(app) && desktopTerminal(id))
        result.push_back({id, g_app_info_get_display_name(app)});
    }
    g_list_free_full(apps, g_object_unref);
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
  }

  std::optional<std::vector<std::string>> prepareOpen() {
    const auto id = preferred();
    if (!id.empty()) {
      const auto app = desktopTerminal(id);
      if (!app)
        return {};
      return app->args;
    }
    auto args = discoverTerminal({});
    if (args.empty())
      return {};
    return args;
  }

  std::optional<std::vector<std::string>> prepareCommand(std::string_view command, const Options& options) {
    if (command.empty()) {
      return std::nullopt;
    }

    if (options.terminalCandidates.empty() && options.useSystemTerminalDiscovery) {
      const auto id = preferred();
      if (!id.empty()) {
        auto app = desktopTerminal(id);
        if (!app)
          return {};
        if (app->args.back() != app->separator)
          app->args.push_back(app->separator);
        app->args.insert(app->args.end(), {"sh", "-lc", std::string(command)});
        return app->args;
      }
    }

    std::vector<std::string> terminal = discoverTerminal(options);
    if (terminal.empty()) {
      return std::nullopt;
    }

    if (terminal.front().contains('/')) {
      terminal.front() = expandExecutablePath(terminal.front());
    }

    const std::string termBin = std::filesystem::path(terminal.front()).filename().string();
    if (termBin == "wezterm") {
      if (std::ranges::find(terminal, "start") == terminal.end())
        terminal.emplace_back("start");
      terminal.emplace_back("--");
    } else if (usesCommandSeparator(termBin)) {
      terminal.emplace_back("--");
    } else {
      terminal.emplace_back("-e");
    }
    terminal.emplace_back("sh");
    terminal.emplace_back("-lc");
    terminal.emplace_back(command);
    return terminal;
  }

  bool launch(std::string_view command, const Options& options) {
    auto prepared = prepareCommand(command, options);
    return prepared.has_value() && process::runAsync(*prepared);
  }

} // namespace terminal_launch
