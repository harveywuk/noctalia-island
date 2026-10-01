#include "shell/settings/startup_apps_editor.h"

#include "i18n/i18n.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "system/desktop_entry.h"
#include "system/startup_apps.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/stepper.h"
#include "ui/controls/toggle.h"
#include "ui/style.h"

#include <algorithm>
#include <format>

namespace settings {
  std::unique_ptr<Node> makeStartupAppsEditor(const SettingsContentContext& ctx) {
    const auto tr = [](std::string_view key) { return i18n::tr("settings.startup-apps." + std::string(key)); };
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
    column->addChild(label(tr("scope-hint")));
    auto& expanded = ctx.expandedGroupsByPage["startup-apps:entries"];
    const auto add = [entries = ctx.config.shell.session.startupApps, expanded = &expanded,
                      commit = ctx.setOverrides](std::string kind, std::string desktopId) {
      std::string id;
      for (int i = 1;; ++i) {
        id = std::format("startup-{:04}", i);
        if (std::ranges::none_of(entries, [&](const auto& e) { return e.id == id; }))
          break;
      }
      expanded->insert(id);
      commit(
          {{{"shell", "session", "startup_apps", id, "enabled"}, false},
           {{"shell", "session", "startup_apps", id, "kind"}, std::move(kind)},
           {{"shell", "session", "startup_apps", id, "desktop_id"}, std::move(desktopId)}}
      );
    };
    const auto pick = [open = ctx.openSearchPickerPopup,
                       tr](const std::string& current, std::function<void(const std::string&)> select) {
      SearchPickerOpenRequest request;
      request.title = tr("pick-app");
      request.placeholder = tr("search-app");
      request.emptyText = tr("no-apps");
      request.selectedValue = current;
      for (const auto& app : desktopEntries())
        if (!app.hidden && !app.noDisplay)
          request.options.push_back({app.id, app.name + " · " + app.id});
      request.onSelect = std::move(select);
      if (open)
        open(std::move(request));
    };
    auto actions = ui::row({.gap = 8 * ctx.scale});
    actions->addChild(button(tr("add-app"), [pick, add] {
      pick("", [add](const auto& id) {
        if (!id.empty())
          add("app", id);
      });
    }));
    actions->addChild(button(tr("add-command"), [add] { add("command", ""); }));
    column->addChild(std::move(actions));
    SettingsControlFactory factory(ctx);
    for (const auto& entry : ctx.config.shell.session.startupApps) {
      const auto path = [&](std::string key) {
        return std::vector<std::string>{"shell", "session", "startup_apps", entry.id, std::move(key)};
      };
      std::string appName = entry.desktopId;
      if (entry.kind == "app")
        for (const auto& app : desktopEntries())
          if (app.id == entry.desktopId) {
            appName = app.name;
            break;
          }
      const auto title = !entry.label.empty()                 ? entry.label
          : entry.kind == "app" && !appName.empty()           ? appName
          : entry.kind == "command" && !entry.command.empty() ? entry.command.substr(0, 72)
                                                              : tr("new");
      auto* body = addSettingsGroupCard(
          {.parent = *column, .group = entry.id, .title = title, .scale = ctx.scale, .expandedGroups = expanded}
      );
      static_cast<Flex*>(column->children().back().get())->setFillWidth(true);
      const auto issue = StartupApps::problem(entry);
      auto enabled = ui::row({.justify = FlexJustify::SpaceBetween, .fillWidth = true});
      enabled->addChild(label(tr("enabled")));
      enabled->addChild(factory.makeToggle(entry.enabled, issue.empty() || entry.enabled, path("enabled")));
      body->addChild(std::move(enabled));
      body->addChild(label(tr("label")));
      body->addChild(factory.makeText(entry.label, tr("label-placeholder"), path("label"), 420));
      if (entry.kind == "app") {
        body->addChild(label(tr("app")));
        body->addChild(button(
            appName.empty() ? tr("pick-app") : appName,
            [pick, current = entry.desktopId, commit = ctx.setOverride, p = path("desktop_id")] {
              pick(current, [commit, p](const auto& id) {
                if (!id.empty())
                  commit(p, id);
              });
            }
        ));
      } else if (entry.kind == "command") {
        body->addChild(label(tr("command-hint")));
        body->addChild(factory.makeText(entry.command, tr("command-placeholder"), path("command"), 500));
      }
      body->addChild(label(tr("delay")));
      body->addChild(factory.makeStepper(
          {.value = entry.delaySeconds, .minValue = 0, .maxValue = 300, .step = 1, .valueSuffix = "s"},
          path("delay_seconds")
      ));
      body->addChild(label(tr(issue.empty() ? (entry.enabled ? "next-login" : "disabled") : issue)));
      body->addChild(label(tr("restore-hint")));
      body->addChild(button(tr("restore"), [id = entry.id, clear = ctx.clearOverride] {
        clear({"shell", "session", "startup_apps", id});
      }));
    }
    return column;
  }
} // namespace settings
