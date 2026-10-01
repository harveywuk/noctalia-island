#include "shell/settings/hyprland_display_editor.h"

#include "compositors/hyprland/hyprland_displays.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "shell/settings/display_layout_canvas.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace settings {
  namespace {
    std::string tr(std::string_view key) { return i18n::tr("settings.displays." + std::string(key)); }
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides(const HyprlandDisplayConfig& c) {
      using Change = std::pair<std::vector<std::string>, ConfigOverrideValue>;
      std::vector<Change> result;
      const auto add = [&](std::string key, ConfigOverrideValue value) {
        result.emplace_back(
            std::vector<std::string>{"shell", "hyprland_displays", c.output, std::move(key)}, std::move(value)
        );
      };
      add("managed", c.managed);
      add("mode", c.mode);
      add("transform", std::int64_t(c.transform));
      add("scale", double(c.scale));
      add("position_managed", c.positionManaged);
      add("x", std::int64_t(c.x));
      add("y", std::int64_t(c.y));
      add("vrr", std::int64_t(c.vrr));
      add("color_mode", c.colorMode);
      add("bit_depth", std::int64_t(c.bitDepth));
      add("sdr_brightness", double(c.sdrBrightness));
      add("sdr_saturation", double(c.sdrSaturation));
      return result;
    }
  } // namespace
  std::unique_ptr<Node> makeHyprlandDisplayEditor(const SettingEntry& entry, const SettingsContentContext& ctx) {
    if (entry.section != SettingsSection::Displays || !ctx.displays || !ctx.displayEditor)
      return {};
    auto* manager = ctx.displays;
    const auto rebuild = ctx.requestContentRebuild;
    manager->changed = [rebuild, reset = ctx.resetContentScroll] {
      reset();
      rebuild();
    };
    if (!*ctx.displayEditor)
      *ctx.displayEditor = std::make_shared<DisplayEditorState>();
    const auto state = *ctx.displayEditor;
    const auto displays = manager->displays();
    auto column = ui::column({.align = FlexAlign::Stretch, .gap = 12 * ctx.scale, .fillWidth = true});
    const auto label = [&](std::string text) {
      return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
    };
    const auto button = [&](std::string text, std::function<void()> clicked) {
      return ui::button(
          {.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(clicked)}
      );
    };
    if (!manager->error().empty())
      column->addChild(label(manager->error()));
    if (manager->previewing()) {
      column->addChild(label(tr("confirm")));
      column->addChild(
          ui::row(
              {.gap = 12 * ctx.scale},
              button(
                  tr("keep"),
                  [manager, config = ctx.configService, rebuild, state] {
                    const bool kept = manager->keep([config](const auto& candidate) {
                      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
                      for (const auto& row : candidate) {
                        auto fields = overrides(row);
                        changes.insert(changes.end(), fields.begin(), fields.end());
                      }
                      return config && config->setOverrides(changes);
                    });
                    if (kept)
                      state->selected.clear();
                    rebuild();
                  }
              ),
              button(tr("revert"), [manager] { manager->revert(); })
          )
      );
      return column;
    }
    if (displays.empty()) {
      column->addChild(label(tr("no-displays")));
      column->addChild(button(tr("refresh"), rebuild));
      return column;
    }
    if (state->selected.empty()) {
      state->drafts.clear();
      state->original.clear();
    }
    for (const auto& output : displays) {
      if (std::ranges::find(state->drafts, output.output, &HyprlandDisplayConfig::output) != state->drafts.end())
        continue;
      const auto saved =
          std::ranges::find(ctx.config.shell.hyprlandDisplays, output.output, &HyprlandDisplayConfig::output);
      auto draft = saved != ctx.config.shell.hyprlandDisplays.end() ? *saved : HyprlandDisplayConfig{};
      draft.output = output.output;
      state->drafts.push_back(draft);
      state->original.push_back(draft);
    }
    auto selected = std::ranges::find(displays, state->selected, &compositors::hyprland::DisplayInfo::output);
    if (selected == displays.end()) {
      selected = displays.begin();
      state->selected = selected->output;
    }
    const auto current = *selected;
    const auto select = [&](std::string title, std::vector<SelectOption> options, std::string value,
                            std::function<void(std::string)> change) {
      std::vector<std::string> labels;
      std::size_t index = 0;
      for (std::size_t i = 0; i < options.size(); ++i) {
        labels.push_back(options[i].label);
        if (options[i].value == value)
          index = i;
      }
      column->addChild(
          ui::row(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::SpaceBetween,
               .gap = 12 * ctx.scale,
               .fillWidth = true},
              label(std::move(title)),
              ui::select(
                  {.options = std::move(labels),
                   .selectedIndex = index,
                   .fontSize = Style::fontSizeBody * ctx.scale,
                   .width = 330 * ctx.scale,
                   .height = Style::controlHeight * ctx.scale,
                   .onSelectionChanged = [options = std::move(options), change = std::move(change),
                                          rebuild](std::size_t chosen, std::string_view) {
                     if (chosen < options.size()) {
                       change(options[chosen].value);
                       rebuild();
                     }
                   }}
              )
          )
      );
    };
    std::vector<DisplayRect> rectangles;
    std::vector<std::string> names;
    for (const auto& output : displays) {
      const auto draft = std::ranges::find(state->drafts, output.output, &HyprlandDisplayConfig::output);
      rectangles.push_back(displayRect(*draft, output));
      names.push_back(output.output);
    }
    column->addChild(label(tr("layout-hint")));
    column->addChild(
        std::make_unique<DisplayLayoutCanvas>(
            rectangles, names, std::size_t(std::distance(displays.begin(), selected)), ctx.scale,
            [state, names, rebuild](std::size_t index, DisplayRect rect, bool moved) {
              state->selected = names[index];
              if (moved) {
                auto& draft = state->draft();
                draft.positionManaged = true;
                draft.x = int(std::lround(rect.x));
                draft.y = int(std::lround(rect.y));
              }
              rebuild();
            }
        )
    );
    column->addChild(button(tr("identify"), [identify = ctx.identifyDisplays, names] {
      if (identify)
        identify(names);
    }));
    std::vector<SelectOption> outputOptions;
    for (const auto& d : displays)
      outputOptions.push_back(
          {d.output, std::to_string(outputOptions.size() + 1) + " · " + d.output + " · " + d.description}
      );
    select(tr("display"), std::move(outputOptions), state->selected, [state](std::string output) {
      state->selected = output;
    });
    column->addChild(label(current.description + " · " + current.mode + " Hz · " + current.colorMode));
    column->addChild(label(tr("staged")));
    const auto geometry = displayRect(state->draft(), current);
    column->addChild(label(tr("position") + std::format(": {}, {}", int(geometry.x), int(geometry.y))));
    std::vector<SelectOption> scales{{"0", tr("keep-current") + std::format(" ({:.0f}%)", current.scale * 100)}};
    for (int percent = 50; percent <= 400; percent += 25)
      scales.push_back({std::to_string(percent), std::to_string(percent) + "%"});
    const auto scaleKey = std::to_string(std::lround(state->draft().scale * 100));
    if (std::ranges::none_of(scales, [&](const auto& option) { return option.value == scaleKey; }))
      scales.push_back({scaleKey, scaleKey + "%"});
    select(tr("scale"), std::move(scales), scaleKey, [state](std::string value) {
      state->draft().scale = float(std::stoi(value)) / 100;
    });
    const auto resolution = [](std::string_view mode) { return std::string(mode.substr(0, mode.find('@'))); };
    std::vector<SelectOption> resolutions{
        {"", tr("keep-current") + " (" + resolution(current.mode) + ")"}, {"preferred", tr("preferred")}
    };
    for (const auto& mode : current.modes) {
      const auto size = resolution(mode);
      if (std::ranges::none_of(resolutions, [&](const auto& option) { return option.value == size; }))
        resolutions.push_back({size, size});
    }
    select(
        tr("resolution"), std::move(resolutions), resolution(state->draft().mode),
        [state, current, resolution](std::string size) {
          if (size.empty() || size == "preferred")
            state->draft().mode = size;
          else {
            const auto mode = std::ranges::find_if(current.modes, [&](const auto& m) { return resolution(m) == size; });
            if (mode != current.modes.end())
              state->draft().mode = *mode;
          }
        }
    );
    const auto effectiveMode = state->draft().mode.empty() ? current.mode : state->draft().mode;
    if (effectiveMode.find('@') != std::string::npos) {
      std::vector<SelectOption> rates;
      for (const auto& mode : current.modes)
        if (resolution(mode) == resolution(effectiveMode))
          rates.push_back({mode, mode.substr(mode.find('@') + 1) + " Hz"});
      select(tr("refresh-rate"), std::move(rates), effectiveMode, [state](std::string mode) {
        state->draft().mode = mode;
      });
    }
    std::vector<SelectOption> transforms{
        {"-1", tr("keep-current") + " (" + tr("transform-" + std::to_string(current.transform)) + ")"}
    };
    for (int value = 0; value < 8; ++value)
      transforms.push_back({std::to_string(value), tr("transform-" + std::to_string(value))});
    select(
        tr("transform"), std::move(transforms), std::to_string(state->draft().transform),
        [state](std::string value) { state->draft().transform = std::stoi(value); }
    );
    std::vector<SelectOption> vrr;
    for (int value = -2; value <= 3; ++value)
      vrr.push_back({std::to_string(value), tr("vrr-" + std::to_string(value))});
    select(tr("vrr"), std::move(vrr), std::to_string(state->draft().vrr), [state](std::string value) {
      state->draft().vrr = std::stoi(value);
    });
    column->addChild(label(tr(current.vrrActive ? "vrr-active" : "vrr-inactive")));
    std::vector<SelectOption> colors{{"", tr("keep-current") + " (" + current.colorMode + ")"}};
    for (const auto* value : {"auto", "srgb", "dcip3", "dp3", "adobe", "wide", "edid", "hdr", "hdredid"})
      colors.push_back({value, tr(std::string("cm-") + value)});
    select(tr("color-mode"), std::move(colors), state->draft().colorMode, [state](std::string value) {
      state->draft().colorMode = std::move(value);
    });
    select(
        tr("bit-depth"),
        {{"0", tr("keep-current") + " (" + std::to_string(current.bitDepth) + ")"}, {"8", "8"}, {"10", "10"}},
        std::to_string(state->draft().bitDepth),
        [state](std::string value) { state->draft().bitDepth = std::stoi(value); }
    );
    for (bool brightness : {true, false}) {
      const float value = brightness ? state->draft().sdrBrightness : state->draft().sdrSaturation;
      std::vector<SelectOption> values{{"0", tr("keep-current")}};
      for (int percent = 10; percent <= 200; percent += 10)
        values.push_back({std::to_string(percent), std::to_string(percent) + "%"});
      select(
          tr(brightness ? "sdr-brightness" : "sdr-saturation"), std::move(values),
          std::to_string(std::lround(value * 100)), [state, brightness](std::string v) {
            (brightness ? state->draft().sdrBrightness : state->draft().sdrSaturation) =
                static_cast<float>(std::stoi(v)) / 100;
          }
      );
    }
    column->addChild(label(tr("hdr-hint")));
    column->addChild(
        ui::row(
            {.gap = 12 * ctx.scale},
            button(
                tr("apply"),
                [state, manager, rebuild] {
                  std::vector<HyprlandDisplayConfig> candidates;
                  for (auto row : state->drafts) {
                    const auto old = std::ranges::find(state->original, row.output, &HyprlandDisplayConfig::output);
                    if (row.output == state->selected || old == state->original.end() || row != *old) {
                      row.managed = true;
                      candidates.push_back(row);
                    }
                  }
                  manager->preview(std::move(candidates));
                  rebuild();
                }
            ),
            button(
                tr("discard"),
                [state, rebuild] {
                  state->selected.clear();
                  rebuild();
                }
            ),
            button(
                tr("use-config"),
                [state, manager, rebuild] {
                  auto candidate = state->draft();
                  candidate.managed = false;
                  manager->preview({std::move(candidate)});
                  rebuild();
                }
            ),
            button(tr("refresh"), rebuild)
        )
    );
    return column;
  }
} // namespace settings
