#include "compositors/hyprland/hyprland_keybinds.h"

#include "compositors/hyprland/hyprland_runtime.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "shell/bar/widgets/workspace_preferences.h"

#include <algorithm>
#include <array>
#include <format>
#include <xkbcommon/xkbcommon.h>

namespace compositors::hyprland {
  namespace {
    constexpr std::string_view prefix = "Noctalia shortcut:";
    unsigned nativeMask(const KeyChord& chord) {
      return ((chord.modifiers & KeyMod::Shift) ? 1 : 0)
          | ((chord.modifiers & KeyMod::Ctrl) ? 4 : 0)
          | ((chord.modifiers & KeyMod::Alt) ? 8 : 0)
          | ((chord.modifiers & KeyMod::Super) ? 64 : 0);
    }
    std::string nativeChord(unsigned mask, std::string_view key) {
      std::string out;
      for (const auto& [bit, name] :
           std::array<std::pair<unsigned, std::string_view>, 4>{{{64, "SUPER"}, {4, "CTRL"}, {8, "ALT"}, {1, "SHIFT"}}})
        if (mask & bit)
          out += std::string(name) + "+";
      return out + std::string(key);
    }
    bool ours(const nlohmann::json& bind) { return bind.value("description", "").starts_with(prefix); }
    bool same(const nlohmann::json& bind, const KeyChord& chord) {
      if (!bind.is_object() || ours(bind) || bind.value("modmask", 0U) != nativeMask(chord))
        return false;
      // Hyprland's binding IPC does not expose the keyboard mapping for physical or multi-key bindings.
      // Conservatively reject replacement of potentially overlapping physical keys.
      if (bind.value("keycode", 0) != 0 || bind.value("key", "").empty() || bind.value("catch_all", false))
        return true;
      const auto sym = xkb_keysym_from_name(bind.value("key", "").c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
      return sym != XKB_KEY_NoSymbol && xkb_keysym_to_lower(sym) == chord.sym;
    }
  } // namespace
  std::optional<KeyChord> compositorChord(std::string_view text) {
    const auto parsed = parseKeyChordSpec(text, true);
    if (!parsed || KeySymbol::isModifier(parsed->sym))
      return {};
    auto chord = *parsed;
    chord.sym = chord.sym == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : xkb_keysym_to_lower(chord.sym);
    if (isPrintableKey(chord.sym) && !(chord.modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)))
      return {};
    return chord;
  }
  std::string compositorChordString(const KeyChord& chord) {
    std::array<char, 128> key{};
    if (xkb_keysym_get_name(
            chord.sym == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : xkb_keysym_to_lower(chord.sym), key.data(), key.size()
        )
        <= 0)
      return {};
    return nativeChord(nativeMask(chord), key.data());
  }
  std::string keybindAction(const HyprlandKeybindConfig& rule) {
    const auto quoted = luaStringLiteral(rule.target);
    if (rule.action == "workspace" || rule.action == "move_workspace") {
      if (!workspace_preferences::validTarget(rule.target)
          && rule.target != "e+1"
          && rule.target != "e-1"
          && rule.target != "previous")
        return {};
      return (rule.action == "workspace" ? "hl.dsp.focus({workspace=" : "hl.dsp.window.move({workspace=")
          + quoted
          + "})";
    }
    if (rule.action == "focus_direction" || rule.action == "move_direction") {
      if (rule.target != "left" && rule.target != "right" && rule.target != "up" && rule.target != "down")
        return {};
      return (rule.action == "focus_direction" ? "hl.dsp.focus({direction=" : "hl.dsp.window.move({direction=")
          + quoted
          + "})";
    }
    if (rule.action == "fullscreen")
      return "hl.dsp.window.fullscreen({mode=\"fullscreen\",action=\"toggle\"})";
    if (rule.action == "floating")
      return "hl.dsp.window.float({action=\"toggle\"})";
    if (rule.action == "close")
      return "hl.dsp.window.close()";
    if (rule.action == "overview")
      return "function() if hl.plugin.overview then hl.plugin.overview.toggle() end end";
    if (rule.action == "exec"
        && !rule.target.empty()
        && rule.target.size() <= 4096
        && rule.target.find('\0') == std::string::npos)
      return "hl.dsp.exec_cmd(" + quoted + ")";
    return {};
  }
  std::string keybindProblem(
      const HyprlandKeybindConfig& rule, const std::vector<HyprlandKeybindConfig>& rules, const nlohmann::json& live
  ) {
    const auto chord = compositorChord(rule.chord);
    if (!chord)
      return "invalid-chord";
    if (keybindAction(rule).empty())
      return "invalid-action";
    for (const auto& other : rules)
      if (other.name != rule.name && other.enabled && compositorChord(other.chord) == chord)
        return "duplicate";
    if (!live.is_array())
      return "unavailable";
    bool conflict = false;
    for (const auto& bind : live) {
      if (!same(bind, *chord))
        continue;
      if (bind.value("keycode", 0) != 0 || bind.value("key", "").empty() || bind.value("catch_all", false))
        return "physical-conflict";
      if (!bind.value("submap", "").empty() || bind.value("submap_universal", "false") == "true")
        return "submap-conflict";
      conflict = true;
    }
    return conflict && !rule.replaceExisting ? "conflict" : "";
  }
  std::string keybindCommands(const std::vector<HyprlandKeybindConfig>& rules, const nlohmann::json& live) {
    if (!live.is_array())
      return {};
    nlohmann::json signature = nlohmann::json::array();
    std::string body;
    for (const auto& rule : rules) {
      if (!rule.enabled)
        continue;
      signature.push_back({rule.name, rule.chord, rule.action, rule.target, rule.replaceExisting, rule.repeating});
      if (!keybindProblem(rule, rules, live).empty())
        continue;
      const auto chord = compositorChord(rule.chord).value();
      if (rule.replaceExisting) {
        for (const auto& bind : live) {
          if (!same(bind, chord))
            continue;
          // The temporary handle removes matching native key identities (including case variants).
          // Replacement is refused above if this would touch a submap or physical key binding.
          body += "hl.bind("
              + luaStringLiteral(nativeChord(nativeMask(chord), bind.value("key", "")))
              + ",function() end):remove()\n";
        }
      }
      body += std::format(
          "hl.bind({},{},{{description={},repeating={}}})\n", luaStringLiteral(compositorChordString(chord)),
          keybindAction(rule), luaStringLiteral(std::string(prefix) + rule.name), rule.repeating
      );
    }
    if (signature.empty())
      return {};
    const auto key = luaStringLiteral(signature.dump());
    return "; do local key="
        + key
        + "; if _G.__noctalia_shortcuts ~= key then hl.define_submap(\"\",function()\n"
        + body
        + "end); _G.__noctalia_shortcuts=key end end";
  }
} // namespace compositors::hyprland
