#pragma once
#include "core/timer_manager.h"
#include "shell/settings/settings_content.h"

namespace settings {
  struct DefaultAppsEditorState {
    std::string message;
    Timer refresh;
  };
  std::unique_ptr<Node> makeDefaultAppsEditor(const SettingsContentContext&);
} // namespace settings
