# Dynamic Noctalia development handover

Updated: 4 October 2026. Working branch: `feature/orbit-island`. The newest state is in
[Checkpoint: 4 October 2026](#checkpoint-4-october-2026); the sections before it describe the
1 October checkpoint and still apply.

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

Follow [BUILDING.md](../BUILDING.md) for dependencies and a fresh checkout. Build
directories are not committed. `build-release` is the release build that gets installed;
it does not compile the unit tests. `build-test` is a debug build with tests enabled.
The Hyprland harness runs `build-rishot/noctalia`, so point that at the binary under test
(for example `ln -sf ../build-release/noctalia build-rishot/noctalia`). Useful commands:

```sh
meson compile -C build-test -j 6
meson test -C build-test --no-rebuild --print-errorlogs
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

## Follow-up session (1 October 2026, afternoon)

Priorities 1–3 from the previous checkpoint were worked through, plus Cupertino styling for
the lock screen, Island and greeter. All 147 unit tests pass; each item below was also checked
in the isolated Hyprland harness and is installed on the development desktop.

| Area | Change |
|------|--------|
| Accessibility | Radio buttons are keyboard focusable; the macOS light outline meets 3:1; High contrast no longer lowers contrast (role-aware transform, `palette_high_contrast` test). |
| Text fit | `NOCTALIA_DEBUG_TEXT_FIT=1` logs ellipsized labels; `noctalia config settings-pages` and `settings-open <section>/<group>`; `--text-fit-only` audits every page in English and German at 1.5x. Text/path rows stack, template grid reflows (`GridView::setAutoColumnMinWidth`). |
| Dock previews | `--dock-preview-perf-only` measures cost with real apps. Skipping undamaged frames and requesting display-sized thumbnails cut shell CPU from 14.1% to 3.7% (three videos). |
| Lock screen | Cupertino login-box layout (default for new setups): date and large time, avatar, glass password pill, shake on failure. `Input::setContentColor`. `--lockscreen-only`. |
| Island | `island.appearance` = `cupertino` (default) or `theme`: always black, white content, Apple activity tints, bare controls, 42px corners; expanded activity cards show one activity. Island launcher sizes like Spotlight (`Panel::islandWidth/islandHeight`). |
| Session | logind lookup falls back to the user's display session, so lock-before-suspend works when the shell runs as a user service. |
| Setup | [SETUP.md](SETUP.md), `scripts/install-local.sh`, `examples/starter.toml`, systemd unit and Hyprland start hook; `--starter-only` boots the starter config. |
| Greeter | Separate fork `~/Projects/noctalia-greeter`, branch `feature/cupertino` (from v1.5.0): Cupertino layout, frame-loop fix for animations, `tests/visual_smoke.py`, Arch PKGBUILD. Built but not installed. |

## Checkpoint: 4 October 2026

### 2–3 October: the Ventura pass

PRs #1 to #24 brought the shell closer to macOS Ventura: controls at macOS proportions (with
matching GTK and Kvantum themes), notification banners and Notification Centre, the session
menu, polkit prompt, dock badges, the Raycast-style launcher, Island script activities, quick
pills, up-next events and the screenshot thumbnail. The fork's strings were translated into
all 26 languages (`1a2b4ba2c`). [VENTURA_PROJECT.md](VENTURA_PROJECT.md) summarises that work
and its guiding decisions; [ORBIT_ISLAND.md](ORBIT_ISLAND.md) documents the Island features.

### 4 October: glass Island and Island polish

All commits are on `feature/orbit-island` and pushed, except where noted. Everything below is
installed on the development desktop.

| Area | Change |
|------|--------|
| Glass Island | `glass` on an Island bar (**Settings → Dynamic Island → Glass**) makes the capsule and the panels it hosts translucent over the compositor's blur. The blur region traces the capsule and split bubbles in up to 64 rectangles instead of bounding boxes (`Surface::tessellateRoundedRect`, `setBlurRegionRectLimit`), so corners stay round. |
| Activities | Edge progress rings around the capsule; a second split bubble for a third concurrent activity, ordered by `activity_priority`; a white visualiser and solid transport controls. |
| Notifications | Critical notifications pulse with the capture glow instead of taking an outline. The banner's app icon is 32 px, the same as the unread card in the expanded view (`adc7f120f`, local only). |
| Crossfades | The incoming view waits for the outgoing one to fade, and the outgoing view stays centred as the capsule morphs (`9fb647369`, `6f7b15112`). |
| Control Center | Big Sur level sliders for volume and brightness, distinct tile icons, no percentages, Wired title. |
| Lock screen | Artwork gradient background while media plays; each track's artwork is fetched once. |
| Tests | Glass-region coarsening test; island and config round-trip tests updated for the glass key and edge-ring default. 152 of 152 unit tests pass in `build-test`. |

### Hyprglass patch

Smooth glass edges need two hyprglass changes that are not upstream yet:
`layers:alpha_coverage` (layer glass follows surface alpha at the edges) and accepting up to
64 protocol region rectangles (was 16). They are on branch `noctalia/alpha-coverage` of the
fork `harveywuk/hyprglass`, offered upstream as
[hyprnux/hyprglass#89](https://github.com/hyprnux/hyprglass/pull/89), which is still open.

- Without the patch, glass edges look stepped and glow shows in the capsule's corners.
- Nothing updates plugins automatically on the development desktop (no hyprpm). The local
  hyprglass source checkout is on the patched branch, so a rebuild keeps the fix. Rebase it
  onto new upstream releases until #89 is merged.
- Restart Hyprland after replacing `hyprglass.so`. Reloading it in place left a window
  decoration pointing at unloaded code and crashed Hyprland on 4 October.
- To test glass in the harness, load the plugin in the private Hyprland
  (`hyprctl plugin load <path>/hyprglass.so`), then `config-reload` the shell.

### Fixed: two clocks while the Island shrinks

Opening the Control Center from the hover view and closing it showed a second, larger clock
clipped at the capsule's right edge (`381fdb89a`). The first rebuild after a hosted panel
returns the surface crossfaded out the wide hover view from before the panel opened. The
panel has already faded and shrunk by then, so that rebuild now drops the old view. A
desktop screen recording found it; the harness only catches it when the panel is opened
by clicking the expanded Island, not with `panel-toggle`.

### Glass performance

The Island's layer surface used to cover the whole output, and hyprglass's layer glass clears a
buffer and runs its composite over the full layer whenever the Island is redrawn. With a glass
Island over moving content at 3440x1440@165 on the integrated GPU, Hyprland held about 111 fps
(165 with glass off). The surface now spans the output's width but only the capsule's height
(128 px at rest), and a hosted panel trims it to the panel's height (`41b69d234`). The same test
holds 165 fps at rest and on hover and about 162 fps with the Control Center open. Window glass
costs much less (about 8% of the integrated GPU for a full-screen window). The nested harness
cannot render on the NVIDIA GPU (GBM allocation fails for headless outputs), so the numbers are
from the integrated GPU, a worst case.

Because the surface now resizes, the shell always sends Hyprland a `no_anim` layer rule for the
`noctalia-island` namespace (`e2e2d88a4`); without it Hyprland stretches the Island over each
resize. On compositors that animate layer resizes some other way, expect the same stretch.

### Smoke suites

All nine `--island-*-only` suites pass. Routing, bars and activity-cycle had drifted from the
features (`c72a96231`): the first two measured the capsule from pixels that the artwork gradient
now lights, and bars clicked Settings coordinates from before the restyle; it now navigates with
`settings-open bar/widgets` and finds buttons by their text.

### Readability and curated palettes

A contrast audit (every OCR'd word on Settings, the Control Center, the Island, notifications and
the launcher, both modes, glass on) drove these changes:

- Light glass is much more opaque than dark (`src/ui/material.cpp`), and Settings' grouped cards
  are nearly opaque white in light mode (`settingsGroupedCardFill`). Light Settings went from
  52-59 words under 4.5:1 per page to 0-2. Settings rebuilds its content on a light/dark switch.
- Badges and notices keep their tint but set text in the body colour; launcher secondary text is
  the body colour at reduced opacity.
- Catppuccin, Gruvbox, Rose Pine and Tokyo Night are bundled next to macOS, mapped from their
  official colours with small lightness nudges where a pair fell short. `community_palettes_test`
  checks every bundled palette; `theme.curated_palettes` (default on) hides the online catalogue.
- The dark accent stays `#0072E3`: the palette test requires 4.5:1 for white text on the accent,
  which Apple's brighter `#0A84FF` doesn't meet.

## Proposed next priorities

1. **Greeter check:** build `1.5.0.r3.g5a82d5c` (branch `feature/cupertino`, pushed to the
   fork `harveywuk/noctalia-greeter`) is installed as of 4 October. Verify it at the next login
   with the real synced wallpaper, which the test harness cannot read.
2. **3090 measurement:** read the desktop Hyprland's GPU use with the Island and Control Center open,
   to confirm the surface change on the real GPU.
3. **Design decisions:** thick sliders for the media seek bar and Settings sliders.
4. **Remaining text fit and reduced-motion marquees:** carried over from 1 October
   (`--text-fit-only`).
5. **Release preparation:** group remaining issues, run the full integration matrix, prepare
   release notes and decide which fixes to offer upstream (high contrast, logind fallback,
   greeter frame loop are generic).

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
