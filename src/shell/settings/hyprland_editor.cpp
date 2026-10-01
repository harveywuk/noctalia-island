#include "shell/settings/hyprland_editor.h"

#include "compositors/hyprland/hyprland_appearance.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/timer_manager.h"
#include "i18n/i18n.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/hyprland_keybind_editor.h"
#include "shell/settings/hyprland_workspace_editor.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "system/desktop_entry.h"
#include "system/keyboard_layout_catalog.h"
#include "ui/builders.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/toggle.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <linux/input-event-codes.h>
#include <sstream>

namespace settings {
  namespace {
    using Path = std::vector<std::string>;
    const Path root{"shell", "hyprland_appearance"};
    Path path(std::string key) {
      auto p = root;
      p.push_back(std::move(key));
      return p;
    }
    std::string tr(std::string_view key) {
      return i18n::tr(std::string("settings.hyprland-editor.") + std::string(key));
    }
    void flatten(const toml::table& table, Path prefix, AppearanceOverrides& result) {
      for (const auto& [key, node] : table) {
        auto p = prefix;
        p.emplace_back(key.str());
        if (const auto* sub = node.as_table())
          flatten(*sub, p, result);
        else if (node.is_boolean())
          result.emplace_back(p, node.value<bool>().value());
        else if (node.is_integer())
          result.emplace_back(p, node.value<std::int64_t>().value());
        else if (node.is_floating_point())
          result.emplace_back(p, node.value<double>().value());
        else if (auto text = node.value<std::string>())
          result.emplace_back(p, *text);
      }
    }
    toml::table appearanceTable(const HyprlandAppearanceConfig& config) {
      ShellConfig shell;
      shell.hyprlandAppearance = config;
      auto table = noctalia::config::schema::writeTable(shell, noctalia::config::schema::shellSchema());
      return *table["hyprland_appearance"].as_table();
    }
    HyprlandMotionCurve curveFor(const HyprlandAppearanceConfig& c, std::string_view key) {
      auto curve = c.curve;
      curve.easing = c.animationEasing;
      if (key == "opening_curve")
        curve = c.openingCurve;
      if (key == "closing_curve")
        curve = c.closingCurve;
      if (key == "moving_curve")
        curve = c.movingCurve;
      if (key == "workspace_curve")
        curve = c.workspaceCurve;
      if (curve.easing == HyprlandAnimationEasing::Inherit) {
        curve = c.curve;
        curve.easing = c.animationEasing;
      }
      return compositors::hyprland::resolvedMotionCurve(curve);
    }
    float cubic(float t, float a, float b) {
      const float u = 1 - t;
      return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
    }
    float progress(const HyprlandMotionCurve& c, float t) {
      if (c.easing == HyprlandAnimationEasing::Spring) {
        // Unit-mass damped oscillator, starting at rest. Preview illustrates the curve.
        const float w = std::sqrt(c.stiffness), z = c.damping / (2 * w);
        if (z < .999F) {
          float wd = w * std::sqrt(1 - z * z);
          return 1 - std::exp(-z * w * t) * (std::cos(wd * t) + z * w / wd * std::sin(wd * t));
        }
        if (z > 1.001F) {
          const float d = std::sqrt(z * z - 1), a = -w * (z - d), b = -w * (z + d);
          return 1 + (b * std::exp(a * t) - a * std::exp(b * t)) / (a - b);
        }
        return 1 - (1 + w * t) * std::exp(-w * t);
      }
      float lo = 0, hi = 1;
      for (int i = 0; i < 20; ++i) {
        float mid = (lo + hi) / 2;
        if (cubic(mid, c.x1, c.x2) < t)
          lo = mid;
        else
          hi = mid;
      }
      return cubic((lo + hi) / 2, c.y1, c.y2);
    }

    class CurveCanvas final : public Node {
    public:
      CurveCanvas(HyprlandMotionCurve c, float scale, float duration, std::function<void(HyprlandMotionCurve)> commit)
          : m_curve(c), m_scale(scale), m_duration(duration), m_commit(std::move(commit)) {
        setSize(360 * scale, 240 * scale);
        for (int i = 0; i < 5; ++i)
          m_grid.push_back(rect(ColorRole::Outline, .4F));
        for (int i = 0; i < 160; ++i)
          m_points.push_back(rect(ColorRole::Primary));
        for (int i = 0; i < 2; ++i)
          m_handles.push_back(rect(ColorRole::Secondary));
        m_window = rect(ColorRole::Primary);
        auto area = std::make_unique<InputArea>();
        m_area = area.get();
        area->setOnPress([this](const InputArea::PointerData& d) {
          if (d.button != BTN_LEFT || m_curve.easing == HyprlandAnimationEasing::Spring)
            return;
          if (d.pressed) {
            const float x = (d.localX / m_scale - 24) / 312, y = (184 - d.localY / m_scale) / 52 - 1;
            auto distance = [&](float a, float b) { return std::hypot((x - a) * 312, (y - b) * 52); };
            m_drag = distance(m_curve.x1, m_curve.y1) < distance(m_curve.x2, m_curve.y2) ? 0 : 1;
            move(d.localX, d.localY);
          } else if (m_drag >= 0) {
            m_drag = -1;
            m_commit(m_curve);
          }
        });
        area->setOnMotion([this](const InputArea::PointerData& d) {
          if (m_drag >= 0 && m_area->pressed())
            move(d.localX, d.localY);
        });
        addChild(std::move(area));
        redraw();
      }
      void play() {
        if (m_timer.active()) {
          m_timer.stop();
          return;
        }
        m_start = std::chrono::steady_clock::now();
        m_timer.startRepeating(std::chrono::milliseconds(16), [this] {
          for (const Node* ancestor = this; ancestor; ancestor = ancestor->parent()) {
            if (!ancestor->visible()) {
              m_timer.stop();
              return;
            }
          }
          float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - m_start).count();
          float t = std::fmod(elapsed, m_duration + .5F) / m_duration;
          float p = progress(m_curve, std::min(t, 1.0F));
          m_window->setPosition((24 + std::clamp(p, -.05F, 1.1F) * 270) * m_scale, 206 * m_scale);
        });
      }

