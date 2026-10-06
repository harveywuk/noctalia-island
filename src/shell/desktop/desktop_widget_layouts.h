#pragma once

#include "config/config_types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace desktop_layouts {
  constexpr std::size_t maxLayouts = 32;
  struct Layout {
    std::string name;
    // Empty means the whole desktop. Otherwise this is the saved monitor's name.
    std::string output;
    DesktopWidgetsConfig snapshot;
    bool operator==(const Layout&) const = default;
  };
  using OutputResolver = std::function<std::string(const DesktopWidgetState&)>;

  [[nodiscard]] std::string cleanName(std::string name);
  [[nodiscard]] std::optional<std::vector<Layout>> decode(const std::string& data);
  [[nodiscard]] std::string encode(const std::vector<Layout>& layouts);
  [[nodiscard]] Layout capture(
      const DesktopWidgetsConfig& snapshot, std::string name, const std::string& output,
      const OutputResolver& resolveOutput
  );
  // Monitor presets replace only roots on the target output and their owned stack
  // members. Imported IDs are remapped if another monitor already uses them.
  [[nodiscard]] DesktopWidgetsConfig apply(
      const DesktopWidgetsConfig& current, const Layout& layout, const std::string& targetOutput,
      const OutputResolver& resolveOutput
  );
} // namespace desktop_layouts
