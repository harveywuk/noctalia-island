#include "ui/material.h"

#include "ui/palette.h"

#include <atomic>

namespace ui::material {

  namespace {

    std::atomic<bool> g_backgroundBlurAvailable{false};

    struct Tint {
      float dark;
      float light;
    };

    struct MaterialTints {
      Tint soft;
      Tint glass;
    };

    // Glass values approximate Ventura's materials once the compositor's blur is
    // underneath; Soft keeps more tint for compositors with weak or no blur.
    constexpr MaterialTints tintsFor(Kind kind) noexcept {
      switch (kind) {
      case Kind::Panel:
        return {.soft = {0.84F, 0.90F}, .glass = {0.72F, 0.80F}};
      case Kind::Toast:
        return {.soft = {0.88F, 0.92F}, .glass = {0.74F, 0.82F}};
      case Kind::Osd:
        return {.soft = {0.86F, 0.90F}, .glass = {0.66F, 0.78F}};
      case Kind::Window:
        return {.soft = {0.88F, 0.92F}, .glass = {0.78F, 0.84F}};
      }
      return {.soft = {1.0F, 1.0F}, .glass = {1.0F, 1.0F}};
    }

  } // namespace

  void setBackgroundBlurAvailable(bool available) noexcept { g_backgroundBlurAvailable.store(available); }

  bool backgroundBlurAvailable() noexcept { return g_backgroundBlurAvailable.load(); }

  PanelTransparencyMode resolveMode(PanelTransparencyMode mode) noexcept {
    if (mode != PanelTransparencyMode::Auto) {
      return mode;
    }
    return backgroundBlurAvailable() ? PanelTransparencyMode::Glass : PanelTransparencyMode::Solid;
  }

  float tintOpacity(Kind kind, PanelTransparencyMode mode) noexcept {
    const MaterialTints tints = tintsFor(kind);
    const bool light = isResolvedLightTheme();
    switch (resolveMode(mode)) {
    case PanelTransparencyMode::Soft:
      return light ? tints.soft.light : tints.soft.dark;
    case PanelTransparencyMode::Glass:
      return light ? tints.glass.light : tints.glass.dark;
    case PanelTransparencyMode::Solid:
    case PanelTransparencyMode::Auto:
      break;
    }
    return 1.0F;
  }

} // namespace ui::material
