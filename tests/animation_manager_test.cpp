#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "ui/motion.h"

#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001F; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0F);
    motion.setEnabled(true);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();

  {
    const float half = Motion::followFactor(8.0F, true, 1.0F);
    const float full = Motion::followFactor(16.0F, true, 1.0F);
    ok &= check(
        nearlyEqual(full, 1.0F - (1.0F - half) * (1.0F - half)), "pointer tracking changes speed with refresh rate"
    );
    ok &= check(
        nearlyEqual(full, Motion::followFactor(8.0F, true, 2.0F)), "pointer tracking ignores the shell animation speed"
    );
    ok &= check(
        nearlyEqual(Motion::followFactor(8.0F, false, 0.5F), 1.0F),
        "reduced-motion pointer tracking must settle immediately"
    );
    ok &= check(
        nearlyEqual(Motion::launchLift(0), 0) && nearlyEqual(Motion::launchLift(1), 0),
        "launch feedback must start and finish at rest"
    );
    for (int i = 0; i <= 100; ++i) {
      const float lift = Motion::launchLift(static_cast<float>(i) / 100.0F);
      ok &= check(lift >= 0 && lift <= 1, "launch feedback escaped its reserved space");
    }
  }

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 200.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0F), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0F);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0F), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0F);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0F;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0F, 0.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0F), "timer animation snapped when motion was disabled");

    manager.tick(0.0F);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  resetMotion();
  return ok ? 0 : 1;
}
