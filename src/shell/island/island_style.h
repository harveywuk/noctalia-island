#pragma once

#include "ui/style.h"

#include <algorithm>

namespace island {

  // Use the same silhouette while the Island lends its surface to a panel.
  // Height is in surface pixels; roundness follows the shell preference.
  // Cupertino uses Apple's larger expanded-island corners; the compact pill is a full
  // capsule either way.
  inline float surfaceRadius(float height, float scale, bool cupertino = false) {
    return std::min(height * 0.5F, Style::scaledRadius(cupertino ? 42.0F : 30.0F, scale));
  }

} // namespace island
