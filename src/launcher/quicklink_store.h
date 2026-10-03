#pragma once

#include "config/config_types.h"

#include <string>
#include <string_view>
#include <vector>

// Quicklinks created or edited from the launcher. They live in the state directory; quicklinks
// written in config.toml are read separately. A saved quicklink with a built-in's id replaces it.
class QuicklinkStore {
public:
  QuicklinkStore();
  // For tests: a store backed by `path`.
  explicit QuicklinkStore(std::string path);

  [[nodiscard]] const std::vector<LauncherQuicklinkConfig>& quicklinks();
  // Adds `link`, or replaces the saved one with the same id. A link without an id gets a new one,
  // which is returned.
  std::string put(LauncherQuicklinkConfig link);
  bool remove(std::string_view id);

private:
  void ensureLoaded();
  void save() const;

  std::string m_path;
  bool m_loaded = false;
  std::vector<LauncherQuicklinkConfig> m_links;
};
