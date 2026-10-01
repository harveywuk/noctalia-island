#include "shell/island/island_progress_outline.h"

#include <cassert>
#include <chrono>
#include <thread>

int main() {
  AnimationManager animations;
  {
    island::ProgressOutline outline;
    outline.setAnimationManager(&animations);
    outline.setGeometry(300, 64, 30, 1);
    const auto& fill = static_cast<const CountdownRingNode&>(*outline.children()[1]);
    outline.update(true, 0.75F, colorSpecFromRole(ColorRole::Primary));
    assert(outline.visible() && !animations.hasActive());
    assert(fill.width() == 300 && fill.height() == 64);
    assert(fill.style().cornerRadius == 30 && fill.style().progress == 0.75F);
    outline.setGeometry(360, 280, 42, 1.4F);
    assert(fill.width() == 360 && fill.height() == 280 && fill.style().thickness == 3.5F);
    outline.update(true, std::nullopt, colorSpecFromRole(ColorRole::Primary));
    assert(animations.hasActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    animations.tick(0);
    assert(fill.style().startOffset > 0 && fill.style().startOffset < 1);
    outline.update(true, 0.25F, colorSpecFromRole(ColorRole::Primary));
    assert(!animations.hasActive() && fill.style().startOffset == 0);
    outline.update(true, 0.25F, colorSpecFromRole(ColorRole::Primary), true);
    assert(animations.hasActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    animations.tick(0);
    assert(fill.opacity() < 1);
    outline.update(false, std::nullopt, colorSpecFromRole(ColorRole::Primary));
    assert(!outline.visible() && !animations.hasActive());
    MotionService::instance().setEnabled(false);
    outline.update(true, std::nullopt, colorSpecFromRole(ColorRole::Primary));
    assert(!animations.hasActive() && fill.opacity() == 1);
    MotionService::instance().setEnabled(true);
    outline.update(true, std::nullopt, colorSpecFromRole(ColorRole::Primary));
    assert(animations.hasActive());
  }
  assert(!animations.hasActive()); // Destruction cancels the looping segment.
  CountdownRingNode ordinary;
  assert(ordinary.style().cornerRadius < 0); // Existing callers remain circular.
}
