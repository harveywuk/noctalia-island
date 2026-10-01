// GridView::setAutoColumnMinWidth drops columns so none is narrower than the minimum,
// never exceeding setColumns() and never dropping below one.

#include "tests/test_check.h"
#include "ui/controls/grid_view.h"

int main() {
  GridView grid;
  grid.setColumns(5);
  grid.setColumnGap(10.0F);

  // Without a minimum the configured count is used, capped by the item count.
  TEST_CHECK(grid.effectiveColumns(300.0F, 20) == 5);
  TEST_CHECK(grid.effectiveColumns(300.0F, 3) == 3);

  grid.setAutoColumnMinWidth(150.0F);
  TEST_CHECK(grid.effectiveColumns(810.0F, 20) == 5); // 5 * 150 + 4 * 10 = 790
  TEST_CHECK(grid.effectiveColumns(789.0F, 20) == 4);
  TEST_CHECK(grid.effectiveColumns(480.0F, 20) == 3);
  TEST_CHECK(grid.effectiveColumns(100.0F, 20) == 1);
  TEST_CHECK(grid.effectiveColumns(5000.0F, 20) == 5);
  TEST_CHECK(grid.effectiveColumns(0.0F, 20) == 5); // unknown width keeps the configured count

  // Padding is not available to columns.
  grid.setPadding(0.0F, 20.0F, 0.0F, 20.0F);
  TEST_CHECK(grid.effectiveColumns(810.0F, 20) == 4);
  return 0;
}
