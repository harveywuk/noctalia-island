#pragma once
#include "config/config_types.h"
#include "core/input/key_chord.h"

#include <nlohmann/json.hpp>

namespace compositors::hyprland {
  std::optional<KeyChord> compositorChord(std::string_view);
  std::string compositorChordString(const KeyChord&);
  std::string keybindAction(const HyprlandKeybindConfig&);
  std::string
  keybindProblem(const HyprlandKeybindConfig&, const std::vector<HyprlandKeybindConfig>&, const nlohmann::json&);
  std::string keybindCommands(const std::vector<HyprlandKeybindConfig>&, const nlohmann::json&);
} // namespace compositors::hyprland
