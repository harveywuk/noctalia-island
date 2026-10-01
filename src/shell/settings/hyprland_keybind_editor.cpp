#include "shell/settings/hyprland_keybind_editor.h"

#include "compositors/hyprland/hyprland_keybinds.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/keybind_recorder.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace settings {
  std::unique_ptr<Node> makeHyprlandKeybindEditor(const SettingsContentContext& ctx) {
    using namespace compositors::hyprland;
    const auto tr = [](std::string_view key) { return i18n::tr("settings.hyprland-keybinds." + std::string(key)); };
    auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
    const auto label = [&](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
    };
    const auto button = [&](std::string text, std::function<void()> action) {
      return ui::button(
          {.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(action)}
      );
    };
    column->addChild(label(tr("hint")));
    auto& expanded = ctx.expandedGroupsByPage["hyprland-keybinds:entries"];
    auto actions = ui::row({.gap = 8 * ctx.scale});
    actions->addChild(
        button(tr("add"), [rules = ctx.config.shell.hyprlandKeybinds, expanded = &expanded, commit = ctx.setOverrides] {
          std::string name;
          for (int i = 1;; ++i) {
            name = std::format("shortcut-{:04}", i);
            if (std::ranges::none_of(rules, [&](const auto& r) { return r.name == name; }))
              break;
          }
          expanded->insert(name);
          commit({{{"shell", "hyprland_keybinds", name, "enabled"}, false}});
        })
    );
    actions->addChild(button(tr("refresh"), ctx.requestContentRebuild));
    column->addChild(std::move(actions));
    const auto live =
        ctx.hyprlandRuntime ? ctx.hyprlandRuntime->requestJson("j/binds").value_or(nlohmann::json{}) : nlohmann::json{};
    SettingsControlFactory factory(ctx);
    for (const auto& rule : ctx.config.shell.hyprlandKeybinds) {
      const auto path = [&](std::string key) {
        return std::vector<std::string>{"shell", "hyprland_keybinds", rule.name, std::move(key)};
      };
      auto* body = addSettingsGroupCard(
          {.parent = *column,
           .group = rule.name,
           .title = (rule.chord.empty() ? tr("new") : rule.chord) + " · " + tr("action-" + rule.action),
           .scale = ctx.scale,
           .expandedGroups = expanded}
      );
      static_cast<Flex*>(column->children().back().get())->setFillWidth(true);
      const auto toggle = [&](std::string key, bool value) {
        auto row = ui::row({.justify = FlexJustify::SpaceBetween, .fillWidth = true});
        row->addChild(label(tr(key)));
        row->addChild(factory.makeToggle(value, true, path(key)));
        body->addChild(std::move(row));
      };
      toggle("enabled", rule.enabled);
      body->addChild(
          ui::keybindRecorder(
              {.chord = compositorChord(rule.chord),
               .scale = ctx.scale,
               .enabled = ctx.canRecordHyprlandShortcut,
               .unsetPlaceholder = tr("record"),
               .recordingPlaceholder = tr("recording"),
               .onCommit = [commit = ctx.setOverride,
                            p = path("chord")](KeyChord chord) { commit(p, compositorChordString(chord)); },
               .configure =
                   [record = ctx.recordHyprlandShortcut](KeybindRecorder& recorder) {
                     recorder.setAllowSuper(true);
                     recorder.setRecordingStateCallback(record);
                   }}
          )
      );
      if (!ctx.canRecordHyprlandShortcut)
        body->addChild(label(tr("record-unavailable")));
      body->addChild(factory.makeText(rule.chord, tr("chord-placeholder"), path("chord"), 360));
      body->addChild(label(tr("action")));
      SelectSetting choices;
      for (const std::string action :
           {"workspace", "move_workspace", "focus_direction", "move_direction", "fullscreen", "floating", "close",
            "overview", "exec"})
        choices.options.push_back({action, tr("action-" + action)});
      choices.selectedValue = rule.action;
      std::vector<std::string> titles;
      std::size_t selected = 0;
      for (std::size_t i = 0; i < choices.options.size(); ++i) {
        titles.push_back(choices.options[i].label);
        if (choices.options[i].value == rule.action)
          selected = i;
      }
      body->addChild(
          ui::select(
              {.options = std::move(titles),
               .selectedIndex = selected,
               .fontSize = Style::fontSizeBody * ctx.scale,
               .width = 360 * ctx.scale,
               .onSelectionChanged = [options = choices.options, rule,
                                      commit = ctx.setOverrides](std::size_t index, std::string_view) {
                 if (index >= options.size())
                   return;
                 const auto action = options[index].value;
                 std::string target = rule.target;
                 if (action != rule.action) {
                   if (action == "focus_direction" || action == "move_direction") {
                     if (rule.action != "focus_direction" && rule.action != "move_direction")
                       target = "left";
                   } else if (action == "workspace" || action == "move_workspace") {
                     if (rule.action != "workspace" && rule.action != "move_workspace")
                       target = "1";
                   } else
                     target.clear();
                 }
                 commit(
                     {{{"shell", "hyprland_keybinds", rule.name, "action"}, action},
                      {{"shell", "hyprland_keybinds", rule.name, "target"}, target}}
                 );
               }}
          )
      );
      if (rule.action == "workspace" || rule.action == "move_workspace") {
        body->addChild(label(tr("workspace-hint")));
        body->addChild(factory.makeText(rule.target, "1", path("target"), 360));
      } else if (rule.action == "focus_direction" || rule.action == "move_direction") {
        SelectSetting directions;
        for (const std::string value : {"left", "right", "up", "down"})
          directions.options.push_back({value, tr(value)});
        directions.selectedValue = rule.target;
        body->addChild(factory.makeSelect(directions, path("target")));
      } else if (rule.action == "exec") {
        body->addChild(label(tr("command-hint")));
        body->addChild(factory.makeText(rule.target, tr("command-placeholder"), path("target"), 500));
      }
      toggle("repeating", rule.repeating);
      const auto problem = keybindProblem(rule, ctx.config.shell.hyprlandKeybinds, live);
      body->addChild(label(tr(problem.empty() ? (rule.enabled ? "active" : "disabled") : problem)));
      toggle("replace_existing", rule.replaceExisting);
      body->addChild(label(tr("replace-hint")));
      body->addChild(button(tr("restore"), [name = rule.name, clear = ctx.clearOverride] {
        clear({"shell", "hyprland_keybinds", name});
      }));
    }
    return column;
  }
} // namespace settings
