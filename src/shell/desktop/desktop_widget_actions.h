#pragma once

#include "config/config_types.h"

namespace desktop_widgets {
  enum class QuickAction {
    Small = 1,
    Medium,
    Large,
    Configure,
    Duplicate,
    Remove,
    Pin,
    SmartRotate,
    OpenPlayer,
    Undo,
    Redo,
    Edit
  };

  // Applies one desktop layout edit. Runtime pins and player actions are handled by the host.
  bool applyQuickAction(DesktopWidgetsConfig& snapshot, const std::string& id, QuickAction action);
} // namespace desktop_widgets
