# Dynamic Noctalia: Island guide

Dynamic Noctalia is an independent Noctalia fork with an opt-in native C++/Wayland
island based on the local Orbit
Quickshell implementation (`Island.qml` and `IslandGlance.qml`). It uses Noctalia's
notification manager, MPRIS service, OSD routing, renderer, and animation settings.

The Island provides:

- A centered theme-coloured clock capsule, with the current Orbit profile's 64 px height,
  24 px clock and 1.1 UI scale in the example profile.
- A 110 ms hover delay and 180 ms leave delay, with a 420 ms size transition.
- A week strip when idle, and artwork, track/artist, seeking and transport controls
  while playing. Pausing from the island holds the media controls until hover ends.
  Long titles scroll using Noctalia's existing label control; short titles stay still.
  Playback time and progress update without restarting the title's scroll.
  The player name beneath the artist opens the existing media panel, whose
  headphones button selects a player. Dragging the seek bar previews the target
  time in the theme accent colour and commits the seek on release.
  Playback controls use Noctalia's themed buttons, with hover/press feedback and
  delayed tooltips. Unavailable controls stay disabled.
- Compact playing activity and brief track announcements in the clock slot.
  A five-band accent-coloured visualiser replaces the right-hand music glyph.
  It follows the default desktop audio output through the existing PipeWire spectrum
  service and settles when the output is silent.
- A small accent-coloured unread-notification bell on the clock and media island. The badge
  opens the existing notification history, which marks entries as seen. Hovering
  over the compact badge keeps it in place; it adds no width to the island.
  The expanded calendar and media views show an unread-count row that opens history.
- Volume, microphone, brightness and other existing OSD events in the capsule.
- The latest notification, dismissal and up to three action buttons, honoring DND
  and pausing notification expiry on hover. Notification history stays in Noctalia.
  Inline replies use Noctalia's existing toast reply editor.
- Control-centre navigation and page actions share one horizontal toolbar at the top of the island, with a subtle divider and no separate page-title row.
  Cards fit the measured active content and animate as tabs, notification history,
  or launcher results change. Long lists stay scrollable within the screen limit.
- Home’s settings button opens Control Centre settings with Home options first.
  Add/remove Profile, Media, Clock and weather, and Shortcuts cards; use the
  stacked layout to arrange full-width cards with the list’s up/down controls.
  The dashboard remains the default. Changes are saved by the existing settings
  system and appear when Home is reopened.
- Native shell panels expand inside the same island surface: control centre and
  its tabs, launcher, clipboard, wallpaper picker, session menu, and other regular
  panels. Escape, close buttons and outside clicks collapse directly to the current
  island state (including the wider playing pill), with an early content fade.
  Text inputs, tab controls, menus, service updates and popup parenting stay with
  Noctalia's panel manager. Keyboard shortcuts/IPC also use the configured island.
- A stable output-sized transparent surface, with input restricted to the visible
  capsule and optional reserved desktop space. Opening/closing panels changes only
  scene geometry, avoiding compositor resize/recentring animations.
  Concealed controls do not accept clicks during expansion, and replacing a card
  cancels any click begun on its previous contents.
- Monitor selection and live configuration reloads.

An explicitly opened panel stays interactive until dismissed. Notifications and
OSDs continue updating their service state in the background. When no panel is
open, notifications and OSDs take priority. Hover shows downloads, media, or the
calendar; otherwise an active countdown precedes downloads, media, and the idle clock.

When two activities run at once, the Island splits as on iPhone: the capsule shows
the first in the activity order and a round bubble beside it shows the next (album
art for media, a progress ring for a timer or download). Clicking the bubble swaps
the two until the activity it brought forward ends. Hovering or an alert tucks the
bubble back under the capsule. Set `split_activities = false` to hide the second
activity instead, or to cycle activities with `cycle_activities = true`.

Turning Do Not Disturb on or off from a keybind or `noctalia msg notification-dnd-set`
shows a short pill: an indigo moon, "Do Not Disturb", and On or Off. Plugging in a
laptop's charger shows a green "Charging" pill with the battery level. Turn the
charging pill off with `[osd.kinds] charging = false`.

## Download indicators

