#include "shell/desktop/desktop_widget_actions.h"
#include "shell/desktop/desktop_widget_setup.h"
#include "shell/desktop/editor/desktop_widgets_history.h"

#include <algorithm>
#include <cstdlib>
#include <print>
#include <unordered_set>

namespace {
  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "FAIL: {}", message);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using desktop_widgets::applyQuickAction;
  using desktop_widgets::QuickAction;
  DesktopWidgetsConfig initial{
      .widgets = {
          {.id = "clock", .type = "clock", .settings = {{"timezone", std::string("Europe/London")}}},
          {.id = "notes", .type = "notes", .settings = {{"file_path", std::string("/notes.txt")}}},
          {.id = "stack",
           .type = "stack",
           .cx = 300,
           .cy = 300,
           .boxWidth = 500,
           .boxHeight = 400,
           .settings = {{"members", std::vector<std::string>{"clock", "notes"}}, {"card_size", std::string("medium")}}},
          {.id = "label", .type = "label"},
      }
  };
  auto state = initial;
  expect(!applyQuickAction(state, "gone", QuickAction::Remove), "stale menu target is ignored");
  expect(!applyQuickAction(state, "label", QuickAction::Large), "non-card widgets do not acquire size presets");
  expect(!applyQuickAction(state, "clock", QuickAction::SmartRotate), "Smart Rotate applies only to stacks");
  expect(state == initial, "unsupported actions leave settings intact");
  DesktopWidgetsHistory history;
  history.reset(state);
  expect(applyQuickAction(state, "stack", QuickAction::Small), "resize changes the preset");
  expect(state.widgets[2].boxWidth == 0 && state.widgets[2].boxHeight == 0, "preset clears custom dimensions");
  history.record(state);
  expect(!applyQuickAction(state, "stack", QuickAction::Small), "selecting the same size is a no-op");
  expect(applyQuickAction(state, "stack", QuickAction::SmartRotate), "Smart Rotate can be toggled");
  expect(std::get<bool>(state.widgets[2].settings.at("smart_rotate")), "Smart Rotate was enabled");
  history.record(state);
  expect(applyQuickAction(state, "stack", QuickAction::Duplicate), "stack duplicates");
  expect(state.widgets.size() == 7, "stack duplicate includes independent members");
  std::unordered_set<std::string> ids;
  for (const auto& widget : state.widgets)
    expect(ids.insert(widget.id).second, "copies have unique IDs");
  const auto copied = desktop_stacks::cards(state.widgets, state.widgets[4].id);
  expect(
      copied.size() == 2 && copied[0].id != "clock" && copied[1].id != "notes", "copied stack links only copied members"
  );
  expect(
      copied[0].settings == initial.widgets[0].settings && copied[1].settings == initial.widgets[1].settings,
      "sources survive duplication"
  );
  history.record(state);
  const auto duplicated = state;
  expect(applyQuickAction(state, "stack", QuickAction::Remove), "stack removal succeeds");
  expect(
      std::ranges::find(state.widgets, "clock", &DesktopWidgetState::id) != state.widgets.end(),
      "removing stack releases rather than deletes its cards"
  );
  history.record(state);
  auto editorHistory = history;
  expect(editorHistory.matches(state), "history can be transferred with its current layout");
  expect(*editorHistory.undo() == duplicated, "editor can undo a desktop quick action");
  history = editorHistory;
  expect(*history.redo() == state, "desktop can redo after returning from the editor");
  expect(history.matches(state), "redo and its snapshot remain synchronized");
  state.widgets.back().cx = 42;
  history.replaceCurrent(state);
  expect(history.matches(state) && *history.undo() == duplicated, "placement normalization preserves undo");
  std::println("PASS: quick actions, independent stack copies, released members and shared history");
}
