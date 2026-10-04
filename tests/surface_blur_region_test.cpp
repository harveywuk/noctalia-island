#include "test_check.h"
#include "wayland/surface.h"

#include <cstddef>
#include <vector>

namespace {

  bool contains(const InputRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  }

  bool covered(const std::vector<InputRect>& region, int x, int y) {
    for (const auto& r : region)
      if (contains(r, x, y))
        return true;
    return false;
  }

  // Every pixel of `original` is still inside `coarse`.
  bool coversAll(const std::vector<InputRect>& coarse, const std::vector<InputRect>& original) {
    for (const auto& r : original)
      for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x)
          if (!covered(coarse, x, y))
            return false;
    return true;
  }

  // Pixels `coarse` adds outside `original`.
  long long excess(const std::vector<InputRect>& coarse, const std::vector<InputRect>& original, int w, int h) {
    long long count = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (covered(coarse, x, y) && !covered(original, x, y))
          ++count;
    return count;
  }

} // namespace

int main() {
  TEST_CHECK(!Surface::regionIntersectsBounds({InputRect{8, -39, 3056, 35}}, 3072, 49));

  TEST_CHECK(Surface::regionIntersectsBounds({InputRect{8, -34, 3056, 35}}, 3072, 49));

  // An expanded Island capsule: a 400x80 pill traced in one-pixel strips (80 of them).
  const auto capsule = Surface::tessellateRoundedRect(10, 8, 400, 80, 40.0F);
  TEST_CHECK(capsule.size() > 64);

  // Hyprglass's patched limit: the trace stays within a couple of pixels of the curve, so a glow
  // drawn just outside the capsule stays out of the glass.
  const auto fine = Surface::coarsenRegion(capsule, 64);
  TEST_CHECK(fine.size() <= 64);
  TEST_CHECK(coversAll(fine, capsule));
  TEST_CHECK(excess(fine, capsule, 440, 100) < 100);

  // The stock limit still covers the shape and still beats a bounding box by a wide margin
  // (each of the box's corners alone adds about 340 pixels).
  const auto coarse = Surface::coarsenRegion(capsule, 16);
  TEST_CHECK(coarse.size() <= 16);
  TEST_CHECK(coversAll(coarse, capsule));
  TEST_CHECK(excess(coarse, capsule, 440, 100) < 400);

  // A capsule and a split bubble beside it stay apart: nothing glasses the gap between them.
  auto pair = capsule;
  const auto bubble = Surface::tessellateRoundedRect(420, 8, 80, 80, 40.0F);
  pair.insert(pair.end(), bubble.begin(), bubble.end());
  const auto split = Surface::coarsenRegion(pair, 64);
  TEST_CHECK(split.size() <= 64);
  TEST_CHECK(coversAll(split, pair));
  for (int y = 8; y < 88; ++y)
    for (int x = 411; x < 419; ++x)
      TEST_CHECK(!covered(split, x, y));

  // A region that already fits comes back as it was.
  const std::vector<InputRect> plain{InputRect{0, 0, 10, 10}, InputRect{20, 0, 10, 10}};
  const auto same = Surface::coarsenRegion(plain, 16);
  TEST_CHECK(same.size() == 2);
  TEST_CHECK(coversAll(same, plain));
  TEST_CHECK(excess(same, plain, 40, 20) == 0);

  // More separate shapes than the limit still fit, and still cover everything.
  std::vector<InputRect> dots;
  for (int i = 0; i < 40; ++i)
    dots.push_back(InputRect{i * 4, 0, 2, 2});
  const auto folded = Surface::coarsenRegion(dots, 16);
  TEST_CHECK(folded.size() <= 16);
  TEST_CHECK(coversAll(folded, dots));

  TEST_CHECK(Surface::coarsenRegion({}, 16).empty());

  return 0;
}
