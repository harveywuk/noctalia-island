#include "shell/settings/default_apps_editor.h"

#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "system/default_apps.h"
#include "system/terminal_launch.h"
#include "ui/builders.h"

#include <algorithm>
#include <chrono>

namespace settings {
  namespace {
    std::string tr(std::string_view key) { return i18n::tr("settings.default-apps." + std::string(key)); }
  } // namespace
  std::unique_ptr<Node> makeDefaultAppsEditor(const SettingsContentContext& ctx) {
    if (!ctx.defaultAppsEditor || !ctx.configService)
      return {};
    if (!*ctx.defaultAppsEditor)
      *ctx.defaultAppsEditor = std::make_shared<DefaultAppsEditorState>();
    const auto state = *ctx.defaultAppsEditor;
    auto* config = ctx.configService;
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = 12 * ctx.scale, .fillWidth = true});
    const auto label = [&](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 0});
    };
    const auto button = [&](std::string text, std::function<void()> action, bool enabled = true) {
      return ui::button(
          {.text = std::move(text),
           .fontSize = Style::fontSizeBody * ctx.scale,
           .enabled = enabled,
           .onClick = [action = std::move(action)] { DeferredCall::callLater(action); }}
      );
    };
    const auto refresh = [state, rebuild = ctx.requestContentRebuild] {
      rebuild();
      // GIO receives association-file changes through its existing main context.
      // Refresh once after those events; no extra polling while Settings is idle.
      state->refresh.start(std::chrono::milliseconds(350), rebuild);
    };
    column->addChild(label(tr("hint")));
    if (!state->message.empty())
      column->addChild(label(state->message));
    column->addChild(
        ui::row(
            {.gap = 8 * ctx.scale}, button(tr("refresh"), refresh),
            button(
                tr("undo"),
                [state, refresh] {
                  if (default_apps::undo(state->message))
                    state->message = tr("undone");
                  refresh();
                },
                default_apps::canUndo()
            )
        )
    );
    for (const auto& role : default_apps::roles()) {
      std::vector<default_apps::App> current;
      for (const auto& type : role.types)
        current.push_back(default_apps::current(type));
      const bool same = std::ranges::all_of(current, [&](const auto& app) { return app.id == current.front().id; });
      const auto title = same ? (current.front().name.empty() ? tr("unset") : current.front().name) : tr("mixed");
      auto row = ui::row(
          {.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .gap = 12 * ctx.scale, .fillWidth = true}
      );
      auto copy = ui::column({.align = FlexAlign::Start, .gap = 3 * ctx.scale, .flexGrow = 1.0F});
      copy->addChild(label(tr(role.id)));
      copy->addChild(
          ui::label(
              {.text = tr(role.id + "-hint"),
               .fontSize = Style::fontSizeCaption * ctx.scale,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .maxLines = 0}
          )
      );
      row->addChild(std::move(copy));
      row->addChild(button(
          title, [role, selected = same ? current.front().id : "", state, open = ctx.openSearchPickerPopup, refresh] {
            SearchPickerOpenRequest request;
            request.title = tr(role.id);
            request.placeholder = tr("search");
            request.emptyText = tr("no-apps");
            request.selectedValue = selected;
            for (const auto& app : default_apps::choices(role))
              request.options.push_back({app.id, app.name + " · " + app.id});
            request.onSelect = [state, role, refresh](const auto& id) {
              if (default_apps::set(role.id, id, state->message))
                state->message = tr("saved");
              refresh();
            };
            open(std::move(request));
          }
      ));
      column->addChild(std::move(row));
      column->addChild(ui::separator());
      if (!same)
        for (std::size_t i = 0; i < role.types.size(); ++i)
          column->addChild(label(role.types[i] + ": " + (current[i].name.empty() ? tr("unset") : current[i].name)));
    }
    column->addChild(label(tr("terminal-hint")));
    const auto terminals = terminal_launch::availableTerminals();
    const auto found =
        std::ranges::find(terminals, ctx.config.shell.preferredTerminal, &terminal_launch::TerminalApp::id);
    const auto terminalName = ctx.config.shell.preferredTerminal.empty() ? tr("automatic")
        : found != terminals.end()                                       ? found->name
                                   : tr("unavailable") + " " + ctx.config.shell.preferredTerminal;
    column->addChild(
        ui::row(
            {.align = FlexAlign::Center,
             .justify = FlexJustify::SpaceBetween,
             .gap = 12 * ctx.scale,
             .fillWidth = true},
            label(tr("terminal")),
            button(terminalName, [state, config, terminals, open = ctx.openSearchPickerPopup, refresh] {
              SearchPickerOpenRequest request;
              request.title = tr("terminal");
              request.placeholder = tr("search");
              request.emptyText = tr("no-terminals");
              request.selectedValue = config->config().shell.preferredTerminal;
              request.options.push_back({"", tr("automatic")});
              for (const auto& app : terminals)
                request.options.push_back({app.id, app.name + " · " + app.id});
              request.onSelect = [state, config, refresh](const auto& id) {
                if (default_apps::setTerminal(*config, id, state->message))
                  state->message = tr("terminal-saved");
                refresh();
              };
              open(std::move(request));
            })
        )
    );
    column->addChild(
        ui::row(
            {.gap = 8 * ctx.scale},
            button(
                tr("test-terminal"),
                [state, refresh] {
                  const auto command = terminal_launch::prepareOpen();
                  if (!command || !process::runAsync(*command))
                    state->message = tr("terminal-failed");
                  refresh();
                }
            ),
            button(
                tr("undo-terminal"),
                [state, config, refresh] {
                  if (default_apps::undoTerminal(*config, state->message))
                    state->message = tr("undone");
                  refresh();
                },
                default_apps::canUndoTerminal(*config)
            )
        )
    );
    return column;
  }
} // namespace settings
