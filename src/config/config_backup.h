#pragma once

#include "config/config_types.h"
#include "core/toml.h"

#include <array>
#include <string>
#include <vector>

namespace config_backup {
  inline constexpr std::array<const char*, 8> sections{"appearance", "bars",       "input",   "displays",
                                                       "shortcuts",  "workspaces", "session", "other"};

  struct Info {
    std::string id, name;
    std::int64_t created = 0;
    bool automatic = false;
  };
  struct Change {
    std::string path, before, after;
  };
  // A preview is an optimistic transaction: refuse to apply it if either config
  // layer changed since it was reviewed. Never rewrite hand-authored files.
  struct Plan {
    Info backup;
    std::vector<std::string> sections;
    toml::table baseline, base, candidate;
    Config config;
    std::vector<Change> changes;
    bool displaysChanged = false;
  };

  toml::table select(const toml::table&, const std::vector<std::string>& sections);
  toml::table replace(const toml::table& current, const toml::table& saved, const std::vector<std::string>& sections);
  std::vector<Change> diff(const toml::table&, const toml::table&);
} // namespace config_backup
