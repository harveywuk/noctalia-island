#pragma once

#include "config/config_types.h"

#include <cstdint>

// Translucent shell materials, modelled on macOS Ventura's popover, notification,
// HUD and window-sidebar materials. The compositor blurs whatever sits behind a
// surface (ext-background-effect); the shell only chooses how much tint covers it.
namespace ui::material {

  enum class Kind : std::uint8_t {
    Panel,  // floating panels and popovers
    Toast,  // notification banners
    Osd,    // volume / brightness HUDs
    Window, // settings window background
  };

  // Recorded by WaylandConnection from the ext-background-effect capabilities event.
  void setBackgroundBlurAvailable(bool available) noexcept;
  [[nodiscard]] bool backgroundBlurAvailable() noexcept;

  // Auto becomes Glass when the compositor can blur behind shell surfaces, else Solid.
  [[nodiscard]] PanelTransparencyMode resolveMode(PanelTransparencyMode mode) noexcept;

  // Surface tint alpha for a material in the given mode. Light materials sit a little
  // more opaque than dark ones, as on macOS, so dark text keeps its contrast.
  [[nodiscard]] float tintOpacity(Kind kind, PanelTransparencyMode mode) noexcept;

  // Settings window background: the Window material when translucency is on, opaque otherwise.
  [[nodiscard]] inline float settingsWindowOpacity(const ShellConfig& shell) noexcept {
    return shell.settingsWindowTranslucent ? tintOpacity(Kind::Window, shell.panel.transparencyMode) : 1.0F;
  }

} // namespace ui::material
