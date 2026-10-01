# Dynamic Noctalia development handover

Updated: 1 October 2026. Working branch: `feature/orbit-island`.

This checkpoint brings together the recent Hyprland settings work, Dynamic Island
integration, Cupertino-inspired shell styling, and dock improvements. The latest
addition is live window previews in the dock. It has been built, tested in an
isolated Hyprland session, and installed in the development desktop session.

The project is an independent fork of Noctalia's native C++/Wayland shell, currently
based on Noctalia 5.2.0. The interface is implemented in the native renderer, not
QML. The upstream licence and credits remain in place.

## Direction

Build a cohesive, configurable Linux desktop with the clarity and interaction
style of Cupertino: a navigable settings sidebar, consistent controls and surfaces,
restrained motion, a useful floating dock, and an Island that integrates with the
rest of the shell.

Keep Noctalia's existing configuration model, colour roles, community palettes,
plugins, bar management and monitor overrides. The macOS community palette is an
optional light/dark choice. Shared visual improvements also work with other palettes.

Prefer polishing the everyday experience and measuring its cost before adding
more controls. Effects should respect the shell's animation preferences; previews
and temporary surfaces should release their resources when they close.

## Implemented in this checkpoint

| Area | Current behaviour |
| --- | --- |
| Settings navigation | Cupertino-inspired sidebar, category overviews, focused editors, Back/Alt+Left navigation, global search and scoped reset actions. |
| Shared appearance | Consistent controls, cards, menus, tooltips, panels, notifications, OSDs and lock-screen surfaces. A bundled macOS community palette supports light/dark mode and offline selection. |
| Shell motion | Shared timings and easing for reveals, dismissals and feedback. Dock and Island transitions follow animation speed and reduced-motion preferences. |
| Island as a bar | Dynamic Island is a presentation in normal Bars settings, with add/edit/delete, monitor overrides, inheritance, visibility and smart hide. |
| Island activities | Configurable media/download/timer priority, optional cycling, hover tabs and interaction-aware timing. Track and Bluetooth previews can target all, focused or named monitors. |
| Bluetooth battery | A connection preview defaults to five seconds, then battery information remains in hover details. Low battery stays visible in the compact Island. |
| Island styling | Cohesive light/dark cards, calendar, widgets, media controls, seeking, batteries, privacy indicators, notifications, OSDs and panel transitions. |
| Dock | Floating appearance, magnification, launch bounce, press feedback, running indicators, window cycling, pinned-app reordering and smart-hide grace timing. |
| Live dock previews | Hover opens a paginated window picker. Visible cards update live, activate their own window, and offer a close button. Matching handles windows with identical titles. |
| Hyprland appearance | Gaps, borders, rounding, opacity, blur/focus, shadows/glow, theme colours, named profiles, undo/reset and theme-linked profile switching. |
| Animation editing | Interactive Bézier handles, numeric coordinates, spring controls, illustrative playback, and separate opening/closing/moving/workspace curves and durations. |
| Plugin controls | Opt-in settings for Hyprglass, Dynamic Cursors, Hyprspace, kinetic scrolling and edge hover when the corresponding plugins are loaded. |
| Input | Native mouse, touchpad and keyboard settings alongside plugin input controls and workspace gestures. |
| Displays | Resolution, refresh rate, fractional scale, VRR policy, colour management, bit depth and transform; draggable layout and display identification; staged changes with a 15-second Keep/Revert flow. |
| Windows and workspaces | Window behaviour, tiling, app appearance/placement rules, workspace preferences and a Hyprland shortcut editor. |
| Desktop administration | Startup apps, default applications, preferred terminal and configuration backups with selective restore, preview and undo. Display-affecting restores use the display confirmation flow. |
| Resource handling | Idle polling improvements, lazy UI resources, a repeatable lifecycle benchmark, and explicit cleanup of live-preview captures. |

Detailed behaviour and configuration keys are documented in the
[Hyprland guide](user/compositor-settings/hyprland.mdx),
[shell settings guide](user/configuration/shell.mdx),
[dock guide](user/dock/index.mdx), and
[theme guide](user/theming/index.mdx).

## Latest work: live dock previews

Enable **Settings → Dock → Behavior → Window Previews**. The option remains off by
default for a fresh configuration; it is enabled in the development desktop.
The default hover delay is 450 ms and can be adjusted in the same section.

- Each visible window gets a reusable capture session and shared-memory buffer.
- Updates are capped at 10 frames per second per window, with at most six windows
  on the current page. This is a thumbnail view, not full-rate video playback.
- Unchanged frames skip texture uploads and popup redraws. The capture protocol
  also permits waiting for source content to change.
- New frames update image nodes in place, keeping buttons and pointer capture
  stable during interaction.
- Window resizes replace buffers as needed and continue capturing.
- Changing pages, closing the picker, disabling previews or tearing down the dock
  cancels the relevant sessions and releases their buffers.
- If capture is unavailable, the card shows an app icon and title. A stopped
  capture retains its last frame. Activation and closing remain available.
- Reduced motion disables shell transition effects; live application content
  still updates.

The existing one-shot capture API remains available to the window switcher.
Capture requires compositor support for the foreign-toplevel image source and
image-copy protocols. Application rendering and compositor behaviour can limit
how often an individual window produces new content.

