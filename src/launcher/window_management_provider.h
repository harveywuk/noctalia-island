#pragma once

#include "launcher/launcher_provider.h"

#include <optional>
#include <span>
#include <string_view>

class CompositorPlatform;

namespace window_management {

  // Logical (layout) coordinates, as Hyprland reports window positions and sizes.
  struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const Rect&) const = default;
  };

  // Raycast's Window Management layouts. The remaining commands (fullscreen, floating, pin, move
  // between displays) are plain compositor dispatchers without a frame.
  enum class Layout : std::uint8_t {
    LeftHalf,
    RightHalf,
    TopHalf,
    BottomHalf,
    TopLeftQuarter,
    TopRightQuarter,
    BottomLeftQuarter,
    BottomRightQuarter,
    FirstThird,
    CenterThird,
    LastThird,
    FirstTwoThirds,
    LastTwoThirds,
    Maximize,
    AlmostMaximize,
    Center,
    MakeLarger,
    MakeSmaller,
  };

  // Where `window` goes for `layout` inside `workArea` (the monitor minus reserved bar space).
  // Center keeps the window's size; Make Larger/Smaller grow or shrink it around its centre by a
  // tenth of the work area, never beyond the work area or below a usable minimum.
  [[nodiscard]] Rect frameFor(Layout layout, const Rect& workArea, const Rect& window);

} // namespace window_management

// Window Management commands in the launcher (Raycast's Window Management extension): Left Half,
// Right Half, quarters and thirds, Maximize, Center, Toggle Fullscreen, Next Display, … They act
// on the focused window through Hyprland's IPC, so the provider is empty on other compositors.
// A tiled window is floated first, as every window is "floating" on macOS.
class WindowManagementProvider : public LauncherProvider {
public:
  struct Command {
    std::string_view id;
    std::string_view glyph;
    std::optional<window_management::Layout> layout;
    // Classic Hyprland dispatcher for commands without a frame; {window} is the address selector.
    std::string_view dispatcher;
  };

  explicit WindowManagementProvider(CompositorPlatform* platform) : m_platform(platform) {}

  [[nodiscard]] std::string_view defaultPrefix() const override { return "wm"; }
  [[nodiscard]] bool defaultIncludeInGlobalSearch() const override { return true; }
  [[nodiscard]] std::string_view id() const override { return "WindowManagement"; }
  [[nodiscard]] std::string displayName() const override;
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "layout-grid"; }
  [[nodiscard]] bool trackUsage() const override { return true; }
  [[nodiscard]] bool supportsAliases() const override { return true; }

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  [[nodiscard]] std::vector<LauncherResult> queryPrefixed(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;
  [[nodiscard]] std::string primaryActionLabel(const LauncherResult& result) const override;

  [[nodiscard]] static std::span<const Command> commands();

private:
  [[nodiscard]] bool available() const;
  [[nodiscard]] std::vector<LauncherResult> search(std::string_view text, bool listAll) const;
  bool run(const Command& command);

  CompositorPlatform* m_platform = nullptr;
};
