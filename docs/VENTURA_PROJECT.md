# Noctalia Island: project summary

_State as of 3 October 2026, 10:50 UTC._

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

### Merged into feature/orbit-island (PRs #1 to #6)

| PR | What it does |
|---|---|
| [#1](https://github.com/harveywuk/noctalia-island/pull/1) | Ventura-style translucent glass for panels, toasts, OSDs and Settings |
| #2 | Spring motion for the Island expanding and collapsing |
| [#3](https://github.com/harveywuk/noctalia-island/pull/3) | Fonts, corner radius (16) and shadows moved toward Ventura; fanned hover calendar restored |
| [#4](https://github.com/harveywuk/noctalia-island/pull/4) | Shell controls resized to macOS proportions |
| #5 | GTK 3/4 and Qt (Kvantum) apps get the same control shapes |
| #6 | Dock brought toward macOS parity |

### Open drafts (PRs #7 to #17)

| PR | Area | What it does |
|---|---|---|
| [#7](https://github.com/harveywuk/noctalia-island/pull/7) | Island | Split Island: a second live activity sits in a bubble beside the capsule. Adds Do Not Disturb and charging pills. |
| [#8](https://github.com/harveywuk/noctalia-island/pull/8) | Session menu | Rounded macOS-style cards and an "Are you sure" alert with a 60-second countdown |
| [#9](https://github.com/harveywuk/noctalia-island/pull/9) | Notifications | macOS banners and time stamps, no countdown bar, history stacked by app |
| [#10](https://github.com/harveywuk/noctalia-island/pull/10) | Launcher | Raycast-style launcher: sections, action bar, file search, actions menu, clipboard history, quicklinks, snippets, scripts, time zones, system commands, aliases, unit conversion, and forms for creating quicklinks and snippets |
| [#11](https://github.com/harveywuk/noctalia-island/pull/11) | Settings | Coloured macOS-style icon tiles in the sidebar |
| [#12](https://github.com/harveywuk/noctalia-island/pull/12) | Qt 5 | qt5ct now picks up the Noctalia colours (its palette has 21 entries, not 22) |
| [#13](https://github.com/harveywuk/noctalia-island/pull/13) | Dock | Red unread-count badges and a hairline glass edge, with toggles for both |
| [#14](https://github.com/harveywuk/noctalia-island/pull/14) | Panels | Panels spring out of the Island; bar and floating panels slide in |
| [#15](https://github.com/harveywuk/noctalia-island/pull/15) | Island | `noctalia msg island-activity-*` lets scripts post live activities (rings with progress) |
| [#16](https://github.com/harveywuk/noctalia-island/pull/16) | Password prompt, tooltips | Polkit dialog styled as the macOS authentication alert; smaller, snugger tooltips |
| [#17](https://github.com/harveywuk/noctalia-island/pull/17) | Island | Quick pills: scripts can flash short messages like "Copied" or "VPN on" |

All eleven also merge cleanly together on the try-out branch `claude/try-all-drafts-lfjkuc`. It builds, and 151 of 152 tests pass. The one failure (`process`) happens only in the cloud sandbox.

### In progress right now

A survey of all the drafts together on 3 October found rough edges. They're being fixed in each area's thread:

- **Island:** two script jobs folded into a single download ring instead of using the split bubble. The fix is being written alongside round 2 (see below).
- **Notifications (#9):** the history panel and the banner's grey Snooze button are being restyled.
- **Launcher (#10):** the icon tab strip, and the old Clipboard panel that duplicates `/clip`.
- **Settings polish (new PR from the try-out thread):** content column width, stray separators, header switches, slider number boxes, button colours, the stepper, panel titles, the Control Center date format and profile card, and the empty tray drawer.
- **Sidebar icons (#11):** done.

**Island round 2**, in build order:

1. Quick pills: done as #17.
2. Screenshot thumbnail: a new screenshot pops out of the Island like on macOS, with Markup. Enter opens it and Esc dismisses it.
3. Up-next meeting: the next calendar event counts down in the Island. This needs a calendar source, which the shell doesn't have yet.

Items 2 and 3 were written and building at 10:37.

## Next steps

1. **Finish the current round.** The Island (round 2 plus the activities fix), notifications, launcher and Settings fixes report done.
2. **Merge everything.** The "Try all the drafts together" thread merges every open PR into `feature/orbit-island` one at a time, in PR order. Each PR is brought up to date and its conflicts fixed first. `main` stays untouched. harvey approved this at 10:42; it hasn't started yet.
3. **Try it on real hardware.** A few things can't be checked in the cloud and need a look on harvey's Hyprland machine:
   - the glass blur and vibrancy, and SF Pro rendering
   - a real polkit prompt and its wrong-password shake
   - clicks and hovers generally, since the cloud test setup can only send keys
4. **Known gaps to consider later:**
   - Launcher snippets paste only from the launcher. They don't expand as you type in other apps.
   - Snippets are typed on one line, so new lines are written as `\n`.
   - Panels open a little more slowly with springs: about 270 ms to open (was 200) and 300 ms to close (was 190). This can be tuned in `src/ui/motion.h`.
   - The top bar only matters with the Island off, so it hasn't been a priority.
5. **Still on the shelf:** the floating Ventura-style Control Center, until harvey asks for it.

## Handy references

- Build: `just configure && just build`. Turn the Island on in config; it's off by default.
