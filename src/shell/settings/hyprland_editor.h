#pragma once
#include "shell/settings/settings_content.h"

namespace settings {
  using AppearanceOverrides = std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>;
  AppearanceOverrides appearanceOverrides(const HyprlandAppearanceConfig& config);
  AppearanceOverrides manualAppearanceOverrides(const HyprlandAppearanceConfig& config);
  HyprlandAppearanceConfig softGlassAppearance(HyprlandAppearanceConfig config);
  HyprlandAppearanceConfig softGlassOverview(HyprlandAppearanceConfig config);
  HyprlandAppRule appAppearancePreset(HyprlandAppRule rule, std::string_view preset);
  void addHyprlandEditorEntries(std::vector<SettingEntry>& entries, const Config& config);
  std::unique_ptr<Node> makeHyprlandEditor(const SettingEntry& entry, const SettingsContentContext& ctx);
} // namespace settings
