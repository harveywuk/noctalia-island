#include "shell/desktop/desktop_widget_layouts.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widgets_history.h"

#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
  std::string output(const DesktopWidgetState& widget) {
    return widget.outputName.empty() ? "LEFT" : widget.outputName;
  }
} // namespace

int main() {
  DesktopWidgetsConfig state{
      .alwaysFullColor = true,
      .grid = {.visible = false, .cellSize = 24, .majorInterval = 8},
      .widgets = {
          {.id = "clock",
           .type = "clock",
           .outputName = "LEFT",
           .cx = 180,
           .cy = 200,
           .placementWidth = 1280,
           .placementHeight = 1024,
           .boxWidth = 220,
           .boxHeight = 180,
           .rotationRad = .35F,
           .flipX = true,
           .flipY = true,
           .enabled = false,
           .settings = {{"card_size", std::string("small")}, {"font_scale", 1.0}, {"show_seconds", false}}},
          // A stack owns members even when their standalone monitor is different.
          {.id = "photos",
           .type = "photos",
           .outputName = "RIGHT",
           .cx = 420,
           .cy = 200,
           .settings =
               {{"image_path", std::string("/album/my photo.png")}, {"entries", WidgetSettingStringMap{{"a", "b"}}}}},
          {.id = "stack",
           .type = "stack",
           .outputName = "LEFT",
           .cx = 600,
           .cy = 200,
           .settings =
               {{"members", std::vector<std::string>{"clock", "photos"}},
                {"auto_rotate", true},
                {"rotation_seconds", std::int64_t(45)}}},
          {.id = "other", .type = "tips", .outputName = "RIGHT"},
          {.id = "primary", .type = "calendar"},
          {.id = "offline",
           .type = "plugin.example",
           .outputName = "DISCONNECTED",
           .settings = {{"empty", WidgetSettingStringMap{}}}},
      },
  };
  const auto all = desktop_layouts::capture(state, "  Work  ", "", output);
  const auto left = desktop_layouts::capture(state, "Focus", "LEFT", output);
  const auto right = desktop_layouts::capture(state, "Media", "RIGHT", output);
  expect(all.name == "Work" && all.snapshot == state, "desktop capture includes disconnected outputs and all fields");
  expect(left.snapshot.widgets.size() == 4, "monitor capture includes stack children and default-output roots");
  expect(
      right.snapshot.widgets.size() == 1 && right.snapshot.widgets[0].id == "other",
      "foreign stack members do not leak into a monitor capture"
  );
  const std::vector layouts{all, left, right};
  const auto encoded = desktop_layouts::encode(layouts);
  expect(desktop_layouts::decode(encoded) == layouts, "every placement field and setting variant round trips exactly");
  expect(desktop_layouts::decode(desktop_layouts::encode({}))->empty(), "empty collection round trips");
  expect(
      desktop_layouts::cleanName("   ").empty() && desktop_layouts::cleanName("bad\nname").empty(),
      "blank and control-character names rejected"
  );
  expect(!desktop_layouts::decode("not json"), "corrupt data rejected");
  auto json = nlohmann::json::parse(encoded);
  json["version"] = 99;
  expect(!desktop_layouts::decode(json.dump()), "future schema rejected without mutation");
  json = nlohmann::json::parse(encoded);
  json["layouts"][0]["widgets"][0]["cx"] = nullptr;
  expect(!desktop_layouts::decode(json.dump()), "invalid geometry rejected");
  json = nlohmann::json::parse(encoded);
  json["layouts"][0]["widgets"][0]["settings"]["wrong"] = std::vector<int>{1, 2};
  expect(!desktop_layouts::decode(json.dump()), "unsupported collection value rejected");
  json = nlohmann::json::parse(encoded);
  json["layouts"][0]["widgets"][1]["id"] = "clock";
  expect(!desktop_layouts::decode(json.dump()), "duplicate widget IDs rejected");
  expect(!desktop_layouts::decode(desktop_layouts::encode({all, all})), "duplicate names rejected");

  auto applied = desktop_layouts::apply(state, left, "RIGHT", output);
  for (const auto& widget : state.widgets) {
    if (widget.id == "other")
      continue;
    const auto it = std::ranges::find(applied.widgets, widget.id, &DesktopWidgetState::id);
    expect(
        it != applied.widgets.end() && *it == widget,
        "monitor apply leaves all other roots and their owned children intact"
    );
  }
  expect(
      std::ranges::none_of(applied.widgets, [](const auto& widget) { return widget.id == "other"; }),
      "target roots replaced"
  );
  const auto stack = std::ranges::find_if(applied.widgets, [](const auto& widget) {
    return widget.type == "stack" && widget.outputName == "RIGHT";
  });
  expect(stack != applied.widgets.end() && stack->id != "stack", "colliding stack ID remapped");
  const auto members = desktop_stacks::cards(applied.widgets, stack->id);
  expect(
      members.size() == 2 && members[0].id != "clock" && members[1].id != "photos",
      "stack references remap with member IDs"
  );
  expect(
      members[1].settings == state.widgets[1].settings && members[0].boxWidth == 220 && members[0].rotationRad == .35F,
      "monitor apply preserves sources and transforms"
  );
  expect(
      applied.grid == state.grid && applied.alwaysFullColor == state.alwaysFullColor,
      "monitor apply does not change shared settings"
  );
  expect(desktop_layouts::apply(applied, all, "RIGHT", output) == state, "desktop apply restores exact state");
  auto empty = left;
  empty.snapshot.widgets.clear();
  const auto cleared = desktop_layouts::apply(state, empty, "LEFT", output);
  expect(
      cleared.widgets.size() == 2 && cleared.widgets[0].id == "other" && cleared.widgets[1].id == "offline",
      "empty monitor preset clears only its roots and owned children"
  );
  expect(desktop_layouts::apply(state, left, "", output) == state, "no target cannot replace primary accidentally");

  DesktopWidgetsHistory history;
  history.reset(state);
  history.record(state);
  expect(!history.canUndo() && !history.canRedo(), "no-op and canceled gestures create no history");
  auto moved = state;
  moved.widgets[2].cx += 120;
  moved.widgets[2].boxWidth = 300;
  history.record(moved);
  history.record(applied);
  expect(*history.undo() == moved && *history.undo() == state, "layout application and geometry each undo atomically");
  expect(!history.undo(), "undo cannot cross session start");
  expect(
      *history.redo() == moved && *history.redo() == applied && !history.redo(),
      "redo restores exact states including sources and stack links"
  );
  (void)history.undo();
  history.record(cleared);
  expect(!history.canRedo(), "new edit discards redo branch");
  history.reset(state);
  history.record(moved, "stack:width");
  auto larger = moved;
  larger.widgets[2].boxWidth = 350;
  history.record(larger, "stack:width");
  expect(*history.undo() == state && *history.redo() == larger, "continuous setting changes coalesce");
  history.breakGroup();
  history.record(moved, "stack:width");
  expect(*history.undo() == larger, "new interaction starts a separate edit for the same control");
  history.reset(state);
  history.record(moved, "width");
  history.record(state, "width");
  expect(!history.canUndo(), "setting returned to its original value leaves no empty undo step");
  history.reset(state);
  for (int i = 0; i < 150; ++i) {
    moved.widgets[2].cx = static_cast<float>(i);
    history.record(moved);
  }
  int count = 0;
  while (history.undo())
    ++count;
  expect(count == 100, "history is bounded to 100 edits");
  history.reset(state);
  expect(!history.canUndo() && !history.canRedo(), "new editor session clears both branches");
  std::println("PASS: desktop history, layout persistence, monitor ownership and ID remapping");
}
