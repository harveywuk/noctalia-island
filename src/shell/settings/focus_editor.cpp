#include "shell/settings/focus_editor.h"

#include "config/config_service.h"
#include "i18n/i18n.h"
#include "notification/focus_state.h"
#include "shell/settings/settings_content_common.h"
#include "ui/builders.h"
#include "util/string_utils.h"

#include <array>
#include <charconv>
#include <format>
#include <sstream>

namespace settings {
  namespace {
    constexpr std::array<const char*, 7> kDays{"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

    std::optional<int> minute(std::string_view value) {
      if (value.size() != 5 || value[2] != ':')
        return std::nullopt;
      int hour = 0, minute = 0;
      auto h = std::from_chars(value.data(), value.data() + 2, hour);
      auto m = std::from_chars(value.data() + 3, value.data() + 5, minute);
      if (h.ec != std::errc{}
          || m.ec != std::errc{}
          || h.ptr != value.data() + 2
          || m.ptr != value.data() + 5
          || hour < 0
          || hour > 23
          || minute < 0
          || minute > 59)
        return std::nullopt;
      return hour * 60 + minute;
    }

    struct Editor {
      ConfigService* config = nullptr;
      FocusConfig initial;
      std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)> commit;
      std::size_t* selected = nullptr;
      std::size_t editing = 0;
      int dayMask = 31;
      Input *apps = nullptr, *start = nullptr, *end = nullptr;
      Toggle *schedule = nullptr, *critical = nullptr;
      Label* feedback = nullptr;
      std::array<Button*, 7> days{};

      void load(std::size_t index) {
        if (index >= focus::kProfiles.size())
          return;
        editing = index;
        if (selected)
          *selected = index;
        const auto* profile =
            focus::profile(config ? config->config().notification.focus : initial, focus::kProfiles[index]);
        std::string names;
        for (const auto& app : profile->allowedApps) {
          if (!names.empty())
            names += ", ";
          names += app;
        }
        apps->setValue(names);
        start->setValue(std::format("{:02}:{:02}", profile->startMinute / 60, profile->startMinute % 60));
        end->setValue(std::format("{:02}:{:02}", profile->endMinute / 60, profile->endMinute % 60));
        start->setInvalid(false);
        end->setInvalid(false);
        schedule->setCheckedImmediate(profile->scheduleEnabled);
        critical->setCheckedImmediate(profile->allowCritical);
        dayMask = profile->days;
        for (std::size_t i = 0; i < days.size(); ++i)
          days[i]->setVariant((dayMask & (1 << i)) ? ButtonVariant::Primary : ButtonVariant::Default);
        feedback->setText("");
      }

      void save() {
        const auto from = minute(start->value()), until = minute(end->value());
        start->setInvalid(!from);
        end->setInvalid(!until);
        if (!from || !until || (schedule->checked() && (*from == *until || dayMask == 0))) {
          feedback->setText(i18n::tr("utilities.focus.invalid-schedule"));
          return;
        }
        std::vector<std::string> names;
        std::istringstream stream(apps->value());
        for (std::string app; std::getline(stream, app, ',');) {
          app = StringUtils::trim(app);
          if (!app.empty() && !std::ranges::contains(names, app))
            names.push_back(std::move(app));
        }
        const auto field = [this](const char* key) {
          return std::vector<std::string>{"notification", "focus", std::string(focus::kProfiles[editing]), key};
        };
        feedback->setText("");
        if (commit)
          commit(
              {{field("allowed_apps"), names},
               {field("allow_critical"), critical->checked()},
               {field("schedule_enabled"), schedule->checked()},
               {field("start_minute"), static_cast<std::int64_t>(*from)},
               {field("end_minute"), static_cast<std::int64_t>(*until)},
               {field("days"), static_cast<std::int64_t>(dayMask)}}
          );
      }
    };
  } // namespace

