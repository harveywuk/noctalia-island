#include "shell/settings/hyprland_workspace_editor.h"

#include "compositors/hyprland/hyprland_displays.h"
#include "i18n/i18n.h"
#include "shell/bar/widgets/workspace_preferences.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/style.h"

namespace settings {
  std::unique_ptr<Node> makeHyprlandWorkspaceEditor(const SettingsContentContext& ctx) {
    auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
    const auto tr = [](std::string_view key) { return i18n::tr("settings.hyprland-workspaces." + std::string(key)); };
    const auto label = [&](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
    };
    const auto button = [&](std::string text, std::function<void()> action) {
      return ui::button(
          {.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(action)}
      );
    };
    column->addChild(label(tr("hint")));
    auto& expanded = ctx.expandedGroupsByPage["hyprland-workspaces:entries"];
    auto target = std::make_shared<std::string>();
    for (int i = 1; i <= 1000; ++i)
      if (std::ranges::none_of(ctx.config.shell.hyprlandWorkspaces, [&](const auto& r) {
            return r.workspace == std::to_string(i);
          })) {
        *target = std::to_string(i);
        break;
      }
    column->addChild(label(tr("target-hint")));
    auto add = ui::row({.gap = 8 * ctx.scale});
    add->addChild(
        ui::input(
            {.value = *target,
             .placeholder = tr("target-placeholder"),
             .width = 280 * ctx.scale,
             .onChange = [target](const std::string& value) { *target = value; }}
        )
    );
    auto feedback = std::make_shared<Label*>(nullptr);
    add->addChild(button(
        tr("add"),
        [target, feedback, rules = ctx.config.shell.hyprlandWorkspaces, expanded = &expanded, commit = ctx.setOverrides,
         tr] {
          if (!workspace_preferences::validTarget(*target)) {
            (*feedback)->setText(tr("invalid-target"));
            return;
          }
          if (std::ranges::any_of(rules, [&](const auto& r) { return r.workspace == *target; })) {
            (*feedback)->setText(tr("duplicate"));
            return;
          }
          expanded->insert(*target);
          commit({{{"shell", "hyprland_workspaces", *target, "enabled"}, false}});
        }
    ));
    column->addChild(std::move(add));
    auto status = label("");
    *feedback = status.get();
    column->addChild(std::move(status));
    SettingsControlFactory factory(ctx);
    for (const auto& rule : ctx.config.shell.hyprlandWorkspaces) {
      const auto path = [&](std::string key) {
        return std::vector<std::string>{"shell", "hyprland_workspaces", rule.workspace, std::move(key)};
      };
      auto* body = addSettingsGroupCard(
          {.parent = *column,
           .group = rule.workspace,
           .title = rule.workspace + (rule.label.empty() ? "" : " · " + rule.label),
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
      if (!workspace_preferences::validTarget(rule.workspace))
        body->addChild(label(tr("invalid-target")));
      toggle("enabled", rule.enabled);
      body->addChild(label(tr("label")));
      body->addChild(factory.makeText(rule.label, tr("label-placeholder"), path("label"), 360));
      body->addChild(label(tr("icon")));
      body->addChild(factory.makeText(rule.icon, tr("icon-placeholder"), path("icon"), 360));
      body->addChild(label(tr("monitor")));
      SelectSetting monitors;
      monitors.options.push_back({"", tr("automatic")});
      if (ctx.displays)
        for (const auto& display : ctx.displays->displays())
          monitors.options.push_back({display.output, display.output + " · " + display.description});
      if (!rule.monitor.empty()
          && std::ranges::none_of(monitors.options, [&](const auto& o) { return o.value == rule.monitor; }))
        monitors.options.push_back({rule.monitor, rule.monitor + " · " + tr("disconnected")});
      monitors.selectedValue = rule.monitor;
      body->addChild(factory.makeSelect(monitors, path("monitor")));
      toggle("persistent", rule.persistent);
      body->addChild(label(tr("monitor-hint")));
      body->addChild(button(tr("restore"), [key = rule.workspace, clear = ctx.clearOverride] {
        clear({"shell", "hyprland_workspaces", key});
      }));
    }
    return column;
  }
} // namespace settings
