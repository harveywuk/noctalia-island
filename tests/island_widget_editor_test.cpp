#include "config/schema/config_schema.h"
#include "shell/settings/island_widget_editor.h"
#include "shell/settings/widget_settings_registry.h"

#include <cassert>

int main() {
  using namespace settings;
  HoverWidgetGroups groups{{{"clock", "volume", "clock"}, {"test/plugin:widget"}, {}}};
  assert(moveHoverWidget(groups, 0, 2, 1, 0));
  assert((groups[0] == std::vector<std::string>{"clock", "volume"}));
  assert((groups[1] == std::vector<std::string>{"clock", "test/plugin:widget"}));
  assert(moveHoverWidget(groups, 1, 1, 2, 0));
  assert(groups[2].front() == "test/plugin:widget");
  assert(moveHoverWidget(groups, 0, 0, 0, 2));
  assert((groups[0] == std::vector<std::string>{"volume", "clock"}));
  assert(moveHoverWidget(groups, 0, 1, 0, 0));
  const auto before = groups;
  assert(!moveHoverWidget(groups, 0, 0, 0, 0));
  assert(!moveHoverWidget(groups, 0, 0, 0, 1));
  assert(!moveHoverWidget(groups, 3, 0, 0, 0));
  assert(!moveHoverWidget(groups, 0, 10, 0, 0));
  assert(!moveHoverWidget(groups, 0, 0, 2, 5));
  assert(groups == before);
  // Invalid paths cannot append to unrelated settings or overwrite another lane.
  assert(!hoverWidgetGroup({"bar", "hover_widgets"}));
  assert(!hoverWidgetGroup({"island", "hover_widgets", "extra"}));
  assert(hoverWidgetGroup({"island", "hover_widgets_right"}) == 2);
  // Presets only write hover layout choices and reference real built-in widgets.
  for (std::size_t preset = 0; preset < 3; ++preset) {
    auto layout = hoverLayoutPreset(preset);
    for (const auto& group : layout.groups)
      for (const auto& widget : group)
        assert(isBuiltInWidgetType(widget));
    const auto changes = hoverLayoutOverrides(layout);
    assert(changes.size() == 10);
    for (const auto& [path, value] : changes) {
      assert(path.size() == 2 && path[0] == "island" && path[1].starts_with("hover_"));
      assert(noctalia::config::schema::isKnownConfigPath(path));
    }
  }
  // The undo snapshot includes all user choices, including duplicate and plugin entries.
  IslandConfig cfg;
  cfg.hoverWidgets = {"my_button", "test/plugin:widget", "my_button"};
  cfg.hoverWidgetsRight = {"weather"};
  cfg.hoverShowCalendar = false;
  const auto saved = hoverLayout(cfg);
  assert(saved.groups[0] == cfg.hoverWidgets && !saved.sections[1]);
  const auto restored = hoverLayoutOverrides(saved);
  assert(std::get<std::vector<std::string>>(restored[0].second) == cfg.hoverWidgets);
  assert(std::get<bool>(restored[4].second) == false);
}
