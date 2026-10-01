#pragma once
#include "shell/settings/settings_content.h"
namespace settings {
  std::unique_ptr<Node> makeStartupAppsEditor(const SettingsContentContext& ctx);
}
