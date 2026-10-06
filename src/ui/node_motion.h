#pragma once

#include "render/animation/motion_service.h"
#include "render/scene/node.h"
#include "ui/motion.h"

namespace Motion {

  // Use on a presentation node so the measured layout stays fixed.
  inline void revealNode(Node& node) {
    auto* animations = node.animationManager();
    if (!animations || !MotionService::instance().enabled())
      return;
    node.setOpacity(0);
    node.setScale(.985F);
    animateSpring(
        *animations, 0, 1, sheetOpen,
        [&node](float progress) {
          node.setOpacity(std::clamp(progress * 1.5F, 0.0F, 1.0F));
          node.setScale(.985F + .015F * progress);
        },
        {}, &node
    );
  }

  inline void fadeNode(Node& node, float target, float duration, std::function<void()> complete = {}) {
    auto* animations = node.animationManager();
    if (!animations) {
      node.setOpacity(target);
      if (complete)
        complete();
      return;
    }
    animations->animate(
        node.opacity(), target, duration, reveal, [&node](float value) { node.setOpacity(value); }, std::move(complete),
        &node
    );
    node.markPaintDirty();
  }

  // The wrapper scales only its artwork, leaving placement geometry and capture stable.
  inline void liftNode(Node& node, bool lifted) {
    auto* animations = node.animationManager();
    const float target = lifted && MotionService::instance().enabled() ? 1.025F : 1.0F;
    if (animations)
      animations->cancelForOwner(&node);
    if (!animations || !MotionService::instance().enabled()) {
      node.setScale(1);
      return;
    }
    if (node.scale() == target)
      return;
    animateSpring(
        *animations, node.scale(), target, widgetLift,
        [&node, lifted](float value) { node.setScale(lifted && !MotionService::instance().enabled() ? 1.0F : value); },
        {}, &node
    );
  }

} // namespace Motion
