#include "compositors/hyprland/hyprland_appearance.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "shell/bar/widgets/workspace_preferences.h"

#include <format>

namespace compositors::hyprland {
  std::string workspaceRuleCommands(const std::vector<HyprlandWorkspaceConfig>& rules) {
    std::string out;
    for (const auto& rule : rules) {
      if (!rule.enabled || !workspace_preferences::validTarget(rule.workspace))
        continue;
      // Names and symbols are presentation overrides. Compositor identities and shortcuts stay stable.
      out += std::format(
          "; hl.workspace_rule({{workspace={},persistent={}", luaStringLiteral(rule.workspace), rule.persistent
      );
      if (!rule.monitor.empty())
        out += ",monitor=" + luaStringLiteral(rule.monitor);
      out += "})";
    }
    return out;
  }
} // namespace compositors::hyprland
