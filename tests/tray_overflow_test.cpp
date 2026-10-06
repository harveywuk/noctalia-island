#include "shell/tray/tray_overflow.h"
#include "tests/test_check.h"

int main() {
  using tray::OverflowItem;
  using tray::selectInlineItems;
  const std::vector<OverflowItem> apps{{"b"}, {"c"}, {"d"}, {"a"}};
  const std::vector<std::string> previous{"b", "c", "d"};
  TEST_CHECK(selectInlineItems(apps, previous, 3) == previous);
  TEST_CHECK(selectInlineItems({{"a"}, {"b"}, {"c"}, {"d"}}, previous, 3) == previous);
  TEST_CHECK(selectInlineItems({{"a"}, {"c"}, {"d"}}, previous, 3) == std::vector<std::string>({"c", "d", "a"}));
  TEST_CHECK(
      selectInlineItems({{"a", true}, {"b"}, {"c"}, {"d"}}, previous, 3) == std::vector<std::string>({"a", "b", "c"})
  );
  TEST_CHECK(selectInlineItems({{"a", true}, {"b", true}, {"c", true}, {"d", true}}, previous, 3) == previous);
  TEST_CHECK(selectInlineItems(apps, previous, 1) == std::vector<std::string>({"b"}));
  TEST_CHECK(selectInlineItems(apps, previous, 0).empty());
  TEST_CHECK(selectInlineItems({}, previous, 3).empty());
  // An open drawer retains its apps until it closes, even when an inline app exits.
  const auto remaining = selectInlineItems({{"a"}, {"c"}, {"d"}}, previous, 3, true);
  TEST_CHECK(remaining == std::vector<std::string>({"c", "d"}));
  TEST_CHECK(selectInlineItems({{"a"}, {"c"}, {"d"}}, remaining, 3) == std::vector<std::string>({"c", "d", "a"}));
}
