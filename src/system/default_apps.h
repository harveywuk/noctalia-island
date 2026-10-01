#pragma once

#include <string>
#include <vector>

class ConfigService;

namespace default_apps {
  struct Role {
    std::string id;
    std::vector<std::string> types;
  };
  struct App {
    std::string id, name;
  };
  const std::vector<Role>& roles();
  std::vector<App> choices(const Role&);
  App current(const std::string& type);
  // Changes only the chosen role's MIME keys. Undo persists across shell restarts
  // and merges unrelated subsequent edits instead of replacing the whole file.
  bool set(const std::string& role, const std::string& desktopId, std::string& error);
  bool canUndo();
  bool undo(std::string& error);
  bool setTerminal(ConfigService&, const std::string& desktopId, std::string& error);
  bool undoTerminal(ConfigService&, std::string& error);
  bool canUndoTerminal(const ConfigService&);
} // namespace default_apps
