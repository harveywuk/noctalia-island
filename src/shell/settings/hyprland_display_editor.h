#pragma once
#include "shell/settings/settings_content.h"

#include <algorithm>
namespace settings {
  struct DisplayEditorState {
    std::string selected;
    std::vector<HyprlandDisplayConfig> drafts, original;
    HyprlandDisplayConfig& draft() { return *std::ranges::find(drafts, selected, &HyprlandDisplayConfig::output); }
  };
  std::unique_ptr<Node> makeHyprlandDisplayEditor(const SettingEntry&, const SettingsContentContext&);
} // namespace settings
