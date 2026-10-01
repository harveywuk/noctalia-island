#pragma once
#include "compositors/workspace_backend.h"
#include "config/config_types.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <optional>

namespace workspace_preferences {
  inline bool validTarget(std::string_view value) {
    if (value.starts_with("name:")) {
      value.remove_prefix(5);
      return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
    }
    int number = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
    return ec == std::errc{}
    && end == value.data() + value.size()
        && number > 0
        && number <= 1000
        && value == std::to_string(number);
  }
  inline const HyprlandWorkspaceConfig* find(const std::vector<HyprlandWorkspaceConfig>& rules, const Workspace& ws) {
    const auto it = std::ranges::find_if(rules, [&](const auto& r) {
      return r.enabled
          && validTarget(r.workspace)
          && (r.workspace == ws.id || (r.workspace.starts_with("name:") && r.workspace.substr(5) == ws.name));
    });
    return it == rules.end() ? nullptr : &*it;
  }
  inline std::string label(const HyprlandWorkspaceConfig& rule) {
    auto icon = StringUtils::truncateUtf8CodePoints(rule.icon, 4);
    auto name = StringUtils::truncateUtf8CodePoints(rule.label, 32);
    return icon.empty() ? name : name.empty() ? icon : icon + " " + name;
  }
  inline std::optional<std::size_t>
  step(std::size_t count, std::optional<std::size_t> current, bool forward, bool wrap) {
    if (count == 0)
      return std::nullopt;
    if (!current || *current >= count)
      return forward ? 0 : count - 1;
    if (forward && *current + 1 < count)
      return *current + 1;
    if (!forward && *current > 0)
      return *current - 1;
    if (wrap && count > 1)
      return forward ? 0 : count - 1;
    return std::nullopt;
  }
} // namespace workspace_preferences