  std::unique_ptr<Node> makeFocusEditor(const SettingsContentContext& ctx) {
    const auto scale = ctx.scale;
    auto state = std::make_shared<Editor>();
    state->config = ctx.configService;
    state->initial = ctx.config.notification.focus;
    state->commit = ctx.setOverrides;
    state->selected = ctx.focusProfile;
    const auto editing = ctx.focusProfile && *ctx.focusProfile < focus::kProfiles.size() ? *ctx.focusProfile : 0;
    const auto tr = [](std::string_view key) { return i18n::tr("utilities.focus." + std::string(key)); };
    const auto label = [&](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * scale, .flexGrow = 1.0F});
    };
    auto editor = ui::column({.align = FlexAlign::Stretch, .gap = 12 * scale, .fillWidth = true});
    std::vector<std::string> names;
    for (auto id : focus::kProfiles)
      names.push_back(tr(id));
    editor->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = 12 * scale}, label(tr("configure")),
            ui::select(
                {.options = names,
                 .selectedIndex = editing,
                 .fontSize = Style::fontSizeBody * scale,
                 .width = 160 * scale,
                 .onSelectionChanged = [state](std::size_t index, std::string_view) { state->load(index); }}
            )
        )
    );
    editor->addChild(label(tr("allowed-apps")));
    editor->addChild(
        ui::input(
            {.out = &state->apps,
             .placeholder = tr("apps-example"),
             .fontSize = Style::fontSizeBody * scale,
             .configure = [](Input& input) { input.inputArea()->setTabFocusKey("settings.focus.apps"); }}
        )
    );
    editor->addChild(makeSettingSubtitleLabel(tr("apps-detail"), scale));
    const auto toggle = [&](std::string_view key, Toggle** out) {
      editor->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = 12 * scale}, label(tr(key)),
              ui::toggle({.out = out, .scale = scale, .configure = [key](Toggle& value) {
                            value.setTabFocusKey("settings.focus." + std::string(key));
                          }})
          )
      );
    };
    toggle("allow-critical", &state->critical);
    toggle("schedule", &state->schedule);
    editor->addChild(
        ui::row(
            {.align = FlexAlign::Center, .gap = 12 * scale}, label(tr("from")),
            ui::input(
                {.out = &state->start,
                 .placeholder = "09:00",
                 .fontSize = Style::fontSizeBody * scale,
                 .width = 85 * scale,
                 .configure = [](Input& input) { input.inputArea()->setTabFocusKey("settings.focus.from"); }}
            ),
            label(tr("until")),
            ui::input(
                {.out = &state->end,
                 .placeholder = "17:00",
                 .fontSize = Style::fontSizeBody * scale,
                 .width = 85 * scale,
                 .configure = [](Input& input) { input.inputArea()->setTabFocusKey("settings.focus.until"); }}
            )
        )
    );
    auto days = ui::row({.gap = 4 * scale});
    for (std::size_t i = 0; i < kDays.size(); ++i)
      days->addChild(
          ui::button(
              {.out = &state->days[i],
               .text = tr(std::string("days.") + kDays[i]),
               .fontSize = Style::fontSizeCaption * scale,
               .paddingH = 6 * scale,
               .flexGrow = 1.0F,
               .onClick =
                   [state, i] {
                     state->dayMask ^= 1 << i;
                     state->days[i]->setVariant(
                         (state->dayMask & (1 << i)) ? ButtonVariant::Primary : ButtonVariant::Default
                     );
                   },
               .configure = [i](
                                Button& button
                            ) { button.inputArea()->setTabFocusKey("settings.focus." + std::string(kDays[i])); }}
          )
      );
    editor->addChild(std::move(days));
    editor->addChild(makeSettingSubtitleLabel(tr("schedule-detail"), scale));
    editor->addChild(
        ui::button(
            {.text = tr("save"),
             .fontSize = Style::fontSizeBody * scale,
             .variant = ButtonVariant::Primary,
             .onClick = [state] { state->save(); },
             .configure = [](Button& button) { button.inputArea()->setTabFocusKey("settings.focus.save"); }}
        )
    );
    editor->addChild(ui::label({.out = &state->feedback, .fontSize = Style::fontSizeCaption * scale, .maxLines = 3}));
    state->load(editing);
    return editor;
  }
} // namespace settings
