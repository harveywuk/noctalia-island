#pragma once

#include "capture/screencopy_capture.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capture {
  struct WindowTarget {
    std::string id; // Canonical compositor window ID, lowercase hex without the 0x prefix.
    LogicalRect bounds;
    int layer = 0;
    int focusOrder = 0;
    std::string title;
  };

  // Frontmost first. Only compositor-reported visible, mapped windows are selectable.
  [[nodiscard]] std::optional<std::vector<WindowTarget>> parseHyprlandCaptureWindows(std::string_view json);
  [[nodiscard]] const WindowTarget* windowAt(const std::vector<WindowTarget>& windows, double x, double y);
} // namespace capture
