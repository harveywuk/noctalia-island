#pragma once

#include "ui/style.h"

#include <algorithm>

namespace island {

  // Use the same silhouette while the Island lends its surface to a panel.
  // Height is in surface pixels; roundness follows the shell preference.
  inline float surfaceRadius(float height, float scale) {
    return std::min(height * 0.5F, Style::scaledRadius(30.0F, scale));
  }

} // namespace island
