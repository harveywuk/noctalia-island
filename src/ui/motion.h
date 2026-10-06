#pragma once

#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

namespace Motion {

  inline constexpr float feedbackMs = 120;
  inline constexpr float pressMs = 70;
  inline constexpr float contentMs = 180;
  inline constexpr float revealMs = 240;
  inline constexpr float dismissMs = 180;
  inline constexpr float resizeMs = 280;
  inline constexpr Easing reveal = Easing::EaseOutCubic;
  inline constexpr Easing dismiss = Easing::EaseInOutCubic;

  // A damped spring, named the way SwiftUI's .spring(response:dampingFraction:) is:
  // responseMs is the period of the undamped oscillation (how quick it feels), and
  // damping is the fraction of critical damping (1 settles without overshoot, lower bounces).
  struct Spring {
    float responseMs;
    float damping;
  };

  // The Island grows with a small overshoot and settles, like the iPhone's Dynamic Island,
  // and collapses with a firmer spring that barely passes its target.
  inline constexpr Spring islandExpand{.responseMs = 450, .damping = 0.75F};
  inline constexpr Spring islandCollapse{.responseMs = 360, .damping = 0.86F};

  // Panels and their popovers open and close on the Island's springs, so the shell moves as one.
  inline constexpr Spring panelOpen = islandExpand;
  inline constexpr Spring panelClose = islandCollapse;
  inline constexpr Spring sheetOpen{.responseMs = 300, .damping = 1.0F};
  inline constexpr Spring widgetLift{.responseMs = 280, .damping = 0.9F};
  // A closing surface is destroyed when its spring ends, so it may stop once 1% of the travel is
  // left instead of waiting out the last fraction of a pixel.
  inline constexpr float closeTolerance = 0.01F;

  // How long a spring runs before its remaining motion is under tolerance (0.1% by default) of the travel.
  inline float settleMs(Spring spring, float tolerance = 0.001F) {
    const float omega = 2.0F * std::numbers::pi_v<float> / (spring.responseMs / 1000.0F);
    return std::log(1.0F / tolerance) / (std::min(spring.damping, 1.0F) * omega) * 1000.0F;
  }

  struct SpringSample {
    float position; // 0 at the start, 1 at the target
    float velocity; // travel per second
  };

  // The spring's position after timeMs, starting at rest at 0 (or moving at velocity, in
  // travel per second, when it takes over from an interrupted spring), heading for 1.
  inline SpringSample spring(Spring spring, float timeMs, float velocity = 0) {
    const float t = std::max(timeMs, 0.0F) / 1000.0F;
    const float omega = 2.0F * std::numbers::pi_v<float> / (spring.responseMs / 1000.0F);
    if (spring.damping >= 1.0F) {
      const float decay = std::exp(-omega * t);
      return {
          1.0F - decay * (1.0F + (omega - velocity) * t),
          decay * (velocity + omega * (omega - velocity) * t),
      };
    }
    const float a = spring.damping * omega;
    const float b = omega * std::sqrt(1.0F - spring.damping * spring.damping);
    const float c = (a - velocity) / b;
    const float decay = std::exp(-a * t);
    const float cosine = std::cos(b * t), sine = std::sin(b * t);
    return {
        1.0F - decay * (cosine + c * sine),
        decay * (velocity * cosine + (a * c + b) * sine),
    };
  }

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

  // Animates from `from` to `to` along a spring that starts at rest, and lands exactly on `to`.
  // The setter may see values slightly past `to` while the spring overshoots.
  inline AnimationManager::Id animateSpring(
      AnimationManager& animations, float from, float to, Spring spring, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr, float tolerance = 0.001F
  ) {
    const float durationMs = settleMs(spring, tolerance);
    return animations.animate(
        0, 1, durationMs, Easing::Linear,
        [from, to, spring, durationMs, setter = std::move(setter)](float t) {
          setter(t >= 1.0F ? to : from + (to - from) * Motion::spring(spring, t * durationMs).position);
        },
        std::move(onComplete), owner
    );
  }

} // namespace Motion
