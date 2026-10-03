#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Launcher aliases: a short word that brings one result to the top (Raycast's aliases) and names
// it for `noctalia msg launcher-run`. Aliases set from the launcher are kept in the state directory;
// ones written in config.toml (shell.launcher.aliases) are layered on top and win on a clash.
class AliasStore {
public:
  struct Target {
    std::string providerId;
    std::string resultId;

    bool operator==(const Target&) const = default;
  };

  AliasStore();
  // For tests: a store backed by `path`.
  explicit AliasStore(std::string path);

  // Config aliases map an alias to "<provider>:<result id>", or to a bare desktop entry id
  // ("firefox" or "firefox.desktop") which resolves through `resolveDesktopId`.
  void setConfigAliases(const std::unordered_map<std::string, std::string>& aliases);

  // The target for `alias` (matched case-insensitively, surrounding space ignored).
  [[nodiscard]] std::optional<Target> find(std::string_view alias);
  // The alias for a result, or empty.
  [[nodiscard]] std::string aliasFor(std::string_view providerId, std::string_view resultId);

  // Gives `target` the alias (replacing any alias it had, and taking the alias from any other
  // result). Returns false for an empty alias.
  bool set(std::string_view alias, Target target);
  bool removeFor(std::string_view providerId, std::string_view resultId);

  // Parses "<provider>:<result id>". A spec without a known provider prefix is a desktop entry id.
  [[nodiscard]] static std::optional<Target> parseSpec(std::string_view spec);
  [[nodiscard]] static std::string normalize(std::string_view alias);

private:
  void ensureLoaded();
  void save() const;

  std::string m_path;
  bool m_loaded = false;
  // Normalized alias -> target.
  std::unordered_map<std::string, Target> m_saved;
  std::unordered_map<std::string, Target> m_config;
};
