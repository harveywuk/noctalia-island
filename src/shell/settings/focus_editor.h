#pragma once

#include "shell/settings/settings_content.h"

namespace settings {
  std::unique_ptr<Node> makeFocusEditor(const SettingsContentContext& ctx);
} // namespace settings
