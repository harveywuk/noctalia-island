#include "tests/test_check.h"
#include "ui/motion.h"

#include <cmath>

int main() {
  const auto expand = Motion::islandExpand;
  const float settle = Motion::settleMs(expand);
  TEST_CHECK(Motion::spring(expand, 0).position == 0 && Motion::spring(expand, 0).velocity == 0);
  // It passes its target slightly and has come back within 0.1% by the settle time.
  float peak = 0;
  for (float t = 0; t <= settle; t += 1)
    peak = std::max(peak, Motion::spring(expand, t).position);
  TEST_CHECK(peak > 1.01F && peak < 1.05F);
  TEST_CHECK(std::abs(Motion::spring(expand, settle).position - 1) < 0.001F);
  // The collapse spring barely overshoots.
  float collapsePeak = 0;
  for (float t = 0; t <= Motion::settleMs(Motion::islandCollapse); t += 1)
    collapsePeak = std::max(collapsePeak, Motion::spring(Motion::islandCollapse, t).position);
  TEST_CHECK(collapsePeak > 1 && collapsePeak < 1.01F);
  // The reported velocity matches the position's slope, and an inherited velocity carries in.
  for (const float start : {0.0F, 3.0F, -2.0F})
    for (const auto spring : {expand, Motion::Spring{.responseMs = 400, .damping = 1}}) {
      TEST_CHECK(std::abs(Motion::spring(spring, 0, start).velocity - start) < 1e-4F);
      const float t = 80, dt = 0.1F;
      const float slope =
          (Motion::spring(spring, t + dt, start).position - Motion::spring(spring, t - dt, start).position)
          / (2 * dt / 1000);
      TEST_CHECK(std::abs(slope - Motion::spring(spring, t, start).velocity) < 0.05F);
    }
  return 0;
}
