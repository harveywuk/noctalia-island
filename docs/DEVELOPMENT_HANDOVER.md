# Dynamic Noctalia development handover

Updated: 5 October 2026. Working branch: `feature/orbit-island`. The newest state is in
[Checkpoint: 5 October 2026](#checkpoint-5-october-2026), which also sets the design direction
for the desktop beyond the shell. The earlier checkpoints still apply.

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
| Notifications | Critical notifications pulse with the capture glow instead of taking an outline. The banner's app icon is 32 px, the same as the unread card in the expanded view (`adc7f120f`). |
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

All 31 suites pass (`hyprland_smoke.py` with no flag and every `--*-only` flag, plus
`capture_smoke.py` and `island_settings_smoke.py`). Several had drifted from the restyled
Settings and now open pages with `settings-open <section>/<group>` and find controls by text
through `tests/ocr.py`, which tries a 2x upscale, an inverted one, a 3x thresholded one and
sparse mode, ignores punctuation at word edges and tolerates one-letter slips. Use it for new
checks instead of fixed coordinates.

The sweep found real regressions, fixed on 4 October: the screenshot toolbar lost its record
buttons after the Island surface change (`158afc38c`), the Notification Centre stopped marking
notifications seen (`01cee3d8e`), and finished sounds kept a PipeWire connection
open (`217bb0114`).

### Contrast audit, second pass

Notification Centre, session menu, polkit prompt, wallpaper panel, OSD, dock tooltip and lock
screen all clear 4.5:1 in both modes; the only exception is the polkit password placeholder
(4.2:1 in dark mode), left fainter on purpose like other placeholders.

## Checkpoint: 5 October 2026

The morning finished the Island polish list. The rest of the day took the Cupertino look past
the shell to the whole desktop: icons, Qt, fonts, window styling, the cursor and sounds, then a
light-mode pass over all of it. Everything is pushed to `feature/orbit-island` and installed on
the development desktop. 153 of 153 unit tests pass, and the default Hyprland smoke run passes
with the desktop's plugins loaded (`NOCTALIA_TEST_HYPR_PLUGINS=~/.local/share/hyprland-plugins/0.56.2`).

### Shell changes

| Commit | Change |
|--------|--------|
| `156c2f1ff` | Settings search matches words in any order and ranks results (`settingSearchScore`, `rankedSettingMatches`); a hidden match shows its visible parent. |
| `61285f26f` | No card behind a hover row that holds only the tray; with few tray apps the pill looked out of place. |
| `42f56314b` | Hover highlights on hover-row widgets are no longer clipped (cell margin of 8 px × scale). |
| `96193edbd` | Left click on a tray item that has no Activate method (most AppIndicator apps) opens its menu, as macOS menu extras do. |
| `024955f9b` | Panels keep the media artwork gradient while they open and close; the card no longer flashes grey. |
| `57d56232d` | With `shell_mode` pinned apart from `mode`, window decoration (shadow, dim, blur, border colours) follows the apps' mode and glass follows the shell's. Before, light app windows under a dark shell kept the dark profile's heavy shadow and dark borders. The smoke test now checks both mixed cases. |
| `353c5213d`, `44093f94f` | Cursor tooling, below. |

### Desktop theming

These files live in the home directory, not the repository, except the three scripts noted.
Each change kept a `*.before-*` backup next to the file it edited.

| Area | State | Where |
|------|-------|-------|
| Icons | WhiteSur (vinceliuice, tag 2026-09-10), swapped light/dark by the `theme_mode_changed` hook | `~/.local/share/icons/WhiteSur*`, `~/.local/bin/noctalia-icon-theme`, `[hooks]` in `~/.config/noctalia/config.toml` |
| Qt | qt6ct with the Kvantum `Noctalia` theme (built by the `kvantum` template from KvMojave) and the `qt` colour template | `~/.config/qt6ct/qt6ct.conf`; `QT_QPA_PLATFORMTHEME=qt6ct` in `hyprland.lua` and `environment.d/91-qt6ct.conf` |
| Templates | `cava` removed (not installed) and `kvantum` added to the built-in templates; fastfetch got a generated config so its template applies | `[theme.templates]` in `~/.local/state/noctalia/settings.toml` |
| Fonts | SF Pro 11 for interface text, GeistMono Nerd Font Mono 11 for monospace (Kitty's font), set in GNOME settings, GTK 3/4, qt6ct and fontconfig (`sans-serif`, `system-ui`, `monospace`) | `~/.config/fontconfig/fonts.conf` |
| Zen | Already themed: the community template's `userChrome.css` imports load and follow the palette (checked against an unthemed headless render) | profile `ub65qn3d.Default (release)` |
| Windows | Corners, shadow and motion match the shell (below). GTK header bars show no window buttons (`button-layout ':'`), since windows are managed from the keyboard. | `[shell.hyprland_appearance]` in `~/.config/noctalia/config.toml`; GNOME settings; `gtk-decoration-layout` |
| Cursor | ful1e5/apple_cursor v2.0.1 as theme `macOS`: the release Xcursors plus scalable SVG Hyprcursors generated from its sources. Selected in every place that names a cursor theme, including the shell's service override and Umbriel. Bibata and its palette recolourer are removed. | `scripts/build-apple-hyprcursor.py`, [CURSOR_THEME.md](CURSOR_THEME.md); `~/.local/share/noctalia-cursor-theme/` holds the source checkout and release |
| Sounds | Original synthesised theme `cupertino` (glass notes, soft volume pop, shutter clicks, plug chime; unplug silent). Undefined events fall back to freedesktop. The sounds are mono; the shell names stream channels, so mono plays centred at the level stereo does. Re-running the script, or adding or removing a file in the theme, takes effect at the next sound: the shell reloads a theme whose files or directories changed. | `scripts/make-cupertino-sounds.py`; `~/.local/share/sounds/cupertino`; `[audio] sound_theme` in config.toml, GNOME and GTK sound settings |

Window values and where they come from:

| Setting | Value | Source in the shell |
|---------|-------|---------------------|
| `rounding` / `rounding_power` | 24 / 3.26 | The rect shader draws a 16 px continuous corner as a superellipse spanning 1.528 × r with exponent 3.26 (`rect_program.cpp`). |
| `shadow_range` / `shadow_offset_y` | 24 / 6 | Panel shadow: `kBlurRadius` 24 (`shell/surface/shadow.h`), `kShadowOffset` 6 down (`shadowDirectionOffset()` in `config/config_types.h`). |
| Opening curve | spring, stiffness 195, damping 20.9 | `islandExpand`: response 450 ms, damping 0.75 (`ui/motion.h`), converted to mass-spring terms. |
| Closing curve | spring, stiffness 304.6, damping 30 | `islandCollapse`: response 360 ms, damping 0.86. |

The floating-Zen app rule now inherits the global rounding, because app rules clamp rounding to 20.

### Light-mode audit

Switching `mode` to light changed GTK 3/4, Qt and Kvantum, the icons, Kitty, Zen and GNOME's
`color-scheme` together, and switching back restored every value. The one failure was window
decoration under the dark-pinned shell, fixed in `57d56232d`. A state script and screenshots
of a GTK and a Qt app on DP-2 before and after are the method to repeat.

### Theme consistency audit: 5 October, afternoon

The later audit checked rendered controls and already-open applications, exposing gaps that
the earlier generated-value checks missed. It used temporary GTK 3, GTK 4/libadwaita, Qt 6,
Chromium and Electron windows on DP-2 with the installed shell. The sequence was dark, light
without restarting apps, light after reopening the native probes, then dark again. Browser
profiles were isolated. All probes were closed, and settings plus the captured generated
theme files were verified restored to their original values.

| Area | Observed result |
|------|-----------------|
| Shell and window decoration | Glass remained dark; window border accent and blur brightness followed app mode and restored correctly. |
| GTK 4/libadwaita | Rendered light/dark controls and named palette colours updated live. SF Pro 11, WhiteSur icons and the macOS cursor matched the desktop settings. |
| GTK 3 | **Incorrect light surface in dark mode.** The dark named colours existed, but stock light Adwaita still drew the background and text. `adw-gtk3` was absent, so the GTK apply hook skipped the base-theme switch. Named colours also stayed stale in an open probe until it was restarted. Reading colour definitions alone is not a rendering check. |
| Qt 6/Kvantum | Fresh probes used the correct palette in each mode. Open probes retained their old palette after switching. WhiteSur-dark remained selected even after reopening in light mode; the separate configuration bug below prevents icon switching. |
| Chromium and Electron | Both followed the portal's light/dark preference live in both directions. Their default web controls used browser colours, not the exact Noctalia palette. The Electron probe used `nativeTheme.themeSource = system`; apps with their own theme override were not covered. |

The Qt icon failure is reproducible without touching desktop settings: the Kvantum apply
script's `ini_set` recognises `style=...` but misses `style = ...`, then appends a second key.
The local `noctalia-icon-theme` hook uses strict Python `ConfigParser`, which raises
`DuplicateOptionError` after updating GNOME/GTK but before updating Qt. A temporary config
containing just a spaced style key reproduces this with the repository's apply script.
Fix whitespace handling and existing duplicate keys before investigating Qt live reload;
this failed hook may also prevent a configuration-change notification Qt would otherwise see.

Repair order: GTK 3's base theme and rendered dark state; Kvantum INI handling and Qt icon
switching; then repeat the live-update checks. Treat the Qt dark-hint boolean in this audit as
inconclusive: it did not distinguish an unknown hint from an explicit light hint. Qt 5 was not
installed and was not tested. This pass records findings; it does not install fixes.

Local evidence and probe sources are in
`build-rishot/theme-consistency-2026-10-05/`: `dark-start.json`, `light-live.json`,
`light-reopened.json`, `dark-restored-live.json`, and corresponding per-toolkit PNGs.
In particular, compare `dark-start-gtk3.png` with `dark-start-gtk4.png`; their matching named
palette values conceal very different rendered surfaces.

### Theme consistency repairs: 5 October

The GTK 3 base-theme and Qt icon failures are now fixed on the development desktop:

- Installed upstream [adw-gtk3 v6.5](https://github.com/lassekongo83/adw-gtk3/releases/tag/v6.5)
  in `~/.local/share/themes/adw-gtk3{,-dark}` after verifying the release archive against
  GitHub's SHA-256 digest. The existing GTK hook now selects the intended base theme.
  Fresh GTK 3 windows actually render `#242426` / `#f5f5f7` in dark mode and
  `#f2f2f7` / `#1d1d1f` in light mode, matching GTK 4 and Qt's generated palette.
- The Kvantum apply/undo scripts accept whitespace around INI keys, values and section
  headings. Apply removes duplicate managed keys; undo restores the original setting while
  preserving later user choices. Both preserve config symlinks and unrelated settings.
  Installed these two scripts into the live shell's asset directory and repaired the Qt
  config by applying the fixed hook. The existing icon hook now completes without an error.
- Added `kvantum_config` to Meson: the original scripts fail this regression test with
  `DuplicateOptionError`; the fix passes, including apply twice, duplicate repair, undo,
  symlinks, absent Qt configs and later user edits. It and `template_undo_signal` pass.

Repeat visual/probe checks covered GTK 3, GTK 4, Qt 6, Chromium and Electron, with the shell
pinned dark. GTK 4 and browser mode tracking still pass. Qt's icons now update live in both
directions after qt6ct's delayed settings refresh; the final audit waits six seconds after
each switch instead of sampling at the refresh boundary. Qt reports an **unknown** colour
scheme hint (enum value 0), not an explicit light preference.

**Remaining limitation:** open GTK 3 and Qt/Kvantum windows retain their previous colours;
reopening them applies the current palette. GTK 3 loads the user `gtk.css` provider once
([source](https://github.com/GNOME/gtk/blob/gtk-3-24/gtk/gtksettings.c)); switching the base theme
does not reload those overriding colours. Kvantum also retains its running style instance
([upstream explanation](https://github.com/tsujan/Kvantum/discussions/975)). Fixing this requires
a separate reload design. The shell does not close or restart users' applications.

Evidence, screenshots, release metadata and backups are in
`build-rishot/theme-consistency-fixed-2026-10-05/`. Dark mode and original theme settings were
restored after each probe run, and all temporary windows were closed. The repository repairs
are included in the commit checkpoint below. Only the two Kvantum scripts and the user GTK
base themes were installed; the earlier split-mode editor build remains uninstalled.

### Design direction for the desktop

These extend the guiding decisions in [VENTURA_PROJECT.md](VENTURA_PROJECT.md).

- **One system, not a themed shell.** Apps, windows, cursor, fonts and sounds should look like
  they belong to the same Cupertino desktop as the Island. Where the shell has a constant (corner
  radius, shadow, spring), the desktop setting is derived from it, not chosen by eye.
- **The Island stays black.** `shell_mode = "dark"` keeps the shell and its glass dark while
  `mode` drives apps, like the iPhone's Dynamic Island. Anything that decorates app windows
  follows `mode`; anything that is part of the shell follows `shell_mode`.
- **Keyboard first, still.** Windows have no title-bar buttons; Hyprland binds manage them.
- **Original or properly licensed assets only.** SF Pro stays a user install and is never
  bundled. Apple's sounds are not used; the sound theme is synthesised in its spirit. The
  cursor comes from a GPL project that draws Apple-style cursors.
- **Light and dark are both first-class.** Every theming change is checked in both modes.

### Working conventions on the development desktop

- DP-1 (3440x1440) holds a remote-desktop VM that resizes when windows open there. Open test
  windows silently on DP-2's workspace (`hl.dsp.exec_cmd(cmd, { workspace = "2 silent" })`),
  close them afterwards, and put focus back on the VM; closing a window can move focus to DP-2.
  A floating window's `move` rule is relative to its monitor.
- The VM window shows the remote machine's cursors, so check cursor themes elsewhere.
- In the harness, set `GSETTINGS_BACKEND=keyfile` for anything that reads GNOME settings, since
  it has no dconf.

## Proposed next priorities

1. **Greeter check:** build `1.5.0.r3.g5a82d5c` (branch `feature/cupertino`, pushed to the
   fork `harveywuk/noctalia-greeter`) is installed as of 4 October. Verify it at the next login
   with the real synced wallpaper, which the test harness cannot read.
2. **3090 measurement:** read the desktop Hyprland's GPU use with the Island and Control Center open,
   to confirm the surface change on the real GPU.
3. **Desktop theming follow-ups:** GTK 3 dark appearance and Qt icon switching are repaired.
   Design live colour reload for existing GTK 3/Qt windows if needed; current apps need reopening.
   Chromium and Electron system-mode probes pass. GTK event sounds through libcanberra remain
   to check; the white cursor variant is one flag away (`--variant macOS-White`).
4. **Hyprglass upstream:** follow [hyprnux/hyprglass#89](https://github.com/hyprnux/hyprglass/pull/89);
   rebase the patched branch onto new releases until it merges.
5. **Release preparation:** group remaining issues, run the full integration matrix (CI has not
   run on this branch), prepare release notes and decide which fixes to offer upstream (high
   contrast, logind fallback, greeter frame loop and the settings search ranking are generic).

Done since the last list: thick sliders (`0e2b3b6ab`: knobless sliders, an 8 px Island seek bar)
and the text-fit audit.

Local follow-up on 5 October: the **split-mode appearance editor** now shares the runtime's
resolver, shows separate window and glass profiles, and preserves the combined look through
Keep, saved profiles, presets and undo. App-mode changes refresh the editor even with a pinned
shell. All 153 Meson tests, the full release Hyprland smoke run with plugins, and translation
checks passed; screenshots were inspected in both split directions.
[SPLIT_MODE_EDITOR.md](SPLIT_MODE_EDITOR.md) records the implementation and regression evidence.
These local source/build changes have not been installed on the development desktop.

## Hyprland session audit: 5 October

The desktop runs Hyprland 0.56.2 under UWSM, using NVIDIA's 615.71.09 open kernel driver.
DP-1 is the AW3423DWF at 3440×1440 / 164.90 Hz; DP-2 is the Razer RZ39-0350 at
2560×1440 / 165.08 Hz, scale 1, rotated 180° below DP-1. The audit preserved these modes,
positions, colour settings and the fullscreen remote desktop's dimensions. Noctalia owns
the saved DCI-P3 and VRR display overrides; the Lua monitor rules provide startup defaults.

Host changes applied:

- Session environment is now in `~/.config/uwsm/env` and `env-hyprland`: macOS cursor at 32 px,
  qt6ct, and Qt's `wayland;xcb` backend order. Removed the duplicate cursor/Qt environment.d
  files and cursor service override after backing them up. Imported the selected variables
  into the running compositor and activation environment for new processes.
- Kitty, Yazi and Zen shortcuts use `uwsm app --`; Super+Escape uses `uwsm stop` for ordered
  logout. Noctalia shortcuts address the installed Island binary directly.
- `start-shell.sh` queues the shell service without restarting portals or overriding UWSM's
  environment. Hyprpolkitagent already starts through graphical-session.target. Disabled
  Noctalia's competing agent in saved settings, matching the existing base config.

`Hyprland --verify-config`, a live reload and `hyprctl configerrors` passed. Real shortcut and
systemd probes received identical selected environment variables; the shortcut probe ran in
`app-graphical.slice`. Shell, authentication and portal services remained running. Both GPUs'
VA-API drivers initialized successfully without forcing a driver; GLX and Hyprland already
select the RTX 3090. DRM modesetting/fbdev, explicit sync, VFR and blur optimizations are enabled.
The NVIDIA driver reports `UseKernelSuspendNotifiers=1`; legacy suspend units were left alone.

Existing processes keep their original environment until restarted. A fresh login and physical
suspend/resume were not exercised. HDR, monitor OSD settings and colour calibration were not
changed. Backups and probe evidence:
`~/.local/share/hyprland-setup/audit-20261005-153102/`.
Session changes follow [Hyprland's UWSM guidance](https://wiki.hypr.land/useful-utilities/uwsm/).

Authentication follow-up: the user chose **Noctalia's integrated agent**. Set
`shell.polkit_agent = true` in both base config and saved settings, and disabled/stopped
`hyprpolkitagent.service`. The live Noctalia journal confirms registration at
`/org/noctalia/PolkitAuthenticationAgent` and reports the agent active. This supersedes the
agent choice above; `start-shell.sh` no longer starts hyprpolkitagent, so it stays disabled
on the next login. Backups are in
`~/.local/share/hyprland-setup/noctalia-auth-20261005-163925/`.

## Commit checkpoint: 5 October

The split-mode editor, Kvantum INI repairs, sound-preview tools and this handover are committed
as separate changes on `feature/orbit-island`. The split-mode build remains uninstalled.

Pre-commit validation passed all 154 registered Meson tests outside the socket-restricted
sandbox, English translation-key checks, formatting checks for all 12 changed C++ files,
Python compilation and shell syntax checks. `just` was unavailable, so formatting was checked
directly with `clang-format --dry-run --Werror`. The earlier full Hyprland smoke evidence in
[SPLIT_MODE_EDITOR.md](SPLIT_MODE_EDITOR.md) was reviewed; that GUI run was not repeated for
this commit checkpoint.

`scripts/make-island-sound-previews.py` and its HTML template generate original Glass, Warm,
Playful and Modular sound sets, a local listening page and downloadable theme archives. Run
with `--styles glass warm playful modular` to include all four. Validation generated all
24 mono samples, checked sample rates and peak headroom, verified archive integrity and
confirmed that the generated page embeds its manifest. Browser playback was not checked in
this commit pass. Generated files stay outside version control; generating previews does
not install or select a sound theme.

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
