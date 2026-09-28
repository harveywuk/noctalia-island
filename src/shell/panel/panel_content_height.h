#pragma once

#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"

#include <algorithm>

namespace panel_content {
  // Read the laid-out content, ignoring space allocated only by flex growth.
  // Scroll views expose their whole measured content, including virtualized lists.
  inline float height(const Node* node) {
    if (!node || !node->visible())
      return 0;
    if (const auto* scroll = dynamic_cast<const ScrollView*>(node)) {
      return scroll->content() ? scroll->content()->height() + 2 * scroll->viewportPaddingV() : 0;
    }
    if (const auto* flex = dynamic_cast<const Flex*>(node)) {
      float extent = 0;
      int count = 0;
      for (const auto& child : flex->children()) {
        if (!child->visible() || !child->participatesInLayout())
          continue;
        const float childHeight = height(child.get());
        if (flex->direction() == FlexDirection::Vertical) {
          extent += childHeight;
        } else if (flex->wrap()) {
          extent = std::max(extent, child->y() - flex->paddingTop() + childHeight);
        } else {
          extent = std::max(extent, childHeight);
        }
        ++count;
      }
      if (flex->direction() == FlexDirection::Vertical && count > 1)
        extent += static_cast<float>(count - 1) * flex->gap();
      extent = std::max(flex->minHeight(), extent + flex->paddingTop() + flex->paddingBottom());
      return flex->maxHeight() > 0 ? std::min(extent, flex->maxHeight()) : extent;
    }
    // Custom layout nodes (calendar grids, virtual canvases, graphs) own their
    // measured extent and are not stretch-only Flex containers.
    return node->height();
  }
} // namespace panel_content
