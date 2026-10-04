#pragma once

#include "compositors/hyprland/hyprland_event_handler.h"
#include "config/config_types.h"
#include "core/timer_manager.h"
#include "render/core/color.h"

#include <string>

namespace compositors::hyprland {

  struct AppearanceProfileResolution {
    HyprlandAppearanceConfig appearance;
    std::string profile;
    bool fallback = false;
  };
  [[nodiscard]] AppearanceProfileResolution resolveAppearanceProfile(const ShellConfig& shell, bool light);

  [[nodiscard]] HyprlandMotionCurve resolvedMotionCurve(HyprlandMotionCurve curve);
  [[nodiscard]] std::string
  pluginAppearanceCommands(const HyprlandAppearanceConfig& config, Color primary, Color surface);
  [[nodiscard]] std::string appearanceCommand(const HyprlandAppearanceConfig& config, Color primary, Color surface);
  [[nodiscard]] std::string appRuleCommands(const std::vector<HyprlandAppRule>& rules);
  [[nodiscard]] std::string placementRuleProblem(const HyprlandPlacementRule& rule);
  [[nodiscard]] std::string placementRuleCommands(const std::vector<HyprlandPlacementRule>& rules);
  [[nodiscard]] std::string workspaceRuleCommands(const std::vector<HyprlandWorkspaceConfig>&);
  [[nodiscard]] std::string gestureInputCommands(const HyprlandInputConfig& config);
  [[nodiscard]] std::string keyboardInputCommands(const HyprlandInputConfig& config);
  [[nodiscard]] std::string inputMotionCommands(const HyprlandInputConfig& config);
  [[nodiscard]] std::string windowBehaviourCommands(const HyprlandWindowBehaviourConfig& config);
  [[nodiscard]] std::string tilingCommands(const HyprlandTilingConfig& config);
  [[nodiscard]] HyprlandInputConfig inputMotionPreset(HyprlandInputConfig config, std::string_view preset);

  // Runtime overrides are stored in Noctalia's config, never in the user's Lua file.
  class HyprlandAppearance final : public HyprlandEventHandler {
  public:
    explicit HyprlandAppearance(HyprlandRuntime& runtime);
    void sync(
        const HyprlandAppearanceConfig& config, Color primary, Color surface,
        const std::vector<HyprlandAppRule>& rules = {}, const HyprlandInputConfig& input = {},
        const HyprlandWindowBehaviourConfig& behaviour = {}, const HyprlandTilingConfig& tiling = {},
        const std::vector<HyprlandPlacementRule>& placementRules = {},
        const std::vector<HyprlandWorkspaceConfig>& workspaces = {},
        const std::vector<HyprlandKeybindConfig>& keybinds = {}
    );
    void handleEvent(std::string_view event, std::string_view data) override;
    void notifyCleanup() override;
    void notifyChanged() override;

  private:
    void apply();
    void syncGlassMasking();
    HyprlandAppearanceConfig m_config;
    HyprlandInputConfig m_input;
    HyprlandWindowBehaviourConfig m_behaviour;
    HyprlandTilingConfig m_tiling;
    std::vector<HyprlandAppRule> m_rules;
    std::vector<HyprlandPlacementRule> m_placementRules;
    std::vector<HyprlandWorkspaceConfig> m_workspaces;
    std::vector<HyprlandKeybindConfig> m_keybinds;
    bool m_rulesInitialized = false;
    Color m_primary;
    Color m_surface;
    std::string m_lastCommand;
    Timer m_applyTimer;
  };

} // namespace compositors::hyprland