Applications publishing `com.canonical.Unity.LauncherEntry.Update` progress appear
automatically. The compact capsule keeps its clock and adds a circular indicator
around the download icon. A single reported percentage fills the ring; Steam
activity and multiple entries use the shell's themed spinner. App names appear
only in the expanded view, keeping the compact activity indicator minimal. Hovering shows
separate application bars or Steam's current phase. With several entries the
compact number is the number of active entries, not a combined percentage.
The expanded card shows up to four rows and a remaining-item count. Notifications
and OSDs retain priority, and the media panel remains accessible from the download
card. Hidden progress and disconnected apps disappear without assuming that a
cancelled download succeeded. Count/badge messages alone never create a download.

For Zen, run `python3 scripts/install-zen-download-progress.py`, then install
[LauncherEntry Integration](https://addons.mozilla.org/firefox/addon/launcherentry-integration/)
in Zen. The signed add-on needs downloads and native-messaging permissions. This
fork supplies its local host in `~/.local/share/noctalia/native-messaging/`, with
the manifest in `~/.mozilla/native-messaging-hosts/`. No browser restart is needed
when installing the add-on for the first time; if it was already installed, toggle
it off and on to reconnect to the newly installed host.

The bridge receives only count and progress, retains no history, and republishes
its current state when the shell restarts. The upstream add-on reports the first
active download's percentage; it does not expose filenames, speeds, per-file
controls, or a byte-weighted overall percentage. Other apps must publish progress
themselves: this is not a system-wide network-traffic detector. The integration
protocol is documented in the [Unity Launcher API](https://wiki.ubuntu.com/Unity/LauncherAPI).

Steam also has a built-in read-only activity reader. Every two seconds it checks
the running client's content log and resolves game names from its library
manifests, including external libraries. The island shows preparing, downloading,
installing and verifying states. Paused/stopped updates and an exited client clear
the indicator; log entries from earlier client sessions are ignored. This does
not modify Steam, install a plugin, or enable remote debugging. Steam's saved
byte counts are not reliable live percentages, so this reader shows activity
instead of an estimated percentage. Native desktop progress, if Steam publishes
it, takes precedence over the log reader.

## Script activities

Scripts and keybinds can post their own live activity. It shows like a download:
a ring around its icon with its title in the capsule, and a row in the hover card.
With Split activities on, a second job (or a download) sits in the bubble beside
the capsule; clicking the bubble swaps them.

```sh
noctalia msg island-activity-start backup "Backing up Documents"
noctalia msg island-activity-update backup 40           # 40 %
noctalia msg island-activity-update backup - "Verifying" # spinner, new title
noctalia msg island-activity-end backup
```

`start` begins with a spinner; `update` takes a percentage from 0 to 100, or `-`
for a spinner, and an optional new title. Each command also accepts one JSON
object, which is the only way to pick an icon:

```sh
noctalia msg island-activity-start '{"id":"build","title":"Building","icon":"hammer","progress":75}'
```

Icons are the shell's Tabler glyph names. Starting an id that already exists
restarts it. An activity nobody updates for an hour is dropped, so a script that
dies doesn't leave it behind. Script activities share the downloads slot in the
activity priority setting, and the card's heading reads "In Progress" when one is
present. Without an icon, a script activity shows a terminal symbol.

## Quick pills

Scripts and keybinds can flash a short pill, like the Focus pill, with any icon:

```sh
noctalia msg osd-show clipboard-check Copied
noctalia msg osd-show shield-lock "VPN connected"
```

The icon is a Tabler glyph name. The pill fades after a moment. With the Island off
it shows as a regular OSD, and it shows even when `[osd.kinds]` hides the built-in
OSDs.

## Battery indicators

The island reuses Noctalia's UPower and Bluetooth battery services. A system
battery appears in the compact capsule while charging or below its existing
warning threshold; peripheral batteries appear whenever they report a charge.
The circular fill represents the actual percentage, with a gentle charging pulse
that respects disabled animations. Low charge uses the theme's error colour.
The clock stays centred, and notifications and OSDs retain priority.

Hovering adds battery rows beneath the calendar, media or download content, with
device names, percentages, status and available time estimates. Healthy system
batteries are also visible here. Up to four devices are shown, prioritising low
and charging batteries. Bluetooth devices shared with UPower are deduplicated;
disconnected devices and absent/unknown packs do not create a false 0% indicator.
There is no new battery polling process or duplicate low-battery notification.
When the compact battery ring is present alongside a download, the download ring
remains visible and its percentage/count is available in the expanded view.

## Privacy indicators

Active microphone, camera and screen-sharing captures reported by Noctalia's
existing PipeWire service appear as themed icons in the compact island. Hovering
the island shows one row per capture type with the app names, alongside media,
downloads and batteries. Multiple streams from the same app share a row.
The existing `shell.privacy` filters apply; stopped captures disappear.

The microphone icon opens the existing audio controls. Camera and screen-sharing
icons expand the app details; stopping those captures remains in the owning app.
Capture icons also remain visible beneath notifications and OSDs. Hovering an icon
keeps it still for clicking; hovering the clock opens the expanded view.

This has the same detection coverage as Noctalia's native privacy widget: it uses
PipeWire capture links and app metadata, not a separate hardware-access monitor.
Applications that bypass PipeWire are outside that detection. The existing privacy
OSDs are retained and no new polling process is added.

## Timer and Pomodoro

The island integrates the existing [official Timer](https://github.com/noctalia-dev/official-plugins/tree/main/timer)
(`noctalia/timer`, tested with 1.2.1) and [community Pomodoro Timer](https://github.com/noctalia-dev/community-plugins/tree/main/pomodoro)
(`thepunkoff/pomodoro`, tested with 1.3.0). Enable them in Settings → Plugins. The
expanded calendar gains Timer/Pomodoro buttons that open their existing panels
inside the island; no bar or desktop widget is required.

An active countdown takes the compact activity slot, showing a circular remaining
fraction and the time remaining in the centre. Hover restores the clock/calendar,
media or download view and adds timer controls. Pause/resume and Cancel send the
plugins' own commands; Open returns to their full panel. Pomodoro displays the
current focus or break phase, and Cancel resets its session cycle. With both
active, both have separate hover controls; a running timer takes precedence over
a paused one, then the plain timer takes precedence over Pomodoro.

The plugin service remains the sole countdown and notification owner. There is no
second clock or duplicate completion alert. Disabling a plugin removes its island
state. Existing upstream limitations apply: these plugin versions initialise their
timers afresh on shell/service restart and do not persist an in-progress countdown.
Tests use copies of the actual upstream plugins in a private session, with a
test-only state observer; production plugin files are not patched.

## Up next

When the shell's calendar is set up (Settings → Calendar: Google, CalDAV or local
vdir calendars), the next timed event counts down in the timers slot. It appears
`up_next_minutes` before it starts (default 10; 0 turns it off), with a blue ring
that empties as the start nears, and reads "Now" for its first five minutes.
Hover shows the event with Join (its meeting link, when it has one), Dismiss and
Open Calendar. All-day events are skipped. It shares the timers slot in the
activity priority setting, and the calendar's own reminder notifications still
fire as before.

## Crowded activity layouts

Expanded activity views keep their main content fixed and scroll the extra timer,
privacy, battery, and notification rows when the output is too short. The card
leaves clearance at the bottom, including at larger UI scales. Keyboard focus
reveals controls inside the scrolling area. Download progress updates preserve
long-name scrolling and the footer position; hidden downloads do not rebuild a
compact countdown. The download card's Close button follows all activity rows.

## Keyboard navigation

`noctalia msg island-focus` explicitly focuses the island on the pointer's monitor.
Tab and Shift+Tab move between activity or notification buttons; Enter activates the
highlighted button. Focus stays on playback when its icon changes, and timed
notifications remain visible while focused. Escape releases focus and collapses
media, or dismisses the focused notification. An incoming notification releases
focus so it cannot inherit a key press intended for the previous content.
Hovering alone leaves keyboard focus with the current app. With no media player
or notification, active download, or countdown, the command opens the existing calendar panel.

The local Umbriel shortcut is Super+Alt+I:

```toml
[keybinds]
"Mod+Alt+I" = { action = "spawn:/home/mrpickles/.local/bin/noctalia msg island-focus", repeat = false }
```

## Scope

This is the first native port, not complete Orbit parity. Noctalia's native panel contents now live inside the morphing capsule. The
standalone Settings window, persistent plugin windows, lock screen and desktop
editors retain their own hosts; they are not regular shell panels. Orbit's exact
glass deck styling, spectrum visualizer, notch mode, and smart hiding remain to be ported. Track announcements
currently ellipsize instead of using Orbit's three-pass marquee. The hover calendar follows Orbit: today is centred between the three previous
and next days, with a larger accent date and fading outer columns. Weekday labels
use the process locale, with three-letter abbreviations centred in equal-width
columns above the dates.

Notification cards fit their measured title, body, and optional action row rather
than reserving a fixed-height card. Long notifications wrap up to a 360-pixel
logical height limit (or the available output height), then ellipsize.
Clipped messages expose a “Read full notification” button; clicking the message
also expands it into a scrollable reading view, capped at 640 logical pixels or
the available screen height. The header and action buttons remain visible.
Collapse restores the compact card. A default app action is available as “Open”
in the expanded view. Dismiss, expand/collapse and action buttons use Noctalia's
themed hover/press feedback and tooltips.
Notification bodies received over D-Bus retain up to 64 KiB, replacing the old
1 KiB cutoff so expanding a long message can show its remaining text.

The island and its expanded panels use the theme's Surface background. Text,
icons, calendar accents, and progress bars follow the palette live, including
light/dark changes.

## Add widgets to the hover view

Open **Settings → Island → Widgets** to add, remove or reorder modules in the
**Left widgets**, **Centre widgets** and **Right widgets** columns. Drag a card by
its handle to reorder it or move it between columns; the insertion line shows where
it will land. Dropping outside the columns cancels the move. The arrows provide the
same moves with ordinary buttons, including keyboard navigation.

Use **+** to open the searchable widget picker, the **gear** to open a widget's
settings (including plugin widget options), and **×** to remove it from that column.
This uses the bar widget catalogue, including installed and enabled plugin
`[[widget]]` entries and configured custom widget instances. Widgets appear beneath
the calendar, media or downloads content when you hover over the Island. They wrap
onto additional rows, and the footer scrolls if it exceeds the screen height.
Groups share a row when they fit; crowded groups stack while keeping their alignment.
The centre group stays centred where space permits, shifting only to avoid its neighbours.

```toml
[island]
enabled = true
hover_widgets = ["volume", "network", "weather", "my_button", "author/plugin:widget"]
hover_widgets_center = ["clock"]
hover_widgets_right = ["battery"]
```

Use only plugin IDs that are installed and enabled. The picker can create named
custom widget instances, or you can define a `[widget.my_button]` configuration table.
Their options and gesture bindings are shared with the normal bar;
use a separate named instance for different settings. Middle-click a widget to open
its settings (unless you have assigned a different middle-click action).
Plugin widgets receive the bar
context `island` and the Island's output. Other plugin entry types, such as desktop
widgets or background services, are not bar widgets and do not appear in this list.

All three lists are empty by default. Existing `hover_widgets` entries become the
left group automatically. Removing an item from a list does not uninstall its
plugin or remove it from another bar. Widget runtimes stop when the hover view closes.

### Layout presets

The Widgets editor offers **Minimal** (a clock), **Media** (volume, media and an
audio visualizer) and **System Monitor** (system stats, network and battery).
Applying a preset replaces all three hover widget lists and the hover section
choices together. It does not change widget options, compact Island settings,
normal bars or installed plugins. **Undo preset** restores the layout from just
before the last preset was applied; it is available for the current shell session.

### Optional hover sections

Under **Settings → Island → Hover Sections**, toggle the built-in clock, calendar,
media controls, downloads, timers, battery details and unread notification count.
All are enabled by default. Turning off the clock and calendar gives idle widgets
their own compact hover row. Turning off media or downloads leaves the other enabled
hover content available while that activity runs.

These options only affect the hover view. Compact activity indicators, incoming
notifications, volume OSD and capture indicators keep working.

```toml
[island]
hover_show_clock = false
hover_show_calendar = false
hover_show_media = true
hover_show_downloads = true
hover_show_timers = true
hover_show_batteries = true
hover_show_unread = true
hover_show_tray = true
```

## Build

Follow `BUILDING.md`. The branch is based on Noctalia v5.2.0, commit ec704377180f.

```sh
meson setup build-island -Dbuildtype=debugoptimized -Dtests=enabled
ninja -C build-island noctalia island_state_test config_schema_roundtrip_test config_path_resolution_test
meson test -C build-island --no-rebuild island_state config_schema_roundtrip config_path_resolution
```

On this workspace the missing libqalculate, nlohmann-json and stb packages were
extracted into the ignored `.deps/` directory. Meson already records the required
include paths and local library location. No system packages were installed.
libqalculate's optional data files still expect a normal system installation, so
calculator unit/currency definitions are unavailable in this local build.

## Try a separate profile

`examples/orbit-island.toml` is the legacy standalone Island profile used by
`tests/island_smoke.py`; for a current desktop start from
[`examples/starter.toml`](../examples/starter.toml) instead. The legacy profile enables the
island and disables the normal bar. To use
only the monitor from the existing Orbit profile, set `monitors = ["DP-1"]`.
Configuration homes are XDG-style roots: Noctalia appends `/noctalia` itself.

```sh
mkdir -p "$HOME/.local/state/noctalia-island/config/noctalia"
cp examples/orbit-island.toml "$HOME/.local/state/noctalia-island/config/noctalia/config.toml"
NOCTALIA_CONFIG_HOME="$HOME/.local/state/noctalia-island/config" \
NOCTALIA_STATE_HOME="$HOME/.local/state/noctalia-island/state" \
  build-island/noctalia
```

Run it in a separate compositor or after stopping the current shell; desktop
notification and tray ownership are shared within a D-Bus session. No autostart or
Orbit service files are changed by this branch.

The feature defaults off. To add it to another profile:

```toml
[island]
enabled = true
monitors = ["DP-1"]
height = 64
clock_size = 24
scale = 1.0
reserve_space = true
clock_seconds = false
```

`scale` multiplies `accessibility.ui_scale`. An empty monitor list shows an island
on every output; an explicit list has no fallback to unmatched outputs.

## Isolated interaction test

```sh
python3 tests/island_smoke.py --output /tmp/noctalia-island-preview
```

The test requires Sway, grim, a C compiler, wayland-scanner, Python GObject bindings, Pillow and xkbcommon headers. It creates a private D-Bus
session, headless compositor, XDG directories and mock MPRIS player. `--sway` and
`--binary` accept alternate executables. Captures show the idle, calendar, activity,
media, paused, brightness OSD, notification, DND and enable/disable states. A pixel comparison checks that an OSD arriving behind a notification does not alter its visible content. The script checks
that transport and seek gestures invoke the mock player's methods, and cleans up
its own processes on success or failure. The captures still need visual review.

Validation on 2026-09-26: the native build and three state/schema tests pass.
The headless interaction test verifies mock-player pause/resume and seeking,
brightness OSDs, notification priority, DND, and enabling/disabling the island.
The expanded test opens control-centre tabs and the launcher, types into search,
dismisses by Escape and outside click, verifies restoration of the desktop, and
disables the island while a panel is collapsing. A Wayland protocol trace checks
that these panels never create a separate panel layer surface. Transition captures
check that closing during playback never undershoots to the idle-clock width;
interrupting an opening animation is also exercised. The test checks a fixed
capsule centre and verifies that its Wayland viewport never resizes during panel
transitions. Content-fit checks cover empty/populated/cleared notifications,
height stability while idle, top-navigation clicks and empty launcher results.
On Hyprland, the island uses
the same delayed Exclusive-to-OnDemand keyboard transition as ordinary panels so
its focus grab can dismiss on outside clicks.
The isolated session has no audio output, so volume routing is compiled but has
not been exercised against a real PipeWire sink.

Reviewed captures: [idle](island-preview/rest.png), [calendar](island-preview/calendar.png),
[media](island-preview/media.png), [embedded control centre](island-preview/control-center.png),
[embedded launcher](island-preview/launcher.png), [notification priority](island-preview/notification.png).

## Island settings

Open **Settings → Dynamic Island** (or `noctalia msg settings-open island`).
Clock size, seconds, compact and expanded clock positions, weekday labels,
expanded media artwork size, and volume bar thickness/percentage are adjustable
live. The section also exposes island height, scale, and reserved desktop space.
Changes use the existing settings persistence and reset controls; no rebuild is
needed. Defaults preserve the current appearance.

```toml
[island]
clock_offset = 0                 # -12..12; negative moves up
expanded_clock_offset = 0        # -12..12; relative to the raised calendar clock
calendar_labels = "abbreviated" # initials | today | abbreviated
media_artwork_size = 56          # 40..80; expanded media card
volume_bar_height = 18           # 5..24
volume_show_percentage = false
```

Larger artwork increases the media card height and moves its seek/transport
controls with it. Single-letter calendar mode uses a narrower card.

## Home layout

The same preferences can be set in the profile:

```toml
[control_center.home]
cards = ["profile", "media", "clock", "shortcuts"]
stacked = false
```

Omit a card to hide it. `stacked = true` follows the list order and scrolls when
needed; the dashboard retains its profile-above, shortcuts-beside layout.
An empty list leaves the toolbar and a prompt to add cards. Unknown and repeated
card names are ignored by the layout. The existing `control_center.shortcuts`
list controls which shortcut buttons appear.

## Installed desktop on this machine

The default shell is now `noctalia-island.service`; `orbit.service` and
`orbit-display.service` are disabled. The installed launcher is
`~/.local/bin/noctalia`, with the binary, assets and private calculator library in
`~/.local/lib/noctalia-island`. The launcher preserves the existing live config
and state under `~/.local/state/noctalia-island`, including GUI preferences.

To install or update the fork, run `scripts/install-local.sh` as described in the
[setup guide](SETUP.md); it builds, tests, installs, keeps the previous binary and
restarts the running unit.

Hyprland's shell shortcuts and lock/idle commands now use Noctalia. The two
Orbit-specific recording shortcuts are removed because this port has no recording
panel yet. Other capture shortcuts remain in their existing configuration.
Backups of the old startup, shortcut and idle files are in
`~/.local/state/noctalia-island/migration-backup/20260926-203655`.

Umbriel is built separately in `~/src/umbriel`, with its portal in
`~/src/xdg-desktop-portal-umbriel`. Its configuration is
`~/.config/umbriel/config.toml`. It keeps Hyprland available as a separate session.
The user service launches the same Noctalia installation in either compositor.

Umbriel and its portal are installed under `/usr/local`; Noctalia Greeter 1.5.0
and Xwayland Satellite are installed through pacman. `/etc/greetd/config.toml`
now launches `/usr/bin/noctalia-greeter-session`. The initial greeter selection is
Hyprland; select Umbriel to try the new compositor. greetd was not restarted during
installation, so the old login greeter remains until greetd restarts; reboot when ready for the new login screen.
The original greetd/PAM files and installed-file manifest are backed up in
`/var/backups/noctalia-migration-20260926-204306`.

The install staging and system installer are under `~/.local/state/noctalia-install`.
`check-umbriel.py` and `check-greeter.py` there run isolated headless checks. These
passed with two outputs, including island panel rendering and the greeter's first
presented frames. A real hardware login/authentication was not attempted.

To restore the old user shell, restore the backed-up Hyprland files, run
`systemctl --user disable --now noctalia-island.service`, then
`systemctl --user enable --now orbit.service orbit-display.service` and reload
Hyprland. To restore the old login greeter, copy the backed-up greetd config back
as root before restarting greetd or rebooting; do not restart greetd inside a session you need to
keep. Umbriel does not replace or uninstall Hyprland.

The greeter output layout also mirrors the desktop: DP-1 at (0,0), DP-2 at
(350,1440) with a 180-degree transform and scale 1 on both outputs.

## Outer progress ring

Enable **Settings → Dynamic Island → Layout → Outer progress ring** to trace
progress around the Island’s edge instead of the compact activity icon. It follows
the capsule as it expands and scales. The default remains the small icon ring.

Active timers take priority over downloads, followed by the battery indicator.
For multiple downloads, the outline shows their average progress; if any total is
unknown, a moving segment indicates activity. Charging batteries pulse and low
batteries use the warning colour. Notifications, OSD cards and recording hide the
outline. Expanded rows retain their individual progress indicators.

```toml
[island]
outer_progress_ring = true
```
