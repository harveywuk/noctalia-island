#pragma once

#include "render/animation/animation.h"

#include <algorithm>
#include <cmath>

namespace Motion {

  inline constexpr float feedbackMs = 120;
  inline constexpr float revealMs = 240;
  inline constexpr float dismissMs = 180;
  inline constexpr float resizeMs = 280;
  inline constexpr Easing reveal = Easing::EaseOutCubic;
  inline constexpr Easing dismiss = Easing::EaseInOutCubic;

  // Continuous pointer tracking uses the same speed preference as timed animations.
  // At reduced motion, settle immediately instead of scheduling follow-up frames.
  inline float followFactor(float deltaMs, bool enabled, float speed) {
    return enabled ? 1.0F - std::exp(-std::clamp(deltaMs, 0.0F, 50.0F) * speed / 65.0F) : 1.0F;
  }

  // Two small, settling hops. Zero at both ends, including a reduced-motion snap.
  inline float launchLift(float progress) {
    const float t = std::clamp(progress, 0.0F, 1.0F);
    if (t >= 1.0F)
      return 0;
    const float hop = t < 0.6F ? t / 0.6F : (t - 0.6F) / 0.4F;
    return 4.0F * hop * (1.0F - hop) * (t < 0.6F ? 1.0F : 0.35F);
  }

} // namespace Motion