## Where to continue in the code

| Concern | Main locations |
| --- | --- |
| Settings structure and controls | `src/shell/settings/settings_window*`, `settings_content*`, `settings_sidebar.cpp`, `settings_registry.cpp` |
| Reusable appearance and motion | `src/ui/style.h`, `src/ui/palette.h`, `src/ui/motion.h`, `src/ui/controls/`, `assets/community-palettes/` |
| Island lifecycle and activity selection | `src/shell/island/`, `src/shell/bar/`, `src/shell/panel/panel_manager_island.cpp` |
| Dock previews and interactions | `src/shell/dock/dock_preview.*`, `dock.cpp`, `dock_items.cpp` |
| Window capture | `src/capture/toplevel_thumbnail_capture.*` |
| Hyprland integration | `src/compositors/hyprland/hyprland_appearance.*`, `hyprland_displays.*`, `hyprland_keybinds.*`, `hyprland_gestures.cpp`, `hyprland_workspace_rules.cpp` |
| Persistence and validation | `src/config/config_types.h`, `src/config/schema/`, `src/config/config_overrides.cpp`, `src/config/config_backup.*` |
| Desktop services | `src/system/startup_apps.*`, `default_apps.*`, `terminal_launch.*` |

The HTML file in [design/settings-preview.html](design/settings-preview.html) is
the settings design reference. The implemented interface lives in the C++ source.

## Validation and useful commands

Before publishing this checkpoint on 1 October 2026, the release rebuild and all
145 registered Meson tests passed. English translation-key validation, formatting
checks for changed C++ files, Python syntax checks and the new documentation links
also passed.

The latest live-preview integration run passed changing-content checks, buffer
reuse/release, resize recovery, clicks during updates, idle resume, hover grace,
smart hide, activation/close, identical titles, pagination, opt-out, light/dark
themes, reduced motion, a fractional-scale rotated output and reload cleanup.
The thumbnail, configuration round-trip and dock unit checks also passed.

Earlier focused integration runs passed settings navigation, shared Cupertino
surfaces, Island styling, dock motion and display layout/rollback. These are
separate runs from the final live-preview check, not a claim that every integration
scenario was rerun after every edit. Physical HDR/VRR hardware and other compositor
versions need their own validation.

Follow [BUILDING.md](../BUILDING.md) for dependencies and a fresh checkout. The local
development build directory is `build-rishot`; it is not committed. After configuring
a build with tests enabled, useful commands are:

```sh
meson compile -C build-rishot -j 6
meson test -C build-rishot --no-rebuild --print-errorlogs
python3 tools/i18n-check.py
python3 tests/hyprland_smoke.py --dock-preview-only
python3 tests/hyprland_smoke.py --dock-motion-only
python3 tests/hyprland_smoke.py --settings-layout-only
python3 tests/hyprland_smoke.py --cupertino-only
python3 tests/hyprland_smoke.py --island-cupertino-only
python3 tests/hyprland_smoke.py --displays-only
python3 tests/hyprland_smoke.py --performance-only
```

The Hyprland harness creates private HOME/XDG directories, D-Bus, PipeWire, a
headless parent compositor and nested GPU-backed Hyprland outputs. It needs the
fixture dependencies, including labwc, Hyprland, a GPU render node, Kitty, grim,
Tesseract with English data and Python Pillow. Its screenshots, logs and measurements
stay under the ignored build directory. See the harness for its local data paths.

When extending input tests, a Hyprland cursor warp alone may omit Wayland motion
events. The dock fixture sends a small relative pointer movement after a warp.
Wait for asynchronous rendering when checking live content. Capture buffers can
be checked through the fixture process's file descriptors to verify cleanup.

## Proposed next priorities

These are follow-on recommendations, not additional features already implemented
or a commitment to a particular next task.

1. **Polish and accessibility:** audit keyboard navigation, focus visibility,
   contrast, long labels, UI scaling and reduced motion across the finished shell.
2. **Live-preview performance:** measure CPU, GPU and retained memory with several
   real applications and video playing; cover minimized/off-workspace windows,
   display disconnects and capture failure. Consider a live/static option or refresh
   control only if those measurements or user preference justify it.
3. **A reproducible desktop setup:** document compatible plugin versions and optional
   dependencies, installation and upgrade steps, and a small example configuration
   that a new user can try without copying a personal desktop configuration.
4. **Release preparation:** group remaining issues by subsystem, run the broader
   integration matrix, expand platform testing and prepare release notes. Keep
   upstream updates manageable through focused follow-up changes.

## Boundaries to preserve

- Keep bar/monitor inheritance, scoped reset and undo behaviour intact.
- Hyprland management stays opt-in. Plugin settings do not install or load plugins;
  local package installations are separate from this source checkpoint.
- Preserve users' hand-written Hyprland configuration and unrelated settings when
  applying, reverting or restoring managed values.
- Keep existing palettes usable. Selecting the macOS palette must remain a choice.
- Do not add idle polling or retain capture buffers after temporary UI closes.
- Report memory measurements with the workload, build and environment. The lifecycle
  benchmark is available; this handover does not establish a universal memory figure.
- Personal configuration, credentials, runtime state, backups and build/test artefacts
  are not part of the source handover.
