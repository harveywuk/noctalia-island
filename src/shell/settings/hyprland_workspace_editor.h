#pragma once
#include "shell/settings/settings_content.h"
namespace settings {
  std::unique_ptr<Node> makeHyprlandWorkspaceEditor(const SettingsContentContext&);
}
