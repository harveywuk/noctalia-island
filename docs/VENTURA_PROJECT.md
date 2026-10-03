# Noctalia Island: project summary

_State as of 3 October 2026, 19:00 UTC._

## Scope and direction

**Goal:** make Noctalia Island look and feel like macOS Ventura.

**What the project is:** a fork of Noctalia 5.2.0. It is a native C++/Wayland desktop shell with its own GLES renderer, not QML, and a Dynamic Island added on top. It runs on Hyprland, and harvey uses it keyboard-first.

**Repository:** [harveywuk/noctalia-island](https://github.com/harveywuk/noctalia-island). All work is based on and merged into `feature/orbit-island`. `main` is the older upstream base and stays untouched.

### Guiding decisions

- **The Dynamic Island is the centrepiece.** New features build on it before anything else.
- **Controls use macOS proportions** (toggles, sliders, buttons, menus), and **GTK and Qt apps should match them**. Where GTK can't follow the shell, the shell follows GTK. For example, switches use GTK's 42×22 shape.
- **Keyboard-first.** Features that need a mouse are out, such as a drag-and-drop file shelf or traffic-light window controls. Window switching stays with Hyprland.
- **Restyle, don't replace.** When a redesign drops a look harvey liked, the original is kept and restyled toward Apple. That is why the fanned hover calendar and the session menu's row of cards survived.
- **Leave out macOS details that add nothing on a single-user setup.** For example, the password prompt has no user name field.
- **Fonts:** SF Pro if installed, then Inter, then sans-serif. SF Pro is never bundled.
- **Accent:** dark mode's accent is `#0072E3`, not Apple's `#0A84FF`, because the palette test requires 4.5:1 contrast for white text.
- **Blur** comes from the compositor. On Hyprland, saturation is `decoration:blur:vibrancy`.
- **On hold:** a floating Ventura-style Control Center. It won't start unless harvey asks.

## Work done

Every PR below is merged into `feature/orbit-island`. The merged branch builds, and 151 of 152 tests pass. The one failure (`process`) happens only in the cloud sandbox. `main` is untouched.

### Look and feel (PRs #1 to #6)

| PR | What it does |
|---|---|
| #1 | Ventura-style translucent glass for panels, toasts, OSDs and Settings |
| #2 | Spring motion for the Island expanding and collapsing |
| #3 | Fonts, corner radius (16) and shadows moved toward Ventura; fanned hover calendar restored |
| #4 | Shell controls resized to macOS proportions |
| #5 | GTK 3/4 and Qt (Kvantum) apps get the same control shapes |
| #6 | Dock brought toward macOS parity |

### Features and polish (PRs #7 to #21)

| PR | Area | What it does |
|---|---|---|
| #7 | Island | Split Island: a second live activity sits in a bubble beside the capsule. Adds Do Not Disturb and charging pills. |
| #8 | Session menu | Rounded macOS-style cards and an "Are you sure" alert with a 60-second countdown |
| #9 | Notifications | macOS banners and time stamps, no countdown bar, history stacked by app, restyled history panel |
| #10 | Launcher | Raycast-style launcher: sections, action bar, file search, actions menu, clipboard history, quicklinks, snippets, scripts, time zones, system commands, aliases, unit conversion, and forms for creating quicklinks and snippets |
| #11 | Settings | Coloured macOS-style icon tiles in the sidebar |
| #12 | Qt 5 | qt5ct now picks up the Noctalia colours (its palette has 21 entries, not 22) |
| #13 | Dock | Red unread-count badges and a hairline glass edge, with toggles for both |
| #14 | Panels | Panels spring out of the Island; bar and floating panels slide in |
| #15 | Island | `noctalia msg island-activity-*` lets scripts post live activities (rings with progress) |
| #16 | Password prompt, tooltips | Polkit dialog styled as the macOS authentication alert; smaller, snugger tooltips |
| #17 | Island | Quick pills: scripts can flash short messages like "Copied" or "VPN on" |
| #18 | Settings, controls | Closes the remaining Ventura gaps: column width, separators, header switches, slider boxes, buttons, stepper, panel titles, Control Center date and profile card, tray drawer |
| #19 | Island | Up-next meeting: the next calendar event counts down in the Island |
| #21 | Island | Screenshot thumbnail: a new screenshot pops out of the Island with Markup. Enter opens it, Esc dismisses it. |
| #23 | Launcher | Raycast parity pass (two rounds): a searchable action panel with key hints over the results, Timers that count down in the Island, Kill Process, date arithmetic, calculator history, Pick Color, Dictionary (dictionaryapi.dev), Search Screenshots, Quick Notes; Suggestions (recent results across providers) on the root search, Esc back to root from a provider view, Ctrl+N/P, Tab autocomplete, fallback commands (`shell.launcher.fallbacks`), Window Management commands on Hyprland, Quit Application / Copy Name on apps, clipboard type filter, more snippet placeholders, Volume/Brightness/Reload Configuration system commands. Also builds against libwayland 1.22 again. |

## Next steps

1. **Try it on real hardware.** A few things can't be checked in the cloud and need a look on harvey's Hyprland machine:
   - the glass blur and vibrancy, and SF Pro rendering
   - a real polkit prompt and its wrong-password shake
   - the screenshot thumbnail and meeting countdown with real screenshots and a real calendar
   - clicks and hovers generally, since the cloud test setup can only send keys
2. **Known gaps to consider later:**
   - Raycast features still missing from the launcher: the floating note window itself (quick notes are captured and listed, but not shown in a window of their own), and snippet expansion while typing in other apps, which needs an input-method hook the shell doesn't have. Script arguments show their placeholders in the row rather than as inline fields. The Raycast passes added the searchable action panel, calculator history, date arithmetic, Kill Process, Timers in the Island, Pick Color, Dictionary, Search Screenshots and Quick Notes. Window Management's layouts use the classic Hyprland dispatchers (`setfloating`, `resizewindowpixel`, `movewindowpixel`); on a Lua-configured Hyprland only Toggle Fullscreen and Toggle Floating have Lua forms, so check the rest on the real machine.
   - Launcher snippets paste only from the launcher. They don't expand as you type in other apps.
   - Snippets are typed on one line, so new lines are written as `\n`.
   - Panels open a little more slowly with springs: about 270 ms to open (was 200) and 300 ms to close (was 190). This can be tuned in `src/ui/motion.h`.
   - The top bar only matters with the Island off, so it hasn't been a priority.
3. **Still on the shelf:** the floating Ventura-style Control Center, until harvey asks for it.

## Handy references

- Build: `just configure && just build`. Turn the Island on in config; it's off by default.
- Ubuntu 24.04 needs WirePlumber 0.5, sdbus-c++ 2.x, current stb headers and g++-14 built or installed by hand; the stock packages are too old. The shell itself builds against libwayland 1.22 again.
- Launcher screenshots from the headless Sway surveys of the Raycast passes (root search, action panel, Island timer, Kill Process, calculator history) are in [assets/launcher-raycast](assets/launcher-raycast/).
