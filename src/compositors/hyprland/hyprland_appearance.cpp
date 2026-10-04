#include "compositors/hyprland/hyprland_appearance.h"

#include "compositors/hyprland/hyprland_keybinds.h"
#include "compositors/hyprland/hyprland_runtime.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/log.h"
#include "system/keyboard_layout_catalog.h"
#include "wayland/surface.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace compositors::hyprland {
  namespace {
    constexpr Logger kLog("hyprland_appearance");
    std::string classMatch(std::string_view appClass) {
      std::string match = "^";
      for (const char ch : appClass) {
        if (std::string_view(R"(\.^$|()[]{}*+?)").find(ch) != std::string_view::npos)
          match += '\\';
        match += ch;
      }
      return match + '$';
    }

    // Scalar readers tolerate wrong types by retaining defaults. Compare the
    // supplied keys with the schema's output so a damaged profile cannot silently
    // become a different look. Partial profiles and integer-valued floats are valid.
    bool profileTypesMatch(const toml::table& input, const toml::table& parsed) {
      for (const auto& [key, node] : input) {
        const auto* expected = parsed.get(key.str());
        if (!expected)
          return false;
        if (const auto* table = node.as_table()) {
          if (!expected->is_table() || !profileTypesMatch(*table, *expected->as_table()))
            return false;
        } else if (expected->is_floating_point()) {
          if (!(node.is_integer() || node.is_floating_point()) || !std::isfinite(node.value<double>().value_or(NAN)))
            return false;
        } else if (node.type() != expected->type()) {
          return false;
        }
      }
      return true;
    }

    float bounded(float v, float lo, float hi, float fallback) {
      return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    }

    std::string rgbLiteral(Color color) {
      const auto byte = [](float channel) {
        return static_cast<unsigned>(std::lround(std::clamp(channel, 0.0F, 1.0F) * 255));
      };
      return std::format("\"rgb({:02x}{:02x}{:02x})\"", byte(color.r), byte(color.g), byte(color.b));
    }

    std::string effectColor(const ColorSpec& spec, float opacity, Color primary, Color surface) {
      auto color = resolveColorSpec(spec);
      if (spec.role == ColorRole::Primary) {
        color = primary;
        color.a *= spec.alpha;
      } else if (spec.role == ColorRole::Surface) {
        color = surface;
        color.a *= spec.alpha;
      }
      const auto byte = [](float channel) {
        return static_cast<unsigned>(std::lround(bounded(channel, 0, 1, 0) * 255));
      };
      return std::format(
          "\"rgba({:02x}{:02x}{:02x}{:02x})\"", byte(color.r), byte(color.g), byte(color.b),
          byte(color.a * bounded(opacity, 0, 1, 1))
      );
    }

    std::string decorationEffects(const HyprlandAppearanceConfig& c, Color primary, Color surface) {
      if (!c.decorationEffectsManaged)
        return {};
      const auto shadow = c.shadowColor.value_or(fixedColorSpec(hex("#1a1a1a")));
      const auto glow = c.glowColor.value_or(colorSpecFromRole(ColorRole::Primary));
      return std::format(
          "; hl.config({{decoration={{shadow={{render_power={},sharp={},offset={{{},{}}},scale={},"
          "color={},color_inactive={}}},glow={{enabled={},range={},render_power={},color={},color_inactive={}}}}}}})",
          c.shadowPower, c.shadowSharp, c.shadowOffsetX, c.shadowOffsetY, c.shadowScale,
          effectColor(shadow, c.shadowOpacity, primary, surface),
          effectColor(c.shadowInactiveColor.value_or(shadow), c.shadowInactiveOpacity, primary, surface), c.glowEnabled,
          c.glowRange, c.glowPower, effectColor(glow, c.glowOpacity, primary, surface),
          effectColor(c.glowInactiveColor.value_or(glow), c.glowInactiveOpacity, primary, surface)
      );
    }

    std::string blurFocus(const HyprlandAppearanceConfig& c) {
      if (!c.blurFocusManaged)
        return {};
      return std::format(
          "; hl.config({{decoration={{rounding_power={},fullscreen_opacity={},dim_inactive={},dim_strength={},"
          "dim_special={},blur={{brightness={},contrast={},vibrancy={},noise={},popups={},special={},"
          "popups_ignorealpha={}}}}}}})",
          c.roundingPower, c.fullscreenOpacity, c.dimInactive, c.dimStrength, c.dimSpecial, c.blurBrightness,
          c.blurContrast, c.blurVibrancy, c.blurNoise, c.blurPopups, c.blurSpecial, c.blurPopupsIgnorealpha
      );
    }

    std::string animationCommands(const HyprlandAppearanceConfig& c) {
      if (!c.customAnimations)
        return {};

      const float multiplier = std::isfinite(c.animationSpeed) ? std::clamp(c.animationSpeed, 0.25F, 3.0F) : 1.0F;
      std::string command;
      auto global = c.curve;
      global.easing = c.animationEasing;
      const auto curve = [&](std::string_view name, HyprlandMotionCurve config) {
        if (config.easing == HyprlandAnimationEasing::Inherit)
          config = global;
        if (config.easing == HyprlandAnimationEasing::Spring) {
          command += std::format(
              "; hl.curve(\"{}\",{{type=\"spring\",mass=1,stiffness={},dampening={}}})", name,
              bounded(config.stiffness, 1, 1000, 200), bounded(config.damping, 1, 100, 20)
          );
        } else {
          config = resolvedMotionCurve(config);
          command += std::format(
              "; hl.curve(\"{}\",{{type=\"bezier\",points={{{{{},{}}},{{{},{}}}}}}})", name, config.x1, config.y1,
              config.x2, config.y2
          );
        }
        return config.easing == HyprlandAnimationEasing::Spring;
      };
      const bool globalSpring = curve("noctalia_window_motion", global);
      const bool openingSpring = curve("noctalia_opening", c.openingCurve);
      const bool closingSpring = curve("noctalia_closing", c.closingCurve);
      const bool movingSpring = curve("noctalia_moving", c.movingCurve);
      const bool workspaceSpring = curve("noctalia_workspace", c.workspaceCurve);
      const auto animation = [&](std::string_view leaf, float duration, std::string_view style, bool enabled = true,
                                 std::string_view name = "noctalia_window_motion", bool spring = false) {
        command += std::format(
            "; hl.animation({{leaf=\"{}\",enabled={},speed={:.4f},{}=\"{}\",style=\"{}\"}})", leaf, enabled,
            duration / multiplier, spring ? "spring" : "bezier", name, style
        );
      };

      std::string_view windowStyle = "popin 90%";
      switch (c.windowAnimation) {
      case HyprlandWindowAnimation::Slide:
        windowStyle = "slide";
        break;
      case HyprlandWindowAnimation::Fade:
        windowStyle = "popin 100%";
        break;
      case HyprlandWindowAnimation::Pop:
      case HyprlandWindowAnimation::Instant:
        break;
      }
      const bool windowMotion = c.windowAnimation != HyprlandWindowAnimation::Instant;
      animation("windows", 4.0F, "", true, "noctalia_window_motion", globalSpring);
      animation(
          "windowsMove", bounded(c.movingDuration, 50, 2000, 300) / 100, "", true, "noctalia_moving", movingSpring
      );
      animation(
          "windowsIn", bounded(c.openingDuration, 50, 2000, 400) / 100, windowStyle, windowMotion, "noctalia_opening",
          openingSpring
      );
      animation(
          "windowsOut", bounded(c.closingDuration, 50, 2000, 250) / 100, windowStyle, windowMotion, "noctalia_closing",
          closingSpring
      );
      animation(
          "fadeIn", bounded(c.openingDuration, 50, 2000, 400) / 200, "", windowMotion, "noctalia_opening", openingSpring
      );
      animation(
          "fadeOut", bounded(c.closingDuration, 50, 2000, 250) / 166.6667F, "", windowMotion, "noctalia_closing",
          closingSpring
      );
      animation("border", 2.5F, "", true, "noctalia_window_motion", globalSpring);

      std::string_view workspaceStyle = "slide";
      switch (c.workspaceAnimation) {
      case HyprlandWorkspaceAnimation::Vertical:
        workspaceStyle = "slidevert";
        break;
      case HyprlandWorkspaceAnimation::Fade:
        workspaceStyle = "fade";
        break;
      case HyprlandWorkspaceAnimation::Slide:
      case HyprlandWorkspaceAnimation::Instant:
        break;
      }
      for (const auto leaf :
           {"workspaces", "workspacesIn", "workspacesOut", "specialWorkspace", "specialWorkspaceIn",
            "specialWorkspaceOut"}) {
        animation(
            leaf, bounded(c.workspaceDuration, 50, 2000, 400) / 100, workspaceStyle,
            c.workspaceAnimation != HyprlandWorkspaceAnimation::Instant, "noctalia_workspace", workspaceSpring
        );
      }
      return command;
    }
  } // namespace

  AppearanceProfileResolution resolveAppearanceProfile(const ShellConfig& shell, bool light) {
    AppearanceProfileResolution result{.appearance = shell.hyprlandAppearance};
    const auto& switching = shell.hyprlandProfileSwitching;
    if (!switching.enabled)
      return result;
    result.profile = light ? switching.lightProfile : switching.darkProfile;
    auto& c = result.appearance;
    if (result.profile == "@soft-glass-dark" || result.profile == "@soft-glass-light") {
      c.glassManaged = true;
      c.glassLight = result.profile == "@soft-glass-light";
      c.blurFocusManaged = c.decorationEffectsManaged = true;
      c.blurBrightness = .95F;
      c.blurContrast = .9F;
      c.blurVibrancy = .2F;
      c.blurNoise = .015F;
      c.dimStrength = .08F;
      c.shadowOpacity = .55F;
      c.shadowInactiveOpacity = .35F;
      c.glowOpacity = .16F;
      c.glowInactiveOpacity = .04F;
      c.glassBlur = 1.5F;
      c.glassRefraction = .35F;
      c.glassChromatic = .15F;
      c.glassFresnel = .6F;
      c.glassSpecular = .8F;
      if (c.glassLight) {
        c.blurFocusManaged = c.decorationEffectsManaged = true;
        c.blurBrightness = 1.04F;
        c.blurContrast = .85F;
        c.blurVibrancy = .12F;
        c.blurNoise = .008F;
        c.dimStrength = .04F;
        c.shadowOpacity = .2F;
        c.shadowInactiveOpacity = .12F;
        c.glowOpacity = .08F;
        c.glowInactiveOpacity = .02F;
        c.glassBlur = 1.2F;
        c.glassRefraction = .25F;
        c.glassChromatic = .08F;
        c.glassFresnel = .4F;
        c.glassSpecular = .45F;
      }
      return result;
    }
    const auto profile = shell.hyprlandAppearanceProfiles.find(result.profile);
    if (profile != shell.hyprlandAppearanceProfiles.end()) {
      try {
        ShellConfig parsed;
        toml::table table;
        table.insert("hyprland_appearance", toml::parse(profile->second));
        noctalia::config::schema::Diagnostics diagnostics;
        noctalia::config::schema::readInto(
            table, parsed, noctalia::config::schema::shellSchema(), "shell", diagnostics
        );
        // Schema recovery can report malformed values as warnings. A profile is
        // a complete look, so reject partial recovery instead of applying defaults.
        if (diagnostics.entries.empty()
            && profileTypesMatch(
                table, noctalia::config::schema::writeTable(parsed, noctalia::config::schema::shellSchema())
            )) {
          c = parsed.hyprlandAppearance;
          // The master management switch always remains under the user's control.
          c.enabled = shell.hyprlandAppearance.enabled;
          return result;
        }
      } catch (const toml::parse_error&) {
      } catch (const std::runtime_error&) {
        // ColorSpec readers reject malformed values by throwing.
      }
    }
    result.fallback = true;
    return result;
  }

  HyprlandMotionCurve resolvedMotionCurve(HyprlandMotionCurve c) {
    switch (c.easing) {
    case HyprlandAnimationEasing::Snappy:
      c.x1 = .16F;
      c.y1 = 1;
      c.x2 = .3F;
      c.y2 = 1;
      break;
    case HyprlandAnimationEasing::Gentle:
      c.x1 = .45F;
      c.y1 = 0;
      c.x2 = .55F;
      c.y2 = 1;
      break;
    case HyprlandAnimationEasing::Linear:
      c.x1 = 0;
      c.y1 = 0;
      c.x2 = 1;
      c.y2 = 1;
      break;
    case HyprlandAnimationEasing::Custom:
      break;
    default:
      c.x1 = .22F;
      c.y1 = 1;
      c.x2 = .36F;
      c.y2 = 1;
      break;
    }
    c.x1 = bounded(c.x1, 0, 1, .22F);
    c.x2 = bounded(c.x2, 0, 1, .36F);
    c.y1 = bounded(c.y1, -1, 2, 1);
    c.y2 = bounded(c.y2, -1, 2, 1);
    c.stiffness = bounded(c.stiffness, 1, 1000, 200);
    c.damping = bounded(c.damping, 1, 100, 20);
    return c;
  }

  std::string pluginAppearanceCommands(const HyprlandAppearanceConfig& c, Color primary, Color surface) {
    std::string out;
    // Layer glass in "auto": a surface with a blur region (the dock, OSDs) is glassed there, one
    // that opts out with an empty region (the wallpaper, a solid Island) is not, and a glass
    // Island, which sends no region, is glassed by its own alpha so curves stay smooth.
    if (c.glassManaged)
      out += std::format(
          "; if hl.plugin.hyprglass then hl.plugin.hyprglass.config({{enabled={},default_theme=\"{}\","
          "blur_strength={},refraction_strength={},chromatic_aberration={},lens_distortion={},"
          "glass_opacity={},fresnel_strength={},specular_strength={},layers={{enabled={},mask_mode=\"auto\"}}}}) end",
          c.glassEnabled, c.glassLight ? "light" : "dark", c.glassBlur, c.glassRefraction, c.glassChromatic,
          c.glassLens, c.glassOpacity, c.glassFresnel, c.glassSpecular, c.glassLayers
      );
    if (c.cursorManaged)
      out += std::format(
          "; for _,p in ipairs(hl.get_loaded_plugins()) do if p.name==\"dynamic-cursors\" then "
          "hl.config({{plugin={{dynamic_cursors={{enabled={},mode=\"{}\",shake={{enabled={},limit={}}}}}}}}}) end end",
          c.cursorEnabled, c.cursorStretch ? "stretch" : "tilt", c.cursorShake, c.cursorShakeLimit
      );
    if (c.overviewManaged)
      out += std::format(
          "; if hl.plugin.overview then "
          "hl.config({{plugin={{overview={{panelHeight={},onBottom={},hideTopLayers={},hideOverlayLayers={}}}}}}}) end",
          c.overviewHeight, c.overviewBottom, c.overviewHideLayers, c.overviewHideLayers
      );
    if (c.overviewManaged && c.overviewStyleManaged) {
      const HyprlandAppearanceConfig defaults;
      const auto color = [&](const std::optional<ColorSpec>& value, const std::optional<ColorSpec>& fallback,
                             float opacity) {
        return effectColor(value.value_or(*fallback), opacity, primary, surface);
      };
      out += std::format(
          "; if hl.plugin.overview then hl.config({{plugin={{overview={{workspaceMargin={},panelBorderWidth={},"
          "workspaceBorderSize={},centerAligned={},disableBlur={},dragAlpha={},overrideAnimSpeed={},"
          "panelColor={},panelBorderColor={},workspaceActiveBackground={},workspaceInactiveBackground={},"
          "workspaceActiveBorder={},workspaceInactiveBorder={}}}}}}}) end",
          c.overviewMargin, c.overviewPanelBorder, c.overviewWorkspaceBorder, c.overviewCentered, !c.overviewBlur,
          c.overviewDragOpacity, c.overviewDuration / 100,
          color(c.overviewPanelColor, defaults.overviewPanelColor, c.overviewPanelOpacity),
          color(c.overviewPanelBorderColor, defaults.overviewPanelBorderColor, c.overviewPanelBorderOpacity),
          color(c.overviewActiveBackground, defaults.overviewActiveBackground, c.overviewActiveBackgroundOpacity),
          color(c.overviewInactiveBackground, defaults.overviewInactiveBackground, c.overviewInactiveBackgroundOpacity),
          color(c.overviewActiveBorder, defaults.overviewActiveBorder, c.overviewActiveBorderOpacity),
          color(c.overviewInactiveBorder, defaults.overviewInactiveBorder, c.overviewInactiveBorderOpacity)
      );
    }
    return out;
  }

  std::string appearanceCommand(const HyprlandAppearanceConfig& c, Color primary, Color surface) {
    if (!c.enabled)
      return {};
    const auto colors = c.followTheme
        ? std::format(",col={{active_border={},inactive_border={}}}", rgbLiteral(primary), rgbLiteral(surface))
        : std::string{};
    return std::format(
               "repl hl.config({{general={{gaps_in={},gaps_out={},border_size={}{}}},"
               "decoration={{rounding={},active_opacity={},inactive_opacity={},"
               "blur={{enabled={},size={},passes={}}},shadow={{enabled={},range={}}}}},"
               "animations={{enabled={}}}}})",
               c.gapsIn, c.gapsOut, c.borderSize, colors, c.rounding, c.activeOpacity, c.inactiveOpacity, c.blurEnabled,
               c.blurSize, c.blurPasses, c.shadowEnabled, c.shadowRange, c.animationsEnabled
           )
        + animationCommands(c)
        + decorationEffects(c, primary, surface)
        + blurFocus(c)
        + pluginAppearanceCommands(c, primary, surface);
  }

  std::string appRuleCommands(const std::vector<HyprlandAppRule>& rules) {
    auto ordered = rules;
    std::ranges::sort(ordered, [](const auto& a, const auto& b) {
      return a.scope == b.scope ? a.name < b.name : a.scope < b.scope;
    });
    std::string result;
    std::set<std::string> glassApps;
    for (const auto& r : ordered) {
      if (!r.enabled || r.appClass.empty() || r.appClass.size() > 512)
        continue;
      const auto scope = r.scope == HyprlandRuleScope::Fullscreen ? ",fullscreen=true"
          : r.scope == HyprlandRuleScope::Floating                ? ",float=true"
                                                                  : "";
      result += std::format(
          "; hl.window_rule({{name={},match={{class={}{}}}", luaStringLiteral("noctalia-app-" + r.name),
          luaStringLiteral(classMatch(r.appClass)), scope
      );
      if (r.opacityManaged)
        result += std::format(
            ",opacity=\"{} override {} override {} override\"", bounded(r.activeOpacity, .2F, 1, 1),
            bounded(r.inactiveOpacity, .2F, 1, 1), bounded(r.fullscreenOpacity, .2F, 1, 1)
        );
      if (r.roundingManaged)
        result += std::format(",rounding={}", std::clamp(r.rounding, 0, 20));
      for (const auto& [key, value] : std::initializer_list<std::pair<std::string_view, HyprlandRuleSwitch>>{
               {"no_blur", r.blur}, {"no_shadow", r.shadow}, {"no_dim", r.dim}, {"no_anim", r.animations}
           })
        if (value != HyprlandRuleSwitch::Inherit)
          result += std::format(",{}={}", key, value == HyprlandRuleSwitch::Off);
      if (r.glass != HyprlandRuleSwitch::Inherit)
        glassApps.insert(r.appClass);
      result += "})";
    }
    // 0.56 tracks dynamic tag cleanup by the raw rule value. A '+' prefix leaves
    // stale tags behind. Bare tags clean up correctly, but toggle on application;
    // mutually exclusive state rules prevent overlapping Noctalia rules from
    // toggling the same tag twice. Hyprglass's disabled-tag precedence is retained.
    for (const auto& app : glassApps)
      for (int state = 0; state < 4; ++state) {
        const bool floating = state & 1, fullscreen = state & 2;
        auto glass = HyprlandRuleSwitch::Inherit;
        for (const auto& r : ordered) {
          if (!r.enabled
              || r.appClass != app
              || r.glass == HyprlandRuleSwitch::Inherit
              || (r.scope == HyprlandRuleScope::Floating && !floating)
              || (r.scope == HyprlandRuleScope::Fullscreen && !fullscreen))
            continue;
          if (glass != HyprlandRuleSwitch::Off)
            glass = r.glass;
        }
        if (glass == HyprlandRuleSwitch::Inherit)
          continue;
        result += std::format(
            "; hl.window_rule({{name={},match={{class={},float={},fullscreen={}}},tag=\"hyprglass_{}\"}})",
            luaStringLiteral("noctalia-glass-" + app + "-" + std::to_string(state)), luaStringLiteral(classMatch(app)),
            floating, fullscreen, glass == HyprlandRuleSwitch::On ? "enabled" : "disabled"
        );
      }
    // Named rules append effects on repeated calls in 0.56. The Lua VM is reset
    // on config reload, so this marker also makes theme/reconnect reapplication
    // idempotent without accumulating effects or toggling tags twice.
    return result.empty()
        ? result
        : "; if not _G.__noctalia_app_appearance_v1 then " + result + "; _G.__noctalia_app_appearance_v1=true end";
  }

  std::string placementRuleProblem(const HyprlandPlacementRule& r) {
    if (r.appClass.empty() || r.appClass.size() > 512)
      return "invalid-app";
    if ((r.workspace == HyprlandPlacementWorkspace::Named || r.workspace == HyprlandPlacementWorkspace::Special)
        && (r.workspaceName.empty()
            || r.workspaceName.size() > 128
            || r.workspaceName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")
                != std::string::npos))
      return "invalid-workspace";
    return {};
  }

  std::string placementRuleCommands(const std::vector<HyprlandPlacementRule>& rules) {
    auto ordered = rules;
    std::ranges::sort(ordered, {}, &HyprlandPlacementRule::name);
    std::string result;
    for (const auto& r : ordered) {
      if (!r.enabled || !placementRuleProblem(r).empty())
        continue;
      std::string effects;
      if (r.mode != HyprlandPlacementMode::Inherit)
        effects += r.mode == HyprlandPlacementMode::Floating ? ",float=true" : ",tile=true";
      if (r.workspace != HyprlandPlacementWorkspace::Inherit) {
        auto workspace = r.workspace == HyprlandPlacementWorkspace::Number
            ? std::to_string(std::clamp(r.workspaceNumber, 1, 1000))
            : (r.workspace == HyprlandPlacementWorkspace::Named ? "name:" : "special:") + r.workspaceName;
        if (r.workspaceSilent)
          workspace += " silent";
        effects += ",workspace=" + luaStringLiteral(workspace);
      }
      if (r.mode != HyprlandPlacementMode::Tiled) {
        if (r.sizeManaged)
          effects += std::format(",size={{{},{}}}", std::clamp(r.width, 100, 8192), std::clamp(r.height, 100, 8192));
        if (r.position == HyprlandPlacementPosition::Center)
          effects += ",center=true";
        else if (r.position == HyprlandPlacementPosition::Offset)
          effects += std::format(",move={{{},{}}}", std::clamp(r.x, 0, 16384), std::clamp(r.y, 0, 16384));
        if (r.pin != HyprlandRuleSwitch::Inherit)
          effects += std::format(",pin={}", r.pin == HyprlandRuleSwitch::On);
      }
      if (!effects.empty())
        result += std::format(
            "; hl.window_rule({{name={},match={{class={}}}{}}})", luaStringLiteral("noctalia-placement-" + r.name),
            luaStringLiteral(classMatch(r.appClass)), effects
        );
    }
    return result.empty()
        ? result
        : "; if not _G.__noctalia_app_placement_v1 then " + result + "; _G.__noctalia_app_placement_v1=true end";
  }

  HyprlandAppearance::HyprlandAppearance(HyprlandRuntime& runtime) : HyprlandEventHandler(runtime) {}

  HyprlandInputConfig inputMotionPreset(HyprlandInputConfig c, std::string_view preset) {
    if (preset != "subtle" && preset != "playful" && preset != "reduced")
      return c;
    c.cursorManaged = c.scrollManaged = true;
    c.cursorEnabled = true;
    c.cursorMode = preset == "playful" ? HyprlandCursorMode::Stretch
        : preset == "reduced"          ? HyprlandCursorMode::None
                                       : HyprlandCursorMode::Tilt;
    c.tiltLimit = 7000;
    c.tiltAngle = 25;
    c.cursorWindow = 100;
    c.stretchLimit = 2500;
    c.rotateLength = 32;
    c.shakeEnabled = preset != "reduced";
    c.shakeThreshold = 6;
    c.shakeLimit = preset == "playful" ? 6 : 4;
    c.shakeTimeout = 1500;
    c.scrollEnabled = preset != "reduced";
    c.scrollDecay = preset == "playful" ? .94F : .90F;
    c.scrollMultiplier = preset == "playful" ? 1.4F : 1.1F;
    c.scrollCutoff = .5F;
    c.scrollInterval = 16;
    c.scrollStopClick = c.scrollStopTarget = true;
    return c;
  }

  std::string keyboardInputCommands(const HyprlandInputConfig& c) {
    if (!c.keyboardManaged)
      return {};
    std::string layouts, variants, options;
    std::vector<std::string> used;
    for (const auto& value : {c.keyboardLayout, c.keyboardLayout2, c.keyboardLayout3, c.keyboardLayout4}) {
      if (value.empty() || value == "none" || std::ranges::find(used, value) != used.end())
        continue;
      const auto& catalog = keyboardLayoutCatalog();
      if (std::ranges::find(catalog, value, &KeyboardLayoutChoice::value) == catalog.end())
        return {};
      const auto colon = value.find(':');
      if (!used.empty()) {
        layouts += ',';
        variants += ',';
      }
      layouts += value.substr(0, colon);
      if (colon != std::string::npos)
        variants += value.substr(colon + 1);
      used.push_back(value);
    }
    if (layouts.empty())
      return {};
    switch (c.keyboardLayoutSwitch) {
    case HyprlandLayoutSwitch::None:
      break;
    case HyprlandLayoutSwitch::AltShift:
      options = "grp:alt_shift_toggle";
      break;
    case HyprlandLayoutSwitch::CtrlShift:
      options = "grp:ctrl_shift_toggle";
      break;
    case HyprlandLayoutSwitch::SuperSpace:
      options = "grp:win_space_toggle";
      break;
    case HyprlandLayoutSwitch::AltSpace:
      options = "grp:alt_space_toggle";
      break;
    case HyprlandLayoutSwitch::BothShifts:
      options = "grp:shifts_toggle";
      break;
    }
    std::string caps;
    switch (c.keyboardCapsLock) {
    case HyprlandCapsLock::Normal:
      break;
    case HyprlandCapsLock::Escape:
      caps = "caps:escape";
      break;
    case HyprlandCapsLock::Control:
      caps = "ctrl:nocaps";
      break;
    case HyprlandCapsLock::SwapEscape:
      caps = "caps:swapescape";
      break;
    case HyprlandCapsLock::SwapControl:
      caps = "ctrl:swapcaps";
      break;
    case HyprlandCapsLock::Disabled:
      caps = "caps:none";
      break;
    }
    if (!caps.empty()) {
      if (!options.empty())
        options += ',';
      options += caps;
    }
    return std::format(
        "; "
        "hl.config({{input={{kb_file=\"\",kb_layout={},kb_variant={},kb_options={},repeat_rate={},repeat_delay={},"
        "numlock_by_default={}}}}})",
        luaStringLiteral(layouts), luaStringLiteral(variants), luaStringLiteral(options),
        std::clamp(c.keyboardRepeatRate, 0, 200), std::clamp(c.keyboardRepeatDelay, 0, 2000), c.keyboardNumLock
    );
  }

  std::string inputMotionCommands(const HyprlandInputConfig& c) {
    std::string out = keyboardInputCommands(c) + gestureInputCommands(c);
    if (c.mouseManaged)
      out += std::format(
          "; "
          "hl.config({{input={{sensitivity={},accel_profile={},left_handed={},natural_scroll={},scroll_factor={}}}}})",
          c.pointerSensitivity,
          luaStringLiteral(
              c.pointerAcceleration == HyprlandPointerAcceleration::Default
                  ? ""
                  : enumToKey(kHyprlandPointerAcceleration, c.pointerAcceleration)
          ),
          c.mouseLeftHanded, c.mouseNaturalScroll, c.mouseScrollFactor
      );
    if (c.touchpadManaged)
      out += std::format(
          "; hl.config({{input={{touchpad={{natural_scroll={},scroll_factor={},tap_to_click={},tap_and_drag={},"
          "drag_lock={},tap_button_map={},clickfinger_behavior={},middle_button_emulation={},disable_while_typing={},"
          "drag_3fg={}}}}}}})",
          c.touchpadNaturalScroll, c.touchpadScrollFactor, c.touchpadTapToClick, c.touchpadTapAndDrag,
          static_cast<int>(c.touchpadDragLock),
          luaStringLiteral(
              c.touchpadTapMap == HyprlandTapMap::Default ? "" : enumToKey(kHyprlandTapMap, c.touchpadTapMap)
          ),
          c.touchpadClickfinger, c.touchpadMiddleEmulation, c.touchpadDisableWhileTyping,
          static_cast<int>(c.touchpadDragFingers)
      );
    if (c.cursorManaged)
      out += std::format(
          "; for _,p in ipairs(hl.get_loaded_plugins()) do if p.name==\"dynamic-cursors\" then "
          "hl.config({{plugin={{dynamic_cursors={{enabled={},mode={},tilt={{limit={},full={},window={}}},"
          "stretch={{limit={},window={}}},rotate={{length={}}},shake={{enabled={},threshold={},limit={},timeout={}}}}}}"
          "}}}) end end",
          c.cursorEnabled, luaStringLiteral(enumToKey(kHyprlandCursorModes, c.cursorMode)), c.tiltLimit, c.tiltAngle,
          c.cursorWindow, c.stretchLimit, c.cursorWindow, c.rotateLength, c.shakeEnabled, c.shakeThreshold,
          c.shakeLimit, c.shakeTimeout
      );
    if (c.scrollManaged)
      out += std::format(
          "; if hl.plugin.kinetic_scroll then hl.config({{plugin={{kinetic_scroll={{enabled={},decel={},"
          "delta_multiplier={},min_velocity={},interval_ms={},disable_in_browser={},stop_on_click={},"
          "stop_on_focus={},stop_on_target_change={},disabled_classes={}}}}}}}) end",
          c.scrollEnabled, c.scrollDecay, c.scrollMultiplier, c.scrollCutoff, c.scrollInterval, c.scrollBrowser,
          c.scrollStopClick, c.scrollStopFocus, c.scrollStopTarget, luaStringLiteral(c.scrollExcluded)
      );
    if (c.edgeManaged) {
      std::string edges;
      if (c.edgeLeft)
        edges += 'l';
      if (c.edgeRight)
        edges += 'r';
      if (c.edgeTop)
        edges += 't';
      if (c.edgeBottom)
        edges += 'b';
      std::string events = "hover,keyboard";
      if (c.edgeClick)
        events += ",click";
      if (c.edgeScroll)
        events += ",scroll";
      out += std::format(
          "; for _,p in ipairs(hl.get_loaded_plugins()) do if p.name==\"hypr-edgehover\" then "
          "hl.config({{plugin={{hypr_edgehover={{enabled={},edges={},max_distance={},keyboard_focus={},gap_pass={}}}}}}"
          "}) end end",
          c.edgeEnabled, luaStringLiteral(edges), c.edgeDistance, c.edgeFocus, luaStringLiteral(events)
      );
    }
    return out;
  }

  std::string windowBehaviourCommands(const HyprlandWindowBehaviourConfig& c) {
    std::string out;
    if (c.focusManaged)
      out += std::format(
          "; hl.config({{input={{follow_mouse={},follow_mouse_threshold={},mouse_refocus={}}}}})", c.focusMode,
          c.focusThreshold, c.mouseRefocus
      );
    if (c.resizeManaged)
      out += std::format(
          "; hl.config({{general={{resize_on_border={},extend_border_grab_area={},hover_icon_on_border={}}}}})",
          c.resizeOnBorder, c.borderGrab, c.borderCursor
      );
    if (c.snapManaged)
      out += std::format(
          "; "
          "hl.config({{general={{snap={{enabled={},window_gap={},monitor_gap={},border_overlap={},respect_gaps={}}}}}}}"
          ")",
          c.snapEnabled, c.snapWindowDistance, c.snapMonitorDistance, c.snapBorderOverlap, c.snapRespectGaps
      );
    if (c.activationManaged)
      out += std::format("; hl.config({{misc={{focus_on_activate={}}}}})", c.focusOnActivate);
    return out;
  }

  std::string tilingCommands(const HyprlandTilingConfig& c) {
    std::string out;
    if (c.layoutManaged)
      out += std::format(
          "; hl.config({{general={{layout={}}}}})", luaStringLiteral(enumToKey(kHyprlandTilingLayouts, c.layout))
      );
    if (c.dwindleManaged)
      out += std::format(
          "; hl.config({{dwindle={{preserve_split={},smart_split={},force_split={},use_active_for_splits={},"
          "default_split_ratio={},split_width_multiplier={},split_bias={}}}}})",
          c.preserveSplit, c.smartSplit, c.forceSplit, c.useActiveForSplits, c.defaultSplitRatio,
          c.splitWidthMultiplier, c.splitBias
      );
    if (c.masterManaged)
      out += std::format(
          "; hl.config({{master={{mfact={},orientation={},new_status={},new_on_active={},new_on_top={}}}}})",
          c.masterFactor, luaStringLiteral(enumToKey(kHyprlandMasterOrientations, c.masterOrientation)),
          luaStringLiteral(enumToKey(kHyprlandMasterStatuses, c.newStatus)),
          luaStringLiteral(enumToKey(kHyprlandMasterPositions, c.newOnActive)), c.newOnTop
      );
    if (c.specialManaged)
      out += std::format(
          "; hl.config({{misc={{close_special_on_empty={}}},binds={{hide_special_on_workspace_change={}}},"
          "input={{special_fallthrough={}}},"
          "cursor={{warp_on_toggle_special={}}}}})",
          c.closeSpecialOnEmpty, c.hideSpecialOnWorkspaceChange, c.specialFallthrough, c.warpOnSpecial
      );
    return out;
  }

  void HyprlandAppearance::sync(
      const HyprlandAppearanceConfig& config, Color primary, Color surface, const std::vector<HyprlandAppRule>& rules,
      const HyprlandInputConfig& input, const HyprlandWindowBehaviourConfig& behaviour,
      const HyprlandTilingConfig& tiling, const std::vector<HyprlandPlacementRule>& placementRules,
      const std::vector<HyprlandWorkspaceConfig>& workspaces, const std::vector<HyprlandKeybindConfig>& keybinds
  ) {
    // Recreate named rules to remove old match/effect fields, including after a shell restart.
    const bool rulesChanged = !m_rulesInitialized
        || m_rules != rules
        || m_placementRules != placementRules
        || workspaceRuleCommands(m_workspaces) != workspaceRuleCommands(workspaces)
        || m_keybinds != keybinds;
    const bool restore = rulesChanged
        || (m_tiling.layoutManaged && !tiling.layoutManaged)
        || (m_tiling.dwindleManaged && !tiling.dwindleManaged)
        || (m_tiling.masterManaged && !tiling.masterManaged)
        || (m_tiling.specialManaged && !tiling.specialManaged)
        || (m_behaviour.focusManaged && !behaviour.focusManaged)
        || (m_behaviour.resizeManaged && !behaviour.resizeManaged)
        || (m_behaviour.snapManaged && !behaviour.snapManaged)
        || (m_behaviour.activationManaged && !behaviour.activationManaged)
        || (m_input.gesturesManaged && !input.gesturesManaged)
        || (m_input.keyboardManaged && !input.keyboardManaged)
        || (m_input.mouseManaged && !input.mouseManaged)
        || (m_input.touchpadManaged && !input.touchpadManaged)
        || (m_input.cursorManaged && !input.cursorManaged)
        || (m_input.scrollManaged && !input.scrollManaged)
        || (m_input.edgeManaged && !input.edgeManaged)
        || (m_config.enabled
            && (!config.enabled
                || (m_config.followTheme && !config.followTheme)
                || (m_config.customAnimations && !config.customAnimations)
                || (m_config.decorationEffectsManaged && !config.decorationEffectsManaged)
                || (m_config.blurFocusManaged && !config.blurFocusManaged)
                || (m_config.glassManaged && !config.glassManaged)
                || (m_config.cursorManaged && !config.cursorManaged)
                || (m_config.overviewManaged && m_config.overviewStyleManaged && !config.overviewStyleManaged)
                || (m_config.overviewManaged && !config.overviewManaged)));
    m_rules = rules;
    m_placementRules = placementRules;
    m_workspaces = workspaces;
    m_keybinds = keybinds;
    m_rulesInitialized = true;
    m_config = config;
    m_input = input;
    m_behaviour = behaviour;
    m_tiling = tiling;
    m_primary = primary;
    m_surface = surface;
    if (restore && m_runtime.available() && m_runtime.configIsLua()) {
      // Restore the user's file values when relinquishing control (including colours).
      m_lastCommand.clear();
      const auto reply = m_runtime.request("reload config-only");
      if (!reply || !reply->starts_with("ok"))
        kLog.warn("could not restore Hyprland appearance from config");
    }
    apply();
  }

  void HyprlandAppearance::syncGlassMasking() {
    // hyprglass with layers:alpha_coverage (> 0) trims layer glass to the surface's alpha, so
    // shell surfaces can send a box per shape and get edges that follow their antialiased curve.
    // Without it, keep the exact strip regions (Hyprland's own blur, or an unpatched hyprglass).
    bool boxes = false;
    if (m_runtime.available() && m_config.glassManaged && m_config.glassEnabled && m_config.glassLayers) {
      const auto reply = m_runtime.requestJson("j/getoption plugin:hyprglass:layers:alpha_coverage");
      boxes = reply && reply->is_object() && reply->value("float", 0.0) > 0.0;
    }
    Surface::setBlurRegionsAsBoxes(boxes);
  }

  void HyprlandAppearance::apply() {
    syncGlassMasking();
    if (!m_runtime.available() || !m_runtime.configIsLua())
      return;
    auto appearance = m_config;
    if (m_input.cursorManaged)
      appearance.cursorManaged = false;
    auto command = appearanceCommand(appearance, m_primary, m_surface)
        + (m_config.enabled ? appRuleCommands(m_rules) : std::string{});
    const auto input = inputMotionCommands(m_input)
        + windowBehaviourCommands(m_behaviour)
        + tilingCommands(m_tiling)
        + placementRuleCommands(m_placementRules)
        + workspaceRuleCommands(m_workspaces)
        + (m_keybinds.empty()
               ? std::string{}
               : keybindCommands(m_keybinds, m_runtime.requestJson("j/binds").value_or(nlohmann::json{})));
    if (!input.empty())
      command = (command.empty() ? "repl do end" : command) + input;
    if (command.empty() || command == m_lastCommand)
      return;
    const auto reply = m_runtime.request(command);
    if (!reply || reply->find("error") != std::string::npos || reply->find("Error") != std::string::npos) {
      kLog.warn("could not apply Hyprland appearance: {}", reply.value_or("IPC unavailable"));
      return;
    }
    m_lastCommand = command;
  }

  void HyprlandAppearance::handleEvent(std::string_view event, std::string_view) {
    if (event != "configreloaded")
      return;
    m_lastCommand.clear();
    // Do not issue synchronous IPC while the compositor is still completing a reload.
    m_applyTimer.start(std::chrono::milliseconds(50), [this]() { apply(); });
  }

  void HyprlandAppearance::notifyCleanup() {
    m_applyTimer.stop();
    m_lastCommand.clear();
  }

  void HyprlandAppearance::notifyChanged() {
    m_lastCommand.clear();
    m_applyTimer.start(std::chrono::milliseconds(50), [this]() { apply(); });
  }
} // namespace compositors::hyprland