    private:
      RectNode* rect(ColorRole role, float alpha = 1) {
        auto n = std::make_unique<RectNode>();
        auto* ptr = n.get();
        auto color = colorForRole(role, alpha);
        n->setStyle(
            {.fill = color, .border = color, .fillMode = FillMode::Solid, .radius = 2, .softness = 1, .borderWidth = 0}
        );
        addChild(std::move(n));
        return ptr;
      }
      void move(float px, float py) {
        float x = std::clamp((px / m_scale - 24) / 312, 0.0F, 1.0F),
              y = std::clamp((184 - py / m_scale) / 52 - 1, -1.0F, 2.0F);
        m_curve.easing = HyprlandAnimationEasing::Custom;
        if (m_drag == 0) {
          m_curve.x1 = x;
          m_curve.y1 = y;
        } else {
          m_curve.x2 = x;
          m_curve.y2 = y;
        }
        redraw();
      }
      void redraw() {
        auto place = [&](Node* n, float x, float y, float w, float h) {
          n->setPosition(x * m_scale, y * m_scale);
          n->setSize(w * m_scale, h * m_scale);
        };
        for (int i = 0; i < 4; ++i)
          place(m_grid[i], 24, 184 - static_cast<float>(i) * 52, 312, 1);
        place(m_grid[4], 24, 28, 1, 156);
        for (int i = 0; i < 160; ++i) {
          float t = static_cast<float>(i) / 159.0F;
          float x = t, y = progress(m_curve, t);
          if (m_curve.easing != HyprlandAnimationEasing::Spring) {
            x = cubic(t, m_curve.x1, m_curve.x2);
            y = cubic(t, m_curve.y1, m_curve.y2);
          }
          place(m_points[i], 24 + x * 312, 184 - (y + 1) * 52, 2.5F, 2.5F);
        }
        place(m_handles[0], 19 + m_curve.x1 * 312, 179 - (m_curve.y1 + 1) * 52, 10, 10);
        place(m_handles[1], 19 + m_curve.x2 * 312, 179 - (m_curve.y2 + 1) * 52, 10, 10);
        for (auto* h : m_handles)
          h->setVisible(m_curve.easing != HyprlandAnimationEasing::Spring);
        place(m_window, 24, 206, 30, 22);
        place(m_area, 0, 0, 360, 195);
      }
      HyprlandMotionCurve m_curve;
      float m_scale, m_duration;
      int m_drag = -1;
      std::function<void(HyprlandMotionCurve)> m_commit;
      std::vector<RectNode*> m_points, m_handles, m_grid;
      RectNode* m_window;
      InputArea* m_area;
      Timer m_timer;
      std::chrono::steady_clock::time_point m_start;
    };
  } // namespace
  AppearanceOverrides appearanceOverrides(const HyprlandAppearanceConfig& config) {
    AppearanceOverrides result;
    flatten(appearanceTable(config), root, result);
    return result;
  }

  HyprlandAppearanceConfig softGlassAppearance(HyprlandAppearanceConfig c) {
    c.enabled = c.followTheme = c.blurFocusManaged = true;
    c.gapsIn = 6;
    c.gapsOut = 12;
    c.borderSize = 2;
    c.rounding = 16;
    c.roundingPower = 3;
    c.activeOpacity = c.inactiveOpacity = c.fullscreenOpacity = 1;
    c.blurEnabled = true;
    c.blurSize = 4;
    c.blurPasses = 2;
    c.blurBrightness = .95F;
    c.blurContrast = .9F;
    c.blurVibrancy = .2F;
    c.blurNoise = .015F;
    c.blurPopups = true;
    c.blurPopupsIgnorealpha = .2F;
    c.blurSpecial = false;
    c.dimInactive = true;
    c.dimStrength = .08F;
    c.dimSpecial = .2F;
    c.decorationEffectsManaged = c.shadowEnabled = c.glowEnabled = true;
    c.shadowRange = 18;
    c.shadowPower = 3;
    c.shadowSharp = false;
    c.shadowOffsetX = 0;
    c.shadowOffsetY = 4;
    c.shadowScale = 1;
    c.shadowColor = fixedColorSpec(hex("#000000"));
    c.shadowInactiveColor.reset();
    c.shadowOpacity = .55F;
    c.shadowInactiveOpacity = .35F;
    c.glowRange = 6;
    c.glowPower = 3;
    c.glowColor = colorSpecFromRole(ColorRole::Primary);
    c.glowInactiveColor.reset();
    c.glowOpacity = .16F;
    c.glowInactiveOpacity = .04F;
    c.glassManaged = c.glassEnabled = true;
    c.glassLight = c.glassLayers = false;
    c.glassBlur = 1.5F;
    c.glassRefraction = .35F;
    c.glassChromatic = .15F;
    c.glassLens = .2F;
    c.glassOpacity = 1;
    c.glassFresnel = .6F;
    c.glassSpecular = .8F;
    c.animationsEnabled = c.customAnimations = true;
    c.animationSpeed = 1;
    c.animationEasing = HyprlandAnimationEasing::Smooth;
    c.windowAnimation = HyprlandWindowAnimation::Pop;
    c.workspaceAnimation = HyprlandWorkspaceAnimation::Slide;
    c.curve = c.openingCurve = c.closingCurve = c.movingCurve = c.workspaceCurve = {};
    c.openingDuration = c.workspaceDuration = 400;
    c.closingDuration = 250;
    c.movingDuration = 300;
    return c;
  }

  AppearanceOverrides manualAppearanceOverrides(const HyprlandAppearanceConfig& config) {
    auto result = appearanceOverrides(config);
    result.emplace_back(Path{"shell", "hyprland_profile_switching", "enabled"}, false);
    return result;
  }

  HyprlandAppearanceConfig softGlassOverview(HyprlandAppearanceConfig c) {
    const HyprlandAppearanceConfig defaults;
    c.overviewManaged = c.overviewStyleManaged = true;
    c.overviewCentered = c.overviewBlur = true;
    c.overviewMargin = 12;
    c.overviewPanelBorder = 1;
    c.overviewWorkspaceBorder = 2;
    c.overviewDragOpacity = .85F;
    c.overviewDuration = 350;
    c.overviewPanelColor = defaults.overviewPanelColor;
    c.overviewPanelBorderColor = defaults.overviewPanelBorderColor;
    c.overviewActiveBackground = defaults.overviewActiveBackground;
    c.overviewInactiveBackground = defaults.overviewInactiveBackground;
    c.overviewActiveBorder = defaults.overviewActiveBorder;
    c.overviewInactiveBorder = defaults.overviewInactiveBorder;
    c.overviewPanelOpacity = defaults.overviewPanelOpacity;
    c.overviewPanelBorderOpacity = defaults.overviewPanelBorderOpacity;
    c.overviewActiveBackgroundOpacity = defaults.overviewActiveBackgroundOpacity;
    c.overviewInactiveBackgroundOpacity = defaults.overviewInactiveBackgroundOpacity;
    c.overviewActiveBorderOpacity = defaults.overviewActiveBorderOpacity;
    c.overviewInactiveBorderOpacity = defaults.overviewInactiveBorderOpacity;
    return c;
  }

  namespace {
    std::string profileLabel(std::string_view name) {
      if (name == "@soft-glass-dark")
        return tr("profile-dark-built-in");
      if (name == "@soft-glass-light")
        return tr("profile-light-built-in");
      return std::string(name);
    }

    std::unique_ptr<Node> makeThemeProfilesEditor(const SettingsContentContext& ctx) {
      auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
      const auto& switching = ctx.config.shell.hyprlandProfileSwitching;
      const auto resolved = compositors::hyprland::resolveAppearanceProfile(ctx.config.shell, isResolvedLightTheme());
      const auto label = [&](std::string text) {
        return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
      };
      column->addChild(label(tr(switching.enabled ? "profile-auto-status" : "profile-manual-status")));
      if (switching.enabled) {
        column->addChild(label(
            tr(isResolvedLightTheme() ? "profile-light-active" : "profile-dark-active")
            + " "
            + profileLabel(resolved.profile)
        ));
        if (resolved.fallback)
          column->addChild(label(tr("profile-fallback")));
      }
      column->addChild(label(tr("profile-switching-hint")));
      column->addChild(
          ui::button(
              {.text = tr(switching.enabled ? "profile-keep-current" : "profile-follow-mode"),
               .fontSize = Style::fontSizeBody * ctx.scale,
               .onClick = [automatic = switching.enabled, look = resolved.appearance, commit = ctx.setOverrides] {
                 if (automatic)
                   commit(manualAppearanceOverrides(look));
                 else
                   commit({{{"shell", "hyprland_profile_switching", "enabled"}, true}});
               }}
          )
      );
      SettingsControlFactory factory(ctx);
      for (const auto& [key, selected] : std::vector<std::pair<std::string, std::string>>{
               {"light_profile", switching.lightProfile}, {"dark_profile", switching.darkProfile}
           }) {
        column->addChild(label(tr("profile-" + key)));
        SelectSetting select;
        select.options = {
            {"@soft-glass-light", profileLabel("@soft-glass-light")},
            {"@soft-glass-dark", profileLabel("@soft-glass-dark")}
        };
        std::vector<std::string> names;
        for (const auto& [name, _] : ctx.config.shell.hyprlandAppearanceProfiles)
          if (!name.starts_with('@'))
            names.push_back(name);
        std::ranges::sort(names);
        for (const auto& name : names)
          select.options.push_back({name, name});
        if (std::ranges::none_of(select.options, [&](const auto& option) { return option.value == selected; }))
          select.options.push_back({selected, selected + " · " + tr("profile-unavailable")});
        select.selectedValue = selected;
        select.preferredWidth = 320;
        column->addChild(factory.makeSelect(select, {"shell", "hyprland_profile_switching", key}));
      }
      return column;
    }
  } // namespace

  HyprlandAppRule appAppearancePreset(HyprlandAppRule rule, std::string_view preset) {
    HyprlandAppRule r;
    r.name = rule.name;
    r.appClass = rule.appClass;
    r.scope = rule.scope;
    r.enabled = rule.enabled;
    if (preset == "reset")
      return r;
    r.opacityManaged = true;
    if (preset == "terminal") {
      r.activeOpacity = .95F;
      r.inactiveOpacity = .9F;
      r.blur = HyprlandRuleSwitch::On;
      r.glass = HyprlandRuleSwitch::Off;
    } else if (preset == "readable") {
      r.blur = r.glass = HyprlandRuleSwitch::Off;
    } else if (preset == "fullscreen") {
      r.scope = HyprlandRuleScope::Fullscreen;
      r.blur = r.glass = r.shadow = r.dim = r.animations = HyprlandRuleSwitch::Off;
      r.roundingManaged = true;
      r.rounding = 0;
    } else if (preset == "dialog") {
      r.scope = HyprlandRuleScope::Floating;
      r.roundingManaged = true;
      r.shadow = HyprlandRuleSwitch::On;
    }
    return r;
  }

  namespace {
    AppearanceOverrides ruleOverrides(const HyprlandAppRule& rule) {
      ShellConfig shell;
      shell.hyprlandAppRules = {rule};
      auto table = noctalia::config::schema::writeTable(shell, noctalia::config::schema::shellSchema());
      AppearanceOverrides result;
      flatten(*table["hyprland_app_rules"][rule.name].as_table(), {"shell", "hyprland_app_rules", rule.name}, result);
      return result;
    }

    void
    addAppPicker(Flex& column, const SettingsContentContext& ctx, std::function<void(const std::string&)> addRule) {
      column.addChild(
          ui::button(
              {.text = tr("pick-app"),
               .fontSize = Style::fontSizeBody * ctx.scale,
               .onClick = [open = ctx.openSearchPickerPopup, running = ctx.runningAppIds, addRule] {
                 SearchPickerOpenRequest request;
                 request.title = tr("pick-app");
                 request.placeholder = tr("search-app");
                 request.emptyText = tr("no-apps");
                 std::unordered_set<std::string> seen;
                 if (running)
                   for (const auto& app : running())
                     if (!app.empty() && seen.insert(app).second)
                       request.options.push_back({app, app + " · " + tr("running-app")});
                 for (const auto& app : desktopEntries()) {
                   const auto id = app.startupWmClass.empty() ? app.id : app.startupWmClass;
                   if (!id.empty() && seen.insert(id).second)
                     request.options.push_back({id, app.name + " · " + id});
                 }
                 request.onSelect = addRule;
                 if (open)
                   open(std::move(request));
               }}
          )
      );
      auto manual = std::make_shared<std::string>();
      column.addChild(
          ui::input(
              {.placeholder = tr("app-class"),
               .width = 360 * ctx.scale,
               .onChange = [manual](const std::string& value) { *manual = value; }}
          )
      );
      column.addChild(
          ui::button({.text = tr("add-app"), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = [manual, addRule] {
                        addRule(*manual);
                      }})
      );
    }

    std::string placementTr(std::string_view key) {
      return i18n::tr("settings.hyprland-placement." + std::string(key));
    }

    AppearanceOverrides placementOverrides(const HyprlandPlacementRule& rule) {
      ShellConfig shell;
      shell.hyprlandPlacementRules = {rule};
      auto table = noctalia::config::schema::writeTable(shell, noctalia::config::schema::shellSchema());
      AppearanceOverrides result;
      flatten(
          *table["hyprland_placement_rules"][rule.name].as_table(), {"shell", "hyprland_placement_rules", rule.name},
          result
      );
      return result;
    }

    std::unique_ptr<Node> makePlacementEditor(const SettingsContentContext& ctx) {
      auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
      const auto label = [&](std::string text) {
        return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
      };
      const auto button = [&](std::string text, std::function<void()> action) {
        return ui::button(
            {.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(action)}
        );
      };
      column->addChild(label(placementTr("hint")));
      auto& expanded = ctx.expandedGroupsByPage["hyprland-placement:entries"];
      addAppPicker(
          *column, ctx,
          [rules = ctx.config.shell.hyprlandPlacementRules, expanded = &expanded,
           commit = ctx.setOverrides](const std::string& app) {
            if (app.empty() || app.size() > 512)
              return;
            HyprlandPlacementRule r;
            for (int i = 1;; ++i) {
              r.name = std::format("app-{:04}", i);
              if (std::ranges::none_of(rules, [&](const auto& old) { return old.name == r.name; }))
                break;
            }
            r.appClass = app;
            expanded->insert(r.name);
            commit(placementOverrides(r));
          }
      );
      SettingsControlFactory factory(ctx);
      for (const auto& r : ctx.config.shell.hyprlandPlacementRules) {
        const auto p = [&](std::string key) {
          return Path{"shell", "hyprland_placement_rules", r.name, std::move(key)};
        };
        auto* body = addSettingsGroupCard(
            {.parent = *column,
             .group = r.name,
             .title =
                 (r.appClass.empty() ? r.name : r.appClass) + " · " + placementTr(r.enabled ? "active" : "disabled"),
             .scale = ctx.scale,
             .expandedGroups = expanded}
        );
        static_cast<Flex*>(column->children().back().get())->setFillWidth(true);
        const auto toggle = [&](std::string key, bool value) {
          auto row = ui::row({.justify = FlexJustify::SpaceBetween, .fillWidth = true});
          row->addChild(label(placementTr(key)));
          row->addChild(factory.makeToggle(value, true, p(key)));
          body->addChild(std::move(row));
        };
        const auto select = [&](std::string key, auto value, const auto& choices) {
          body->addChild(label(placementTr(key)));
          SelectSetting setting;
          for (const auto& choice : choices) {
            setting.options.push_back({std::string(choice.key), i18n::tr(choice.labelKey)});
            if (choice.value == value)
              setting.selectedValue = choice.key;
          }
          body->addChild(factory.makeSelect(setting, p(key)));
        };
        const auto number = [&](std::string key, int value, int low, int high) {
          body->addChild(label(placementTr(key)));
          body->addChild(factory.makeSlider(value, low, high, 1, p(key), true));
        };
        auto actions = ui::row({.gap = 8 * ctx.scale});
        actions->addChild(button(placementTr("reset"), [r, commit = ctx.setOverrides] {
          HyprlandPlacementRule reset;
          reset.name = r.name;
          reset.appClass = r.appClass;
          commit(placementOverrides(reset));
        }));
        actions->addChild(button(tr("rule-remove"), [r, clear = ctx.clearOverride] {
          clear({"shell", "hyprland_placement_rules", r.name});
        }));
        body->addChild(std::move(actions));
        toggle("enabled", r.enabled);
        body->addChild(factory.makeText(r.appClass, tr("app-class"), p("app_class"), 360));
        if (const auto problem = compositors::hyprland::placementRuleProblem(r); !problem.empty())
          body->addChild(label(placementTr(problem)));
        select("mode", r.mode, kHyprlandPlacementModes);
        select("workspace", r.workspace, kHyprlandPlacementWorkspaces);
        if (r.workspace == HyprlandPlacementWorkspace::Number)
          number("workspace_number", r.workspaceNumber, 1, 1000);
        else if (
            r.workspace == HyprlandPlacementWorkspace::Named || r.workspace == HyprlandPlacementWorkspace::Special
        ) {
          body->addChild(label(placementTr("workspace_name")));
          body->addChild(
              factory.makeText(r.workspaceName, placementTr("workspace-name-hint"), p("workspace_name"), 360)
          );
        }
        if (r.workspace != HyprlandPlacementWorkspace::Inherit)
          toggle("workspace_silent", r.workspaceSilent);
        if (r.mode != HyprlandPlacementMode::Tiled) {
          body->addChild(label(placementTr("floating-hint")));
          toggle("size_managed", r.sizeManaged);
          if (r.sizeManaged) {
            number("width", r.width, 100, 8192);
            number("height", r.height, 100, 8192);
          }
          select("position", r.position, kHyprlandPlacementPositions);
          if (r.position == HyprlandPlacementPosition::Offset) {
            number("x", r.x, 0, 16384);
            number("y", r.y, 0, 16384);
          }
          select("pin", r.pin, kHyprlandRuleSwitches);
        }
      }
      return column;
    }

    std::unique_ptr<Node> makeAppRuleEditor(const SettingsContentContext& ctx) {
      auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
      const auto label = [&](std::string text) {
        return ui::label({.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 5});
      };
      const auto button = [&](std::string text, std::function<void()> action) {
        return ui::button(
            {.text = std::move(text), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(action)}
        );
      };
      column->addChild(label(tr("app-rules-hint")));
      auto& expanded = ctx.expandedGroupsByPage["hyprland-app-rules:entries"];
      const auto addRule = [rules = ctx.config.shell.hyprlandAppRules, expanded = &expanded,
                            commit = ctx.setOverrides](const std::string& app) {
        if (app.empty() || app.size() > 512)
          return;
        HyprlandAppRule r;
        for (int i = 1;; ++i) {
          r.name = std::format("app-{:04}", i);
          if (std::ranges::none_of(rules, [&](const auto& old) { return old.name == r.name; }))
            break;
        }
        r.appClass = app;
        expanded->insert(r.name);
        commit(ruleOverrides(r));
      };
      addAppPicker(*column, ctx, addRule);
      SettingsControlFactory factory(ctx);
      for (const auto& r : ctx.config.shell.hyprlandAppRules) {
        const auto p = [&](std::string key) { return Path{"shell", "hyprland_app_rules", r.name, std::move(key)}; };
        std::string scopeLabel;
        for (const auto& scope : kHyprlandRuleScopes)
          if (scope.value == r.scope)
            scopeLabel = i18n::tr(scope.labelKey);
        auto* body = addSettingsGroupCard(
            {.parent = *column,
             .group = r.name,
             .title = (r.appClass.empty() ? r.name : r.appClass) + " · " + scopeLabel,
             .scale = ctx.scale,
             .expandedGroups = expanded}
        );
        static_cast<Flex*>(column->children().back().get())->setFillWidth(true);
        const auto toggle = [&](std::string key, std::string title, bool value) {
          auto row = ui::row({.justify = FlexJustify::SpaceBetween, .fillWidth = true});
          row->addChild(label(tr(title)));
          row->addChild(factory.makeToggle(value, true, p(key)));
          body->addChild(std::move(row));
        };
        auto actions = ui::row({.gap = 8 * ctx.scale});
        actions->addChild(button(tr("rule-reset"), [r, commit = ctx.setOverrides] {
          commit(ruleOverrides(appAppearancePreset(r, "reset")));
        }));
        actions->addChild(button(tr("rule-remove"), [r, clear = ctx.clearOverride] {
          clear({"shell", "hyprland_app_rules", r.name});
        }));
        body->addChild(std::move(actions));
        toggle("enabled", "rule-enabled", r.enabled);
        body->addChild(factory.makeText(r.appClass, tr("app-class"), p("app_class"), 360));
        const auto select = [&](std::string key, auto value, const auto& choices) {
          body->addChild(label(tr("rule-" + key)));
          SelectSetting setting;
          for (const auto& choice : choices) {
            setting.options.push_back({std::string(choice.key), i18n::tr(choice.labelKey)});
            if (choice.value == value)
              setting.selectedValue = choice.key;
          }
          body->addChild(factory.makeSelect(setting, p(key)));
        };
        select("scope", r.scope, kHyprlandRuleScopes);
        auto presets = ui::row({.gap = 6 * ctx.scale});
        for (const std::string preset : {"terminal", "readable", "fullscreen", "dialog"})
          presets->addChild(button(tr("rule-preset-" + preset), [r, preset, commit = ctx.setOverrides] {
            commit(ruleOverrides(appAppearancePreset(r, preset)));
          }));
        body->addChild(std::move(presets));
        toggle("opacity_managed", "rule-opacity", r.opacityManaged);
        if (r.opacityManaged)
          for (const auto& [key, value] : std::vector<std::pair<std::string, float>>{
                   {"active_opacity", r.activeOpacity},
                   {"inactive_opacity", r.inactiveOpacity},
                   {"fullscreen_opacity", r.fullscreenOpacity}
               }) {
            body->addChild(label(tr("rule-" + key)));
            body->addChild(factory.makeSlider(value, .2, 1, .01, p(key)));
          }
        toggle("rounding_managed", "rule-rounding", r.roundingManaged);
        if (r.roundingManaged)
          body->addChild(factory.makeSlider(r.rounding, 0, 20, 1, p("rounding"), true));
        select("blur", r.blur, kHyprlandRuleSwitches);
        select("shadow", r.shadow, kHyprlandRuleSwitches);
        select("dim", r.dim, kHyprlandRuleSwitches);
        select("animations", r.animations, kHyprlandRuleSwitches);
        select("glass", r.glass, kHyprlandRuleSwitches);
      }
      return column;
    }
  } // namespace

  void addHyprlandEditorEntries(std::vector<SettingEntry>& entries, const Config& current) {
    entries.push_back(
        {.section = SettingsSection::AppPlacement,
         .group = "hyprland-placement",
         .title = placementTr("title"),
         .subtitle = placementTr("hint"),
         .path = {"shell", "hyprland_placement_rules"},
         .control = ButtonSetting{},
         .searchText = "hyprland app placement workspace floating tiled size position centre pin rules"}
    );
    entries.push_back(
        {.section = SettingsSection::Workspaces,
         .group = "hyprland-workspaces",
         .title = i18n::tr("settings.hyprland-workspaces.title"),
         .subtitle = i18n::tr("settings.hyprland-workspaces.hint"),
         .path = {"shell", "hyprland_workspaces"},
         .control = ButtonSetting{},
         .searchText = "workspace names icons monitor persistent keep empty"}
    );
    entries.push_back(
        {.section = SettingsSection::Keybinds,
         .group = "hyprland-keybinds",
         .title = i18n::tr("settings.hyprland-keybinds.title"),
         .subtitle = i18n::tr("settings.hyprland-keybinds.hint"),
         .path = {"shell", "hyprland_keybinds"},
         .control = ButtonSetting{},
         .searchText = "hyprland shortcuts keybind super workspace move window fullscreen launcher conflict"}
    );
    const auto& t = current.shell.hyprlandTiling;
    const auto tilingTr = [](std::string_view key) { return i18n::tr("settings.hyprland-tiling." + std::string(key)); };
    const auto addTiling = [&](std::string group, std::string key, SettingControl control,
                               SettingVisibility visible = {}) {
      entries.push_back(
          {.section = SettingsSection::WorkspaceTiling,
           .group = "hyprland-tiling-" + group,
           .title = tilingTr(key),
           .subtitle = tilingTr(key + "-description"),
           .path = {"shell", "hyprland_tiling", key},
           .control = std::move(control),
           .searchText = StringUtils::toLower(
               "hyprland workspace tiling layout dwindle master special " + key + " " + tilingTr(key)
           ),
           .visibleWhen = std::move(visible)}
      );
    };
    addTiling("layout", "layout_managed", ToggleSetting{t.layoutManaged});
    SelectSetting layoutSelect;
    for (const auto& option : kHyprlandTilingLayouts)
      layoutSelect.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
    layoutSelect.selectedValue = enumToKey(kHyprlandTilingLayouts, t.layout);
    addTiling("layout", "layout", layoutSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.layoutManaged;
    });
    addTiling("dwindle", "dwindle_managed", ToggleSetting{t.dwindleManaged});
    addTiling("dwindle", "preserve_split", ToggleSetting{t.preserveSplit}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.dwindleManaged && !t.smartSplit;
    });
    addTiling("dwindle", "smart_split", ToggleSetting{t.smartSplit}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.dwindleManaged;
    });
    SelectSetting forceSplitSelect;
    forceSplitSelect.options = {
        {"0", tilingTr("follow-pointer")}, {"1", tilingTr("left-top")}, {"2", tilingTr("right-bottom")}
    };
    forceSplitSelect.selectedValue = std::to_string(t.forceSplit);
    forceSplitSelect.valueType = SelectValueType::Integer;
    addTiling("dwindle", "force_split", forceSplitSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.dwindleManaged && !t.smartSplit;
    });
    addTiling("dwindle", "use_active_for_splits", ToggleSetting{t.useActiveForSplits}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.dwindleManaged;
    });
    addTiling(
        "dwindle", "default_split_ratio", SliderSetting{t.defaultSplitRatio, 0.1, 1.9, 0.05, false},
        [](const Config& cfg) {
          const auto& t = cfg.shell.hyprlandTiling;
          return t.dwindleManaged;
        }
    );
    addTiling(
        "dwindle", "split_width_multiplier", SliderSetting{t.splitWidthMultiplier, 0.1, 3, 0.05, false},
        [](const Config& cfg) {
          const auto& t = cfg.shell.hyprlandTiling;
          return t.dwindleManaged && !t.smartSplit;
        }
    );
    SelectSetting splitBiasSelect;
    splitBiasSelect.options = {{"0", tilingTr("directional")}, {"1", tilingTr("current")}};
    splitBiasSelect.selectedValue = std::to_string(t.splitBias);
    splitBiasSelect.valueType = SelectValueType::Integer;
    addTiling("dwindle", "split_bias", splitBiasSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.dwindleManaged;
    });
    addTiling("master", "master_managed", ToggleSetting{t.masterManaged});
    addTiling("master", "master_factor", SliderSetting{t.masterFactor, 0, 1, 0.05, false}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.masterManaged;
    });
    SelectSetting masterOrientationSelect;
    for (const auto& option : kHyprlandMasterOrientations)
      masterOrientationSelect.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
    masterOrientationSelect.selectedValue = enumToKey(kHyprlandMasterOrientations, t.masterOrientation);
    addTiling("master", "master_orientation", masterOrientationSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.masterManaged;
    });
    SelectSetting newStatusSelect;
    for (const auto& option : kHyprlandMasterStatuses)
      newStatusSelect.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
    newStatusSelect.selectedValue = enumToKey(kHyprlandMasterStatuses, t.newStatus);
    addTiling("master", "new_status", newStatusSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.masterManaged;
    });
    SelectSetting newOnActiveSelect;
    for (const auto& option : kHyprlandMasterPositions)
      newOnActiveSelect.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
    newOnActiveSelect.selectedValue = enumToKey(kHyprlandMasterPositions, t.newOnActive);
    addTiling("master", "new_on_active", newOnActiveSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.masterManaged;
    });
    addTiling("master", "new_on_top", ToggleSetting{t.newOnTop}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.masterManaged && t.newOnActive == HyprlandMasterPosition::None;
    });
    addTiling("special", "special_managed", ToggleSetting{t.specialManaged});
    addTiling("special", "close_special_on_empty", ToggleSetting{t.closeSpecialOnEmpty}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.specialManaged;
    });
    addTiling(
        "special", "hide_special_on_workspace_change", ToggleSetting{t.hideSpecialOnWorkspaceChange},
        [](const Config& cfg) {
          const auto& t = cfg.shell.hyprlandTiling;
          return t.specialManaged;
        }
    );
    addTiling("special", "special_fallthrough", ToggleSetting{t.specialFallthrough}, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.specialManaged;
    });
    SelectSetting warpOnSpecialSelect;
    warpOnSpecialSelect.options = {{"0", tilingTr("never")}, {"1", tilingTr("enabled")}, {"2", tilingTr("force")}};
    warpOnSpecialSelect.selectedValue = std::to_string(t.warpOnSpecial);
    warpOnSpecialSelect.valueType = SelectValueType::Integer;
    addTiling("special", "warp_on_special", warpOnSpecialSelect, [](const Config& cfg) {
      const auto& t = cfg.shell.hyprlandTiling;
      return t.specialManaged;
    });
    const auto& b = current.shell.hyprlandWindowBehaviour;
    const auto behaviourTr = [](std::string_view key) {
      return i18n::tr("settings.hyprland-behaviour." + std::string(key));
    };
    const auto addBehaviour = [&](std::string group, std::string key, SettingControl control,
                                  SettingVisibility visible = {}) {
      entries.push_back(
          {.section = SettingsSection::WindowBehaviour,
           .group = "hyprland-behaviour-" + group,
           .title = behaviourTr(key),
           .subtitle = behaviourTr(key + "-description"),
           .path = {"shell", "hyprland_window_behaviour", key},
           .control = std::move(control),
           .searchText = StringUtils::toLower(
               "hyprland window behaviour focus resizing snapping activation " + key + " " + behaviourTr(key)
           ),
           .visibleWhen = std::move(visible)}
      );
    };
    SelectSetting focusMode;
    focusMode.options = {
        {"0", behaviourTr("focus-click")},
        {"1", behaviourTr("focus-follow")},
        {"2", behaviourTr("focus-detached")},
        {"3", behaviourTr("focus-separate")}
    };
    focusMode.selectedValue = std::to_string(b.focusMode);
    focusMode.valueType = SelectValueType::Integer;
    addBehaviour("focus", "focus_managed", ToggleSetting{b.focusManaged});
    addBehaviour("focus", "focus_mode", focusMode, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.focusManaged;
    });
    addBehaviour("focus", "focus_threshold", SliderSetting{b.focusThreshold, 0, 100, 1, false}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.focusManaged && b.focusMode == 1;
    });
    addBehaviour("focus", "mouse_refocus", ToggleSetting{b.mouseRefocus}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.focusManaged && b.focusMode == 1;
    });
    addBehaviour("resize", "resize_managed", ToggleSetting{b.resizeManaged});
    addBehaviour("resize", "resize_on_border", ToggleSetting{b.resizeOnBorder}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.resizeManaged;
    });
    addBehaviour("resize", "border_grab", SliderSetting{b.borderGrab, 0, 100, 1, true}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.resizeManaged && b.resizeOnBorder;
    });
    addBehaviour("resize", "border_cursor", ToggleSetting{b.borderCursor}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.resizeManaged && b.resizeOnBorder;
    });
    addBehaviour("snap", "snap_managed", ToggleSetting{b.snapManaged});
    addBehaviour("snap", "snap_enabled", ToggleSetting{b.snapEnabled}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.snapManaged;
    });
    addBehaviour(
        "snap", "snap_window_distance", SliderSetting{b.snapWindowDistance, 0, 100, 1, true}, [](const Config& cfg) {
          const auto& b = cfg.shell.hyprlandWindowBehaviour;
          return b.snapManaged && b.snapEnabled;
        }
    );
    addBehaviour(
        "snap", "snap_monitor_distance", SliderSetting{b.snapMonitorDistance, 0, 100, 1, true}, [](const Config& cfg) {
          const auto& b = cfg.shell.hyprlandWindowBehaviour;
          return b.snapManaged && b.snapEnabled;
        }
    );
    addBehaviour("snap", "snap_border_overlap", ToggleSetting{b.snapBorderOverlap}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.snapManaged && b.snapEnabled;
    });
    addBehaviour("snap", "snap_respect_gaps", ToggleSetting{b.snapRespectGaps}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.snapManaged && b.snapEnabled;
    });
    addBehaviour("activation", "activation_managed", ToggleSetting{b.activationManaged});
    addBehaviour("activation", "focus_on_activate", ToggleSetting{b.focusOnActivate}, [](const Config& cfg) {
      const auto& b = cfg.shell.hyprlandWindowBehaviour;
      return b.activationManaged;
    });
    const auto& input = current.shell.hyprlandInput;
    const auto inputTr = [](std::string_view key) { return i18n::tr("settings.hyprland-input." + std::string(key)); };
    const auto addInput = [&](std::string group, std::string key, SettingControl control,
                              SettingVisibility visible = {}) {
      entries.push_back(
          {.section = SettingsSection::InputMotion,
           .group = "hyprland-input-" + group,
           .title = inputTr(key),
           .subtitle = inputTr(key + "-description"),
           .path = {"shell", "hyprland_input", key},
           .control = std::move(control),
           .searchText = StringUtils::toLower(
               "hyprland input gestures swipe workspace hyprspace keyboard layout language repeat caps numlock mouse "
               "pointer touchpad tapping dragging acceleration motion cursor kinetic scroll edge hover "
               + key
               + " "
               + inputTr(key)
           ),
           .visibleWhen = std::move(visible)}
      );
    };
    entries.push_back(
        {.section = SettingsSection::InputMotion,
         .group = "hyprland-input-presets",
         .title = inputTr("presets"),
         .subtitle = inputTr("presets-description"),
         .path = {"shell", "hyprland_input"},
         .control = ButtonSetting{},
         .searchText = "hyprland input motion presets subtle playful reduced"}
    );
    const auto inputEnum = [](const auto& options, auto selected) {
      SelectSetting control;
      for (const auto& option : options)
        control.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
      control.selectedValue = enumToKey(options, selected);
      return control;
    };
    const auto keyboardVisible = [](const Config& cfg) { return cfg.shell.hyprlandInput.keyboardManaged; };
    addInput("keyboard", "keyboard_managed", ToggleSetting{input.keyboardManaged});
    const std::vector<std::string> layouts{
        input.keyboardLayout, input.keyboardLayout2, input.keyboardLayout3, input.keyboardLayout4
    };
    for (std::size_t i = 0; i < layouts.size(); ++i) {
      SearchPickerSetting picker;
      if (i)
        picker.options.push_back({"none", inputTr("layout-none")});
      for (const auto& choice : keyboardLayoutCatalog())
        picker.options.push_back({choice.value, choice.label, choice.value});
      picker.selectedValue = layouts[i];
      picker.placeholder = inputTr("layout-search");
      picker.emptyText = inputTr("layout-not-found");
      addInput(
          "keyboard", i ? "keyboard_layout_" + std::to_string(i + 1) : "keyboard_layout", std::move(picker),
          keyboardVisible
      );
    }
    addInput(
        "keyboard", "keyboard_layout_switch", inputEnum(kHyprlandLayoutSwitch, input.keyboardLayoutSwitch),
        keyboardVisible
    );
    addInput(
        "keyboard", "keyboard_repeat_rate", SliderSetting{input.keyboardRepeatRate, 0, 200, 1, true}, keyboardVisible
    );
    addInput(
        "keyboard", "keyboard_repeat_delay", SliderSetting{input.keyboardRepeatDelay, 0, 2000, 25, true},
        keyboardVisible
    );
    addInput("keyboard", "keyboard_caps_lock", inputEnum(kHyprlandCapsLock, input.keyboardCapsLock), keyboardVisible);
    addInput("keyboard", "keyboard_num_lock", ToggleSetting{input.keyboardNumLock}, keyboardVisible);
    addInput("mouse", "mouse_managed", ToggleSetting{input.mouseManaged});
    addInput(
        "mouse", "pointer_sensitivity", SliderSetting{input.pointerSensitivity, -1, 1, .05, false},
        [](const Config& cfg) { return cfg.shell.hyprlandInput.mouseManaged; }
    );
    addInput(
        "mouse", "pointer_acceleration", inputEnum(kHyprlandPointerAcceleration, input.pointerAcceleration),
        [](const Config& cfg) { return cfg.shell.hyprlandInput.mouseManaged; }
    );
    addInput("mouse", "mouse_left_handed", ToggleSetting{input.mouseLeftHanded}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.mouseManaged;
    });
    addInput("mouse", "mouse_natural_scroll", ToggleSetting{input.mouseNaturalScroll}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.mouseManaged;
    });
    addInput(
        "mouse", "mouse_scroll_factor", SliderSetting{input.mouseScrollFactor, .1, 5, .1, false},
        [](const Config& cfg) { return cfg.shell.hyprlandInput.mouseManaged; }
    );
    addInput("touchpad", "touchpad_managed", ToggleSetting{input.touchpadManaged});
    addInput("touchpad", "touchpad_natural_scroll", ToggleSetting{input.touchpadNaturalScroll}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.touchpadManaged;
    });
    addInput(
        "touchpad", "touchpad_scroll_factor", SliderSetting{input.touchpadScrollFactor, .1, 5, .1, false},
        [](const Config& cfg) { return cfg.shell.hyprlandInput.touchpadManaged; }
    );
    addInput("touchpad", "touchpad_tap_to_click", ToggleSetting{input.touchpadTapToClick}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.touchpadManaged;
    });
    addInput("touchpad", "touchpad_tap_and_drag", ToggleSetting{input.touchpadTapAndDrag}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.touchpadManaged;
    });
    addInput(
        "touchpad", "touchpad_drag_lock", inputEnum(kHyprlandDragLock, input.touchpadDragLock),
        [](const Config& cfg) { return cfg.shell.hyprlandInput.touchpadManaged; }
    );
    addInput("touchpad", "touchpad_tap_map", inputEnum(kHyprlandTapMap, input.touchpadTapMap), [](const Config& cfg) {
      return cfg.shell.hyprlandInput.touchpadManaged;
    });
    addInput("touchpad", "touchpad_clickfinger", ToggleSetting{input.touchpadClickfinger}, [](const Config& cfg) {
      return cfg.shell.hyprlandInput.touchpadManaged;
    });
    addInput(
        "touchpad", "touchpad_middle_emulation", ToggleSetting{input.touchpadMiddleEmulation},
        [](const Config& cfg) { return cfg.shell.hyprlandInput.touchpadManaged; }
    );
    addInput(
        "touchpad", "touchpad_disable_while_typing", ToggleSetting{input.touchpadDisableWhileTyping},
        [](const Config& cfg) { return cfg.shell.hyprlandInput.touchpadManaged; }
    );
    addInput(
        "touchpad", "touchpad_drag_fingers", inputEnum(kHyprlandDragFingers, input.touchpadDragFingers),
        [](const Config& cfg) { return cfg.shell.hyprlandInput.touchpadManaged; }
    );
    const auto gesturesVisible = [](const Config& cfg) { return cfg.shell.hyprlandInput.gesturesManaged; };
    const auto workspaceGestureVisible = [](const Config& cfg) {
      return cfg.shell.hyprlandInput.gesturesManaged && cfg.shell.hyprlandInput.workspaceGestureEnabled;
    };
    const auto overviewGestureVisible = [](const Config& cfg) {
      return cfg.shell.hyprlandInput.gesturesManaged && cfg.shell.hyprlandInput.overviewGestureEnabled;
    };
    addInput("gestures", "gestures_managed", ToggleSetting{input.gesturesManaged});
    addInput("gestures", "workspace_gesture_enabled", ToggleSetting{input.workspaceGestureEnabled}, gesturesVisible);
    addInput(
        "gestures", "workspace_gesture_fingers", inputEnum(kHyprlandGestureFingers, input.workspaceGestureFingers),
        workspaceGestureVisible
    );
    addInput(
        "gestures", "workspace_gesture_sensitivity",
        SliderSetting{input.workspaceGestureSensitivity, .25, 3, .05, false}, workspaceGestureVisible
    );
    addInput(
        "gestures", "workspace_gesture_distance", SliderSetting{input.workspaceGestureDistance, 100, 1000, 25, true},
        workspaceGestureVisible
    );
    addInput(
        "gestures", "workspace_gesture_invert", ToggleSetting{input.workspaceGestureInvert}, workspaceGestureVisible
    );
    addInput("gestures", "overview_gesture_enabled", ToggleSetting{input.overviewGestureEnabled}, gesturesVisible);
    addInput(
        "gestures", "overview_gesture_fingers", inputEnum(kHyprlandGestureFingers, input.overviewGestureFingers),
        overviewGestureVisible
    );
    addInput(
        "gestures", "overview_gesture_direction", inputEnum(kHyprlandGestureDirection, input.overviewGestureDirection),
        overviewGestureVisible
    );
    addInput(
        "gestures", "overview_gesture_distance", SliderSetting{input.overviewGestureDistance, 30, 500, 10, true},
        overviewGestureVisible
    );
    addInput("cursor", "cursor_managed", ToggleSetting{input.cursorManaged});
    addInput("cursor", "cursor_enabled", ToggleSetting{input.cursorEnabled}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged;
    });
    SelectSetting cursorMode;
    for (const auto& option : kHyprlandCursorModes)
      cursorMode.options.push_back({std::string(option.key), i18n::tr(option.labelKey)});
    cursorMode.selectedValue = enumToKey(kHyprlandCursorModes, input.cursorMode);
    addInput("cursor", "cursor_mode", cursorMode, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged;
    });
    addInput("cursor", "tilt_limit", SliderSetting{input.tiltLimit, 500, 15000, 100, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged && i.cursorMode == HyprlandCursorMode::Tilt;
    });
    addInput("cursor", "tilt_angle", SliderSetting{input.tiltAngle, 0, 90, 1, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged && i.cursorMode == HyprlandCursorMode::Tilt;
    });
    addInput("cursor", "cursor_window", SliderSetting{input.cursorWindow, 10, 300, 10, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged;
    });
    addInput(
        "cursor", "stretch_limit", SliderSetting{input.stretchLimit, 500, 15000, 100, true}, [](const Config& cfg) {
          const auto& i = cfg.shell.hyprlandInput;
          return i.cursorManaged && i.cursorMode == HyprlandCursorMode::Stretch;
        }
    );
    addInput("cursor", "rotate_length", SliderSetting{input.rotateLength, 8, 100, 1, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged && i.cursorMode == HyprlandCursorMode::Rotate;
    });
    addInput("cursor", "shake_enabled", ToggleSetting{input.shakeEnabled}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged;
    });
    addInput(
        "cursor", "shake_threshold", SliderSetting{input.shakeThreshold, 1, 20, 0.5, false}, [](const Config& cfg) {
          const auto& i = cfg.shell.hyprlandInput;
          return i.cursorManaged && i.shakeEnabled;
        }
    );
    addInput("cursor", "shake_limit", SliderSetting{input.shakeLimit, 1, 12, 0.5, false}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged && i.shakeEnabled;
    });
    addInput("cursor", "shake_timeout", SliderSetting{input.shakeTimeout, 100, 5000, 100, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.cursorManaged && i.shakeEnabled;
    });
    addInput("scroll", "scroll_managed", ToggleSetting{input.scrollManaged});
    addInput("scroll", "scroll_enabled", ToggleSetting{input.scrollEnabled}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_decay", SliderSetting{input.scrollDecay, 0.5, 0.98, 0.01, false}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput(
        "scroll", "scroll_multiplier", SliderSetting{input.scrollMultiplier, 0.25, 3, 0.05, false},
        [](const Config& cfg) {
          const auto& i = cfg.shell.hyprlandInput;
          return i.scrollManaged;
        }
    );
    addInput("scroll", "scroll_cutoff", SliderSetting{input.scrollCutoff, 0.1, 5, 0.1, false}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_interval", SliderSetting{input.scrollInterval, 8, 32, 1, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_browser", ToggleSetting{input.scrollBrowser}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_stop_click", ToggleSetting{input.scrollStopClick}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_stop_focus", ToggleSetting{input.scrollStopFocus}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput("scroll", "scroll_stop_target", ToggleSetting{input.scrollStopTarget}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.scrollManaged;
    });
    addInput(
        "scroll", "scroll_excluded",
        TextSetting{.value = input.scrollExcluded, .placeholder = "steam, org.telegram.desktop"},
        [](const Config& cfg) {
          const auto& i = cfg.shell.hyprlandInput;
          return i.scrollManaged;
        }
    );
    addInput("edge", "edge_managed", ToggleSetting{input.edgeManaged});
    addInput("edge", "edge_enabled", ToggleSetting{input.edgeEnabled}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_left", ToggleSetting{input.edgeLeft}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_right", ToggleSetting{input.edgeRight}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_top", ToggleSetting{input.edgeTop}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_bottom", ToggleSetting{input.edgeBottom}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_distance", SliderSetting{input.edgeDistance, 0, 100, 1, true}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    SelectSetting edgeFocus;
    edgeFocus.options = {
        {"-1", inputTr("focus-follow")}, {"0", inputTr("focus-never")}, {"1", inputTr("focus-always")}
    };
    edgeFocus.selectedValue = std::to_string(input.edgeFocus);
    edgeFocus.valueType = SelectValueType::Integer;
    addInput("edge", "edge_focus", edgeFocus, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_click", ToggleSetting{input.edgeClick}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    addInput("edge", "edge_scroll", ToggleSetting{input.edgeScroll}, [](const Config& cfg) {
      const auto& i = cfg.shell.hyprlandInput;
      return i.edgeManaged;
    });
    const auto& c = current.shell.hyprlandAppearance;
    auto add = [&](std::string group, std::string key, SettingControl control, SettingVisibility visible = {}) {
      entries.push_back(
          {.section = SettingsSection::Appearance,
           .group = std::move(group),
           .title = tr(key),
           .subtitle = tr(key + "-description"),
           .path = path(key),
           .control = std::move(control),
           .searchText = "hyprland appearance animation curve bezier spring glass plugin preset",
           .visibleWhen = std::move(visible)}
      );
      entries.back().searchText += " " + entries.back().title + " " + entries.back().subtitle + " " + key;
      entries.back().searchText = StringUtils::toLower(entries.back().searchText);
    };
    for (auto& entry : entries)
      if (entry.path == path("animation_easing")) {
        auto& select = std::get<SelectSetting>(entry.control);
        std::erase_if(select.options, [](const auto& option) { return option.value == "inherit"; });
        select.groupedCommit = [resolved = curveFor(c, "curve")](std::string_view choice, const Path& primary) {
          AppearanceOverrides result{{primary, std::string(choice)}};
          if (choice == "custom")
            for (const auto& [name, value] : std::vector<std::pair<std::string, float>>{
                     {"x1", resolved.x1}, {"y1", resolved.y1}, {"x2", resolved.x2}, {"y2", resolved.y2}
                 }) {
              auto coordinate = path("curve");
              coordinate.push_back(name);
              result.emplace_back(coordinate, double(value));
            }
          return result;
        };
      }
    auto managed = [](const Config& config) { return config.shell.hyprlandAppearance.enabled; };
    auto motion = [](const Config& config) {
      return config.shell.hyprlandAppearance.enabled
          && config.shell.hyprlandAppearance.customAnimations
          && config.shell.hyprlandAppearance.animationsEnabled;
    };
    add("hyprland-presets", "enabled", ButtonSetting{}, {});
    for (const auto& [key, curve] : std::vector<std::pair<std::string, HyprlandMotionCurve>>{
             {"curve", c.curve},
             {"opening_curve", c.openingCurve},
             {"closing_curve", c.closingCurve},
             {"moving_curve", c.movingCurve},
             {"workspace_curve", c.workspaceCurve}
         }) {
      const auto group = key == "curve" ? "hyprland-animations" : "hyprland-" + key;
      if (key != "curve") {
        const std::string stem = key.substr(0, key.find('_'));
        float duration = stem == "opening" ? c.openingDuration
            : stem == "closing"            ? c.closingDuration
            : stem == "moving"             ? c.movingDuration
                                           : c.workspaceDuration;
        SliderSetting slider{duration, 50, 2000, 10, true};
        slider.valueSuffix = "ms";
        add(group, stem + "_duration", slider, motion);
        SelectSetting select;
        for (const auto& o : kHyprlandAnimationEasings) {
          select.options.push_back({std::string(o.key), i18n::tr(o.labelKey)});
          if (o.value == curve.easing)
            select.selectedValue = o.key;
        }
        select.groupedCommit = [resolved = curveFor(c, key), key](std::string_view choice, const Path& primary) {
          AppearanceOverrides result{{primary, std::string(choice)}};
          if (choice == "custom")
            for (const auto& [name, value] : std::vector<std::pair<std::string, float>>{
                     {"x1", resolved.x1}, {"y1", resolved.y1}, {"x2", resolved.x2}, {"y2", resolved.y2}
                 }) {
              auto coordinate = path(key);
              coordinate.push_back(name);
              result.emplace_back(coordinate, double(value));
            }
          return result;
        };
        add(group, key, std::move(select), motion);
        entries.back().path.push_back("easing");
        entries.back().title = tr("easing");
      }
      add(group, key, ButtonSetting{}, motion);
    }
    add("hyprland-glass", "glass_managed", ToggleSetting{c.glassManaged}, managed);
    auto glass = [](const Config& config) {
      return config.shell.hyprlandAppearance.enabled && config.shell.hyprlandAppearance.glassManaged;
    };
    add("hyprland-glass", "glass_enabled", ToggleSetting{c.glassEnabled}, glass);
    add("hyprland-glass", "glass_light", ToggleSetting{c.glassLight}, glass);
    add("hyprland-glass", "glass_layers", ToggleSetting{c.glassLayers}, glass);
    for (const auto& [key, value, max] : std::vector<std::tuple<std::string, float, float>>{
             {"glass_blur", c.glassBlur, 10},
             {"glass_refraction", c.glassRefraction, 2},
             {"glass_chromatic", c.glassChromatic, 2},
             {"glass_lens", c.glassLens, 2},
             {"glass_opacity", c.glassOpacity, 1},
             {"glass_fresnel", c.glassFresnel, 2},
             {"glass_specular", c.glassSpecular, 2}
         })
      add("hyprland-glass", key, SliderSetting{value, 0, max, .01, false}, glass);
    add("hyprland-plugins", "cursor_managed", ToggleSetting{c.cursorManaged},
        [managed](const Config& config) { return managed(config) && !config.shell.hyprlandInput.cursorManaged; });
    auto cursor = [](const Config& config) {
      return config.shell.hyprlandAppearance.enabled
          && config.shell.hyprlandAppearance.cursorManaged
          && !config.shell.hyprlandInput.cursorManaged;
    };
    add("hyprland-plugins", "cursor_enabled", ToggleSetting{c.cursorEnabled}, cursor);
    add("hyprland-plugins", "cursor_stretch", ToggleSetting{c.cursorStretch}, cursor);
    add("hyprland-plugins", "cursor_shake", ToggleSetting{c.cursorShake}, cursor);
    add("hyprland-plugins", "cursor_shake_limit", SliderSetting{c.cursorShakeLimit, 1, 20, .1, false}, cursor);
    add("hyprland-plugins", "overview_managed", ToggleSetting{c.overviewManaged}, managed);
    auto overview = [](const Config& config) {
      return config.shell.hyprlandAppearance.enabled && config.shell.hyprlandAppearance.overviewManaged;
    };
    add("hyprland-plugins", "overview_bottom", ToggleSetting{c.overviewBottom}, overview);
    add("hyprland-plugins", "overview_hide_layers", ToggleSetting{c.overviewHideLayers}, overview);
    add("hyprland-plugins", "overview_height", SliderSetting{c.overviewHeight, 80, 600, 10, true}, overview);
    add("hyprland-plugins", "overview_preset", ButtonSetting{}, managed);
    add("hyprland-plugins", "overview_style_managed", ToggleSetting{c.overviewStyleManaged}, overview);
    const auto overviewStyle = [overview](const Config& config) {
      return overview(config) && config.shell.hyprlandAppearance.overviewStyleManaged;
    };
    add("hyprland-plugins", "overview_centered", ToggleSetting{c.overviewCentered}, overviewStyle);
    add("hyprland-plugins", "overview_blur", ToggleSetting{c.overviewBlur}, overviewStyle);
    add("hyprland-plugins", "overview_margin", SliderSetting{c.overviewMargin, 0, 36, 1, true}, overviewStyle);
    add("hyprland-plugins", "overview_panel_border", SliderSetting{c.overviewPanelBorder, 0, 10, 1, true},
        overviewStyle);
    add("hyprland-plugins", "overview_workspace_border", SliderSetting{c.overviewWorkspaceBorder, 0, 10, 1, true},
        overviewStyle);
    add("hyprland-plugins", "overview_drag_opacity", SliderSetting{c.overviewDragOpacity, 0, 1, .01, false},
        overviewStyle);
    SliderSetting overviewDuration{c.overviewDuration, 0, 2000, 50, true};
    overviewDuration.valueSuffix = "ms";
    add("hyprland-plugins", "overview_duration", overviewDuration, overviewStyle);
    for (const auto& [key, color, opacityKey, opacity] :
         std::vector<std::tuple<std::string, std::optional<ColorSpec>, std::string, float>>{
             {"overview_panel_color", c.overviewPanelColor, "overview_panel_opacity", c.overviewPanelOpacity},
             {"overview_panel_border_color", c.overviewPanelBorderColor, "overview_panel_border_opacity",
              c.overviewPanelBorderOpacity},
             {"overview_active_background", c.overviewActiveBackground, "overview_active_background_opacity",
              c.overviewActiveBackgroundOpacity},
             {"overview_inactive_background", c.overviewInactiveBackground, "overview_inactive_background_opacity",
              c.overviewInactiveBackgroundOpacity},
             {"overview_active_border", c.overviewActiveBorder, "overview_active_border_opacity",
              c.overviewActiveBorderOpacity},
             {"overview_inactive_border", c.overviewInactiveBorder, "overview_inactive_border_opacity",
              c.overviewInactiveBorderOpacity}
         }) {
      add("hyprland-plugins", key,
          ColorSpecPickerSetting{
              .roles = {},
              .selectedValue = optionalColorSpecConfigValue(color),
              .allowNone = false,
              .allowCustomColor = true,
              .noneLabel = {}
          },
          overviewStyle);
      add("hyprland-plugins", opacityKey, SliderSetting{opacity, 0, 1, .01, false}, overviewStyle);
    }

    add("hyprland-shadow-glow", "decoration_effects_managed", ToggleSetting{c.decorationEffectsManaged}, managed);
    const auto effects = [](const Config& config) {
      return config.shell.hyprlandAppearance.enabled && config.shell.hyprlandAppearance.decorationEffectsManaged;
    };
    const auto shadow = [](const Config& config) {
      const auto& a = config.shell.hyprlandAppearance;
      return a.enabled && a.decorationEffectsManaged && a.shadowEnabled;
    };
    const auto glow = [](const Config& config) {
      const auto& a = config.shell.hyprlandAppearance;
      return a.enabled && a.decorationEffectsManaged && a.glowEnabled;
    };
    const auto picker = [&](const std::optional<ColorSpec>& color, bool inherit) {
      return ColorSpecPickerSetting{
          .roles = {},
          .selectedValue = optionalColorSpecConfigValue(color),
          .allowNone = inherit,
          .allowCustomColor = true,
          .noneLabel = tr("match-active")
      };
    };
    add("hyprland-shadow-glow", "shadow_color", picker(c.shadowColor, false), shadow);
    add("hyprland-shadow-glow", "shadow_inactive_color", picker(c.shadowInactiveColor, true), shadow);
    add("hyprland-shadow-glow", "shadow_opacity", SliderSetting{c.shadowOpacity, 0, 1, .01, false}, shadow);
    add("hyprland-shadow-glow", "shadow_inactive_opacity", SliderSetting{c.shadowInactiveOpacity, 0, 1, .01, false},
        shadow);
    add("hyprland-shadow-glow", "shadow_power", SliderSetting{c.shadowPower, 1, 4, 1, true}, shadow);
    add("hyprland-shadow-glow", "shadow_sharp", ToggleSetting{c.shadowSharp}, shadow);
    add("hyprland-shadow-glow", "shadow_offset_x", SliderSetting{c.shadowOffsetX, -250, 250, 1, true}, shadow);
    add("hyprland-shadow-glow", "shadow_offset_y", SliderSetting{c.shadowOffsetY, -250, 250, 1, true}, shadow);
    add("hyprland-shadow-glow", "shadow_scale", SliderSetting{c.shadowScale, 0, 1, .01, false}, shadow);
    add("hyprland-shadow-glow", "glow_enabled", ToggleSetting{c.glowEnabled}, effects);
    add("hyprland-shadow-glow", "glow_range", SliderSetting{c.glowRange, 0, 100, 1, true}, glow);
    add("hyprland-shadow-glow", "glow_power", SliderSetting{c.glowPower, 1, 4, 1, true}, glow);
    add("hyprland-shadow-glow", "glow_color", picker(c.glowColor, false), glow);
    add("hyprland-shadow-glow", "glow_inactive_color", picker(c.glowInactiveColor, true), glow);
    add("hyprland-shadow-glow", "glow_opacity", SliderSetting{c.glowOpacity, 0, 1, .01, false}, glow);
    add("hyprland-shadow-glow", "glow_inactive_opacity", SliderSetting{c.glowInactiveOpacity, 0, 1, .01, false}, glow);

    add("hyprland-blur-focus", "blur_focus_managed", ToggleSetting{c.blurFocusManaged}, managed);
    const auto focus = [](const Config& config) {
      const auto& a = config.shell.hyprlandAppearance;
      return a.enabled && a.blurFocusManaged;
    };
    const auto blur = [focus](const Config& config) {
      return focus(config) && config.shell.hyprlandAppearance.blurEnabled;
    };
    add("hyprland-blur-focus", "rounding_power", SliderSetting{c.roundingPower, 2, 10, .1, false}, focus);
    add("hyprland-blur-focus", "fullscreen_opacity", SliderSetting{c.fullscreenOpacity, 0, 1, .01, false}, focus);
    add("hyprland-blur-focus", "dim_inactive", ToggleSetting{c.dimInactive}, focus);
    add("hyprland-blur-focus", "dim_strength", SliderSetting{c.dimStrength, 0, 1, .01, false},
        [focus](const Config& config) { return focus(config) && config.shell.hyprlandAppearance.dimInactive; });
    add("hyprland-blur-focus", "dim_special", SliderSetting{c.dimSpecial, 0, 1, .01, false}, focus);
    add("hyprland-blur-focus", "blur_brightness", SliderSetting{c.blurBrightness, 0, 2, .01, false}, blur);
    add("hyprland-blur-focus", "blur_contrast", SliderSetting{c.blurContrast, 0, 2, .01, false}, blur);
    add("hyprland-blur-focus", "blur_vibrancy", SliderSetting{c.blurVibrancy, 0, 1, .01, false}, blur);
    add("hyprland-blur-focus", "blur_noise", SliderSetting{c.blurNoise, 0, 1, .001, false}, blur);
    add("hyprland-blur-focus", "blur_popups", ToggleSetting{c.blurPopups}, blur);
    add("hyprland-blur-focus", "blur_popups_ignorealpha", SliderSetting{c.blurPopupsIgnorealpha, 0, 1, .01, false},
        [blur](const Config& config) { return blur(config) && config.shell.hyprlandAppearance.blurPopups; });
    add("hyprland-blur-focus", "blur_special", ToggleSetting{c.blurSpecial}, blur);
    entries.push_back(
        {.section = SettingsSection::Appearance,
         .group = "hyprland-app-rules",
         .title = tr("app-rules"),
         .subtitle = tr("app-rules-hint"),
         .path = {"shell", "hyprland_app_rules"},
         .control = ButtonSetting{},
         .searchText = "hyprland application window rules opacity blur shadow glass terminal fullscreen dialog",
         .visibleWhen = managed}
    );
    entries.push_back(
        {.section = SettingsSection::Appearance,
         .group = "hyprland-theme-profiles",
         .title = tr("theme-profiles"),
         .subtitle = tr("profile-switching-hint"),
         .path = {"shell", "hyprland_profile_switching"},
         .control = ButtonSetting{},
         .searchText = "hyprland automatic day night light dark theme profiles manual override",
         .visibleWhen = managed}
    );
    // Automatic profiles own these values. Pause and copy the active look before
    // editing so controls never show or mutate an inactive underlying setting.
    for (auto& entry : entries) {
      if (entry.path.size() < 3
          || entry.path[0] != "shell"
          || entry.path[1] != "hyprland_appearance"
          || entry.group == "hyprland-presets"
          || entry.path.back() == "enabled")
        continue;
      const auto previous = entry.visibleWhen;
      entry.visibleWhen = [previous](const Config& config) {
        return !config.shell.hyprlandProfileSwitching.enabled && (!previous || previous(config));
      };
    }
  }

  std::unique_ptr<Node> makeHyprlandEditor(const SettingEntry& entry, const SettingsContentContext& ctx) {
    if (entry.group == "hyprland-input-presets") {
      const auto inputTr = [](std::string_view key) { return i18n::tr("settings.hyprland-input." + std::string(key)); };
      auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
      column->addChild(
          ui::label(
              {.text = inputTr("presets-description"), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 4}
          )
      );
      auto row = ui::row({.gap = 8 * ctx.scale});
      for (const std::string preset : {"subtle", "playful", "reduced"}) {
        row->addChild(
            ui::button(
                {.text = inputTr(preset),
                 .onClick = [c = ctx.config.shell.hyprlandInput, preset, commit = ctx.setOverrides] {
                   ShellConfig shell;
                   shell.hyprlandInput = compositors::hyprland::inputMotionPreset(c, preset);
                   const auto table =
                       noctalia::config::schema::writeTable(shell, noctalia::config::schema::shellSchema());
                   AppearanceOverrides values;
                   flatten(*table["hyprland_input"].as_table(), {"shell", "hyprland_input"}, values);
                   commit(std::move(values));
                 }}
            )
        );
      }
      column->addChild(std::move(row));
      return column;
    }
    if (entry.group == "hyprland-theme-profiles")
      return makeThemeProfilesEditor(ctx);
    if (entry.group == "hyprland-app-rules")
      return makeAppRuleEditor(ctx);
    if (entry.group == "hyprland-keybinds")
      return makeHyprlandKeybindEditor(ctx);
    if (entry.group == "hyprland-workspaces")
      return makeHyprlandWorkspaceEditor(ctx);
    if (entry.group == "hyprland-placement")
      return makePlacementEditor(ctx);
    if (!std::holds_alternative<ButtonSetting>(entry.control)
        || entry.path.size() != 3
        || entry.path[0] != "shell"
        || entry.path[1] != "hyprland_appearance")
      return nullptr;
    const auto key = entry.path.back();
    const auto c = compositors::hyprland::resolveAppearanceProfile(ctx.config.shell, isResolvedLightTheme()).appearance;
    auto column = ui::column({.gap = 10 * ctx.scale, .fillWidth = true});
    const auto button = [&](std::string label, std::function<void()> action) {
      return ui::button(
          {.text = std::move(label), .fontSize = Style::fontSizeBody * ctx.scale, .onClick = std::move(action)}
      );
    };
    if (key == "overview_preset") {
      column->addChild(button(tr("overview_preset"), [c, commit = ctx.setOverrides] {
        auto values = appearanceOverrides(softGlassOverview(c));
        std::erase_if(values, [](const auto& value) { return !value.first.back().starts_with("overview_"); });
        commit(std::move(values));
      }));
      return column;
    }
    if (entry.group == "hyprland-presets") {
      if (ctx.config.shell.hyprlandProfileSwitching.enabled)
        column->addChild(
            ui::label({.text = tr("profile-preset-hint"), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 3})
        );
      auto actions = ui::row({.gap = 8 * ctx.scale});
      actions->addChild(button(tr("undo"), [undo = ctx.hyprlandUndo, commit = ctx.setOverrides] {
        if (undo && *undo)
          commit(manualAppearanceOverrides(**undo));
      }));
      if (!ctx.config.shell.hyprlandProfileSwitching.enabled)
        actions->addChild(button(tr("reset-all"), [clear = ctx.clearOverride] { clear(root); }));
      column->addChild(std::move(actions));
      auto presets = ui::row({.gap = 8 * ctx.scale});
      for (const std::string name : {"minimal", "soft", "expressive", "soft-glass"}) {
        presets->addChild(
            button(tr(name), [c = HyprlandAppearanceConfig(c), name, commit = ctx.setOverrides]() mutable {
              c.enabled = true;
              c.customAnimations = true;
              c.animationsEnabled = true;
              c.animationSpeed = 1;
              c.openingCurve = {};
              c.closingCurve = {};
              c.movingCurve = {};
              c.workspaceCurve = {};
              c.openingDuration = 400;
              c.closingDuration = 250;
              c.movingDuration = 300;
              c.workspaceDuration = 400;
              if (name == "minimal") {
                c.gapsIn = 3;
                c.gapsOut = 6;
                c.rounding = 4;
                c.shadowRange = 4;
                c.blurEnabled = false;
                c.animationEasing = HyprlandAnimationEasing::Snappy;
                c.animationSpeed = 1.5F;
                c.windowAnimation = HyprlandWindowAnimation::Fade;
              }
              if (name == "soft") {
                c.gapsIn = 6;
                c.gapsOut = 12;
                c.rounding = 16;
                c.shadowRange = 12;
                c.blurEnabled = true;
                c.blurSize = 4;
                c.blurPasses = 2;
                c.animationEasing = HyprlandAnimationEasing::Smooth;
                c.windowAnimation = HyprlandWindowAnimation::Pop;
              }
              if (name == "expressive") {
                c.gapsIn = 8;
                c.gapsOut = 16;
                c.rounding = 20;
                c.shadowRange = 20;
                c.blurEnabled = true;
                c.animationEasing = HyprlandAnimationEasing::Spring;
                c.curve.stiffness = 220;
                c.curve.damping = 16;
                c.windowAnimation = HyprlandWindowAnimation::Slide;
              }
              if (name == "soft-glass")
                c = softGlassAppearance(c);
              commit(manualAppearanceOverrides(c));
            })
        );
      }
      column->addChild(std::move(presets));
      auto name = std::make_shared<std::string>();
      column->addChild(
          ui::input(
              {.placeholder = tr("profile-name"), .width = 300 * ctx.scale, .onChange = [name](const std::string& v) {
                 *name = v;
               }}
          )
      );
      column->addChild(button(tr("save-profile"), [name, c, set = ctx.setOverride] {
        if (name->empty() || name->size() > 80 || name->starts_with('@'))
          return;
        std::ostringstream text;
        text << appearanceTable(c);
        set({"shell", "hyprland_appearance_profiles", *name}, text.str());
      }));
      std::vector<std::string> names;
      for (const auto& [n, _] : ctx.config.shell.hyprlandAppearanceProfiles)
        names.push_back(n);
      std::ranges::sort(names);
      for (const auto& n : names) {
        auto row = ui::row({.gap = 8 * ctx.scale});
        const auto data = ctx.config.shell.hyprlandAppearanceProfiles.at(n);
        row->addChild(button(n, [data, commit = ctx.setOverrides] {
          try {
            ShellConfig shell;
            toml::table table;
            table.insert("hyprland_appearance", toml::parse(data));
            noctalia::config::schema::Diagnostics diagnostics;
            noctalia::config::schema::readInto(
                table, shell, noctalia::config::schema::shellSchema(), "shell", diagnostics
            );
            commit(manualAppearanceOverrides(shell.hyprlandAppearance));
          } catch (const toml::parse_error&) { /* Existing profile remains untouched. */
          }
        }));
        row->addChild(button(tr("delete-profile"), [n, clear = ctx.clearOverride] {
          clear({"shell", "hyprland_appearance_profiles", n});
        }));
        column->addChild(std::move(row));
      }
      auto resets = ui::column({.gap = 8 * ctx.scale});
      for (const std::string group : {"windows", "animations", "glass", "plugins", "shadow-glow", "blur-focus"}) {
        if (ctx.config.shell.hyprlandProfileSwitching.enabled)
          break;
        resets->addChild(button(tr("reset-" + group), [group, c, clear = ctx.clearOverrides] {
          std::vector<Path> paths;
          for (const auto& [p, _] : appearanceOverrides(c)) {
            const auto& k = p[2];
            bool plugin = k.starts_with("cursor_") || k.starts_with("overview_");
            bool glass = k.starts_with("glass_");
            bool effect = k.starts_with("shadow_") || k.starts_with("glow_") || k == "decoration_effects_managed";
            bool blurFocus =
                k.starts_with("blur_") || k.starts_with("dim_") || k == "fullscreen_opacity" || k == "rounding_power";
            bool animation = !plugin
                && (k.find("animation") != std::string::npos
                    || k.find("curve") != std::string::npos
                    || k.find("duration") != std::string::npos);
            if ((group == "shadow-glow" && effect)
                || (group == "blur-focus" && blurFocus)
                || (group == "glass" && glass)
                || (group == "plugins" && plugin)
                || (group == "animations" && animation)
                || (group == "windows" && !glass && !plugin && !animation && !effect && !blurFocus && k != "enabled"))
              paths.push_back(p);
          }
          clear(std::move(paths));
        }));
      }
      column->addChild(std::move(resets));
      return column;
    }
    if (key != "curve" && !key.ends_with("_curve"))
      return nullptr;
    auto curve = curveFor(c, key);
    column->addChild(ui::label({.text = tr("curve-hint"), .fontSize = Style::fontSizeBody * ctx.scale, .maxLines = 3}));
    const auto curvePath = path(key);
    const auto easingPath =
        key == "curve" ? path("animation_easing") : Path{"shell", "hyprland_appearance", key, "easing"};
    const auto commitCurve = [set = ctx.setOverrides, curvePath, easingPath](HyprlandMotionCurve v) {
      AppearanceOverrides values{{easingPath, std::string("custom")}};
      for (const auto& [k, n] :
           std::vector<std::pair<std::string, float>>{{"x1", v.x1}, {"y1", v.y1}, {"x2", v.x2}, {"y2", v.y2}}) {
        auto p = curvePath;
        p.push_back(k);
        values.emplace_back(p, double(n));
      }
      set(std::move(values));
    };
    float duration = key == "opening_curve" ? c.openingDuration
        : key == "closing_curve"            ? c.closingDuration
        : key == "moving_curve"             ? c.movingDuration
                                            : c.workspaceDuration;
    auto canvas = std::make_unique<CurveCanvas>(curve, ctx.scale, duration / 1000 / c.animationSpeed, commitCurve);
    auto* canvasPtr = canvas.get();
    column->addChild(std::move(canvas));
    column->addChild(button(tr("preview"), [canvasPtr] { canvasPtr->play(); }));
    SettingsControlFactory factory(ctx);
    if (curve.easing == HyprlandAnimationEasing::Spring) {
      for (const auto& [k, v, hi] : std::vector<std::tuple<std::string, float, float>>{
               {"stiffness", curve.stiffness, 1000}, {"damping", curve.damping, 100}
           }) {
        column->addChild(ui::label({.text = tr(k)}));
        auto p = curvePath;
        p.push_back(k);
        column->addChild(factory.makeSlider(v, 1, hi, 1, p, false, [easingPath, curvePath, curve, k](double) {
          auto other = curvePath;
          other.push_back(k == "stiffness" ? "damping" : "stiffness");
          return AppearanceOverrides{
              {easingPath, std::string("spring")}, {other, double(k == "stiffness" ? curve.damping : curve.stiffness)}
          };
        }));
      }
    } else {
      for (const auto& [k, v, lo, hi] : std::vector<std::tuple<std::string, float, float, float>>{
               {"x1", curve.x1, 0, 1}, {"y1", curve.y1, -1, 2}, {"x2", curve.x2, 0, 1}, {"y2", curve.y2, -1, 2}
           }) {
        column->addChild(ui::label({.text = k}));
        auto p = curvePath;
        p.push_back(k);
        column->addChild(factory.makeSlider(v, lo, hi, .01, p, false, [easingPath, curve, curvePath, k](double) {
          AppearanceOverrides result{{easingPath, std::string("custom")}};
          for (const auto& [n, value] : std::vector<std::pair<std::string, float>>{
                   {"x1", curve.x1}, {"y1", curve.y1}, {"x2", curve.x2}, {"y2", curve.y2}
               })
            if (n != k) {
              auto otherPath = curvePath;
              otherPath.push_back(n);
              result.emplace_back(otherPath, double(value));
            }
          return result;
        }));
      }
    }
    column->addChild(button(tr("reset-curve"), [clear = ctx.clearOverrides, curvePath, easingPath] {
      clear({curvePath, easingPath});
    }));
    return column;
  }
} // namespace settings
