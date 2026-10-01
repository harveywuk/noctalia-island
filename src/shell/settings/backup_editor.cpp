#include "shell/settings/backup_editor.h"

#include "compositors/hyprland/hyprland_displays.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/toggle.h"

#include <algorithm>
#include <ctime>
#include <format>

namespace settings {
  namespace {
    std::string tr(std::string_view key) { return i18n::tr("settings.backups." + std::string(key)); }
    std::string title(const config_backup::Info& backup) {
      const auto seconds = static_cast<std::time_t>(backup.created / 1000000);
      std::tm tm{};
      localtime_r(&seconds, &tm);
      char date[32]{};
      std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);
      return backup.name + " · " + date + (backup.automatic ? " · " + tr("automatic") : "");
    }
  } // namespace
  std::unique_ptr<Node> makeBackupEditor(const SettingsContentContext& ctx) {
    if (!ctx.backupEditor || !ctx.configService)
      return {};
    if (!*ctx.backupEditor)
      *ctx.backupEditor = std::make_shared<BackupEditorState>();
    const auto state = *ctx.backupEditor;
    auto* config = ctx.configService;
    auto* displays = ctx.displays;
    const auto rebuild = ctx.requestContentRebuild;
    const auto refresh = [state, config] {
      std::string error;
      state->backups = config->listBackups(error);
      if (!error.empty())
        state->message = error;
      state->loaded = true;
    };
    if (!state->loaded)
      refresh();
    if (displays)
      displays->changed = rebuild;
    if (state->displayPreview && (!displays || !displays->previewing())) {
      state->displayPreview = false;
      state->message = tr("reverted");
    }
    if (state->displayPreview && state->plan) {
      // Display tests can persist derived lock-screen widget geometry. Refresh
      // the visible review after that reload, before binding the Keep action to
      // its transaction. A later change is still rejected by restoreBackup.
      std::string error;
      auto reviewed = config->previewBackup(state->plan->backup.id, state->plan->sections, error);
      if (!reviewed || reviewed->config.shell.hyprlandDisplays != state->plan->config.shell.hyprlandDisplays) {
        displays->revert();
        state->displayPreview = false;
        state->plan.reset();
        state->message = error.empty() ? tr("display-review-changed") : error;
      } else if (reviewed->baseline != state->plan->baseline) {
        state->plan = std::make_shared<config_backup::Plan>(std::move(*reviewed));
        state->message = tr("review-updated");
      }
    }
    auto column = ui::column({.gap = 12 * ctx.scale, .fillWidth = true});
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
    column->addChild(label(tr("hint")));
    if (!state->message.empty())
      column->addChild(label(state->message));
    const auto preview = [state, config, rebuild](std::string id, std::vector<std::string> sections) {
      state->selected = std::move(id);
      state->selectedSections = std::move(sections);
      state->visibleChanges = 30;
      auto plan = config->previewBackup(state->selected, state->selectedSections, state->message);
      state->plan = plan ? std::make_shared<config_backup::Plan>(std::move(*plan)) : nullptr;
      rebuild();
    };
    if (state->plan) {
      const auto plan = state->plan;
      column->addChild(label(tr("review") + " " + title(plan->backup)));
      std::string scope;
      for (const auto& section : plan->sections) {
        if (!scope.empty())
          scope += ", ";
        scope += tr(section);
      }
      column->addChild(label(scope));
      const auto apply = [state, config, plan, refresh, rebuild] {
        std::string undo;
        const bool restored = config->restoreBackup(*plan, undo, state->message);
        if (restored) {
          state->undoId = std::move(undo);
          state->undoSections = plan->sections;
          state->message = tr("restored");
          state->displayPreview = false;
          state->plan.reset();
          refresh();
        }
        rebuild();
        return restored;
      };
      auto actions = ui::row({.gap = 8 * ctx.scale});
      if (state->displayPreview && displays && displays->previewing()) {
        column->addChild(label(tr("display-confirm")));
        actions->addChild(button(tr("keep"), [displays, apply] {
          displays->keep([apply](const auto&) { return apply(); });
        }));
      } else {
        column->addChild(label(plan->changes.empty() ? tr("no-changes") : tr("undo-hint")));
        actions->addChild(button(
            plan->displaysChanged ? tr("test-displays") : tr("restore"),
            [state, displays, plan, apply, rebuild] {
              if (!plan->displaysChanged) {
                apply();
                return;
              }
              if (!displays || displays->previewing()) {
                state->message = tr("display-unavailable");
                rebuild();
                return;
              }
              std::vector<HyprlandDisplayConfig> candidates;
              for (const auto& output : displays->displays()) {
                auto saved = std::ranges::find(
                    plan->config.shell.hyprlandDisplays, output.output, &HyprlandDisplayConfig::output
                );
                auto candidate = saved != plan->config.shell.hyprlandDisplays.end() ? *saved : HyprlandDisplayConfig{};
                candidate.output = output.output;
                candidates.push_back(std::move(candidate));
              }
              state->displayPreview = displays->preview(std::move(candidates));
              if (!state->displayPreview)
                state->message = displays->error().empty() ? tr("display-unavailable") : displays->error();
              rebuild();
            },
            !plan->changes.empty()
        ));
      }
      actions->addChild(button(tr("cancel"), [state, displays, rebuild] {
        if (state->displayPreview && displays)
          displays->revert();
        state->displayPreview = false;
        state->plan.reset();
        rebuild();
      }));
      column->addChild(std::move(actions));
      column->addChild(label(std::to_string(plan->changes.size()) + " " + tr("changes")));
      for (std::size_t i = 0; i < std::min(state->visibleChanges, plan->changes.size()); ++i) {
        const auto& row = plan->changes[i];
        column->addChild(label(row.path));
        // Wrap complete values so commands and file paths can be reviewed.
        column->addChild(label(tr("before") + " " + row.before));
        column->addChild(label(tr("after") + " " + row.after));
      }
      if (state->visibleChanges < plan->changes.size())
        column->addChild(button(tr("more"), [state, rebuild] {
          state->visibleChanges += 30;
          rebuild();
        }));
      return column;
    }
    column->addChild(
        ui::input(
            {.value = state->name,
             .placeholder = tr("name"),
             .fontSize = Style::fontSizeBody * ctx.scale,
             .width = 420 * ctx.scale,
             .onChange = [state](const auto& text) { state->name = text; }}
        )
    );
    auto actions = ui::row({.gap = 8 * ctx.scale});
    actions->addChild(button(tr("save"), [state, config, refresh, rebuild] {
      if (auto saved = config->createBackup(state->name, state->message)) {
        state->selected = saved->id;
        state->message = tr("saved");
        refresh();
      }
      rebuild();
    }));
    actions->addChild(button(tr("refresh"), [refresh, rebuild] {
      refresh();
      rebuild();
    }));
    if (!state->undoId.empty())
      actions->addChild(button(tr("undo"), [state, preview] { preview(state->undoId, state->undoSections); }));
    column->addChild(std::move(actions));
    column->addChild(label(tr("scope-hint")));
    if (state->backups.empty()) {
      column->addChild(label(tr("empty")));
      return column;
    }
    const auto selected = std::ranges::find(state->backups, state->selected, &config_backup::Info::id);
    column->addChild(button(
        selected == state->backups.end() ? tr("choose") : title(*selected),
        [state, open = ctx.openSearchPickerPopup, rebuild] {
          SearchPickerOpenRequest request;
          request.title = tr("choose");
          request.placeholder = tr("search");
          request.emptyText = tr("empty");
          request.selectedValue = state->selected;
          for (const auto& backup : state->backups)
            request.options.push_back({backup.id, title(backup)});
          request.onSelect = [state, rebuild](const auto& id) {
            state->selected = id;
            rebuild();
          };
          open(std::move(request));
        }
    ));
    column->addChild(label(tr("sections")));
    for (auto id : config_backup::sections) {
      auto row = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .fillWidth = true});
      row->addChild(label(tr(id)));
      row->addChild(
          ui::toggle(
              {.checked = std::ranges::find(state->selectedSections, id) != state->selectedSections.end(),
               .scale = ctx.scale,
               .onChange = [state, id](bool enabled) {
                 std::erase(state->selectedSections, id);
                 if (enabled)
                   state->selectedSections.emplace_back(id);
               }}
          )
      );
      column->addChild(std::move(row));
    }
    column->addChild(button(
        tr("preview"), [state, preview] { preview(state->selected, state->selectedSections); },
        selected != state->backups.end()
    ));
    return column;
  }
} // namespace settings
