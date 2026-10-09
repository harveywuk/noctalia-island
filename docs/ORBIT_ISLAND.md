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
- Compact playing activity and five-second track announcements in the clock slot.
  The title and artist appear on separate centred lines beside the artwork, with
  the animated artwork background and visualiser retained. Metadata received while
  paused announces when playback starts; resuming the same track stays quiet.
  Browser players that reuse track IDs or page URLs still announce changed titles.
  Previews briefly take priority over compact downloads and timers, then restore
  the current activity. Alerts, OSDs and open panels retain priority. The existing
  Track preview duration and monitor settings apply, including zero to disable.
  A five-band accent-coloured visualiser replaces the right-hand music glyph.
  It follows the default desktop audio output through the existing PipeWire spectrum
  service and settles when the output is silent.
  Once an auto-hidden Island is fully concealed, waveform sampling and the artwork
  animation stop. They resume on reveal. An open panel also suspends the Island's
  background animation until it returns, and artwork survives configuration reloads.
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
the two until the activity it brought forward ends. Expanding opens the activity
currently in the capsule. Choosing Media, Downloads, Timers or Awake in the expanded
card also selects it for the compact capsule while Split activities is on. Ending
the selected activity falls back to the configured activity order. Each monitor
keeps its own selection. Jobs from one app, including Steam games, swap by their
individual transfer identities. Replacing a bubble crossfades its content; a click
held across an activity ending cannot activate the replacement. Reduced motion
changes the content immediately. Hovering or an alert tucks the bubble back under
the capsule. Set `split_activities = false` to hide the second
activity instead, or to cycle activities with `cycle_activities = true`.

Turning Do Not Disturb on or off from a keybind or `noctalia msg notification-dnd-set`
shows a short pill drawn like the macOS Big Sur Control Center tile: a round toggle
(solid indigo with a white moon when on, grey when off), "Do Not Disturb", and On or
Off under it. Plugging in a laptop's charger shows a green "Charging" pill with the
battery level. Turn the charging pill off with `[osd.kinds] charging = false`.

Connection, Charging, Do Not Disturb and transfer-result cards centre their icon
and text together, using the same icon size, spacing and title weight. Two-line
cards centre their detail beneath the title; transfer results keep a single line.
Actionable cards share a subtle hover fill and keyboard focus outline. Media
artwork stays visible beneath these cards while playback continues.

Successive device, network and transfer-result cards gently fade their icon and
text when the visible message changes, even if the capsule stays the same size.
The group remains centred throughout. A burst replaces the incoming content with
the latest message instead of queuing older cards or restarting a still-dominant
outgoing fade. Battery percentages, signal updates, keyboard focus and identical
completion messages stay steady. These transitions preserve each event's expiry,
keep the artwork running underneath and settle immediately with animations disabled.

In the Cupertino look, microphone and brightness changes show as Big Sur's Display
module: the name and level over a white-filled groove with the symbol inside its leading
end. Volume is just a larger groove centred in the capsule, with no title, and the level
at its right with `volume_show_percentage`. The volume groove is `volume_bar_height`
plus 10 px thick (28 px by default). The theme look keeps its icon and accent bar.

Unread notifications in the Cupertino hover view are a macOS Notification Centre stack:
the newest as its own card (app icon, app name and time, title and two lines of body),
with up to two older cards peeking out beneath and an "N more" count. Clicking the stack
opens the notification history.

## Download indicators

Applications publishing `com.canonical.Unity.LauncherEntry.Update` progress appear
automatically. The compact capsule keeps its clock and adds a circular indicator
around the download icon. Reported percentages fill the ring, with equal weight
for each entry. An unknown total uses the shell's themed spinner. App names appear
only in the expanded view, keeping the compact activity indicator minimal. Hovering shows
separate application bars or Steam's current phase. With several entries the
compact number is the number of active entries, not a combined percentage.
The expanded card shows up to four rows and a remaining-item count. Rows have a
small gap, percentages align at the trailing edge, and entries without a reported
percentage give their title the full available width. The outer progress ring uses
a fine line with a faint coloured halo; its unfilled track stays subdued. Notifications
and OSDs retain priority, and the media panel remains accessible from the download
card. Hidden progress and disconnected apps disappear without assuming that a
cancelled download succeeded. Count/badge messages alone never create a download.

Running and paused rows are clickable when their source app has a window. Click
to return to the app, or use `noctalia msg island-focus`, Tab or Shift+Tab to select
a row, and Enter or Space to open it. A faint row highlight and keyboard outline
keep the artwork visible. Focus follows the same transfer through percentage
updates and reordered rows. Removing the selected row or closing its app releases
keyboard focus. Script activities without an app identity remain informational.

Confirmed completion shows a green checkmark, a green halo, and **Download finished**
for five seconds before returning to the current activity. This requires a desktop
app to report 100% after active progress, or Steam to report an explicit finished
update. Hiding, cancelling, or disconnecting alone does not signal success. Nearby
finishes share one notice and restart its five-second lifetime. Reduced motion
keeps the halo steady. Notifications, OSDs, keyboard controls, and open panels
retain priority. Activity rotation pauses while the notice is present, preserving
the current activity's remaining display time. A notice that expires underneath
an alert or open panel does not replay when that interruption ends.
When media is playing with the artwork background enabled, that background stays
visible through success and failure notices, downloads, timers and alerts. The
glow sits over it; playback and artwork updates do not replace the foreground card.

Hovering a notice shows its source app or script title. When the app still has a
window, click the notice to return to it. This matches the desktop ID and
`StartupWMClass`, without launching another process. Script notices without an
app identity show their title as information only.

Run `noctalia msg island-focus` while an actionable notice is visible, then press
Enter or Space to return to its app. Escape releases keyboard focus. The normal
five-second deadline still applies, and expiry or a replacement notice releases
the keyboard grab. A notice arriving while you are already using keyboard media
controls leaves those controls in focus.

An explicitly paused transfer shows an amber pause symbol and **Paused**. Its last
reported percentage stays visible; unknown totals stop spinning. Running jobs
appear before paused ones. A confirmed failure shows a red cross, red halo and
**Transfer failed** for five seconds, using the same interruption and reduced-motion
behavior as completion. Repeated reports of the same failure do not extend it.
These states require explicit Steam log events or script statuses. The standard
desktop progress signal provides neither pause nor error status, so a stalled
percentage or disappearing entry alone never produces either state.

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
installing and verifying states. The compact capsule, split bubble and expanded
rows use a download arrow, package or checked file to reflect the current stage.
Groups keep the download arrow, and script activities retain their custom symbol.
An explicit suspension keeps an amber paused row
until it resumes, is cancelled, completes, or the client exits. Explicit update
errors produce the failure notice. Other stopped updates clear the indicator;
log entries from earlier client sessions are ignored and replayed errors do not
produce new notices. This does
not modify Steam, install a plugin, or enable remote debugging. Steam's saved
byte counts are not reliable live percentages, so this reader shows activity
instead of an estimated percentage. Shader-cache downloads use the same phase
tracking. A successful scheduler removal after observed work also confirms
completion, including when Steam has already cleared its visible phase. Pauses,
cancellations and scheduler entries without observed work never imply success.
Native desktop progress, if Steam publishes
it, takes precedence over the log reader.

When the optional [leds-valve-shim](https://github.com/anna-oake/leds-valve-shim)
virtual driver is available, Noctalia reads `/dev/valve-leds-shim` directly and
mirrors its 17 RGB lights around the download ring with the same subtle halo.
Steam's moving highlight and partially lit edge come from the device's colours;
they are not converted into a numeric percentage or a completion event. Game
names, phases and confirmed completion still come from the log reader. Native
desktop percentages retain priority.

The LED pattern is used for a single actively downloading Steam game, including
its split bubble and inner ring when the outer ring is disabled. Other phases,
ambiguous groups and reduced motion keep the existing indicators. Snapshot
sampling is capped at 20 Hz, with repainting only when colours change. Missing,
disabled, malformed or more than five-second-old frames fall back automatically;
the slower Steam poll retries the device so reloading it needs no shell restart.
Noctalia opens it read-only and closes it after each sample, allowing module unload.

For investigation, `python3 scripts/steam-led-probe.py` prints changed version 1
snapshots as JSON for two minutes. `--seconds` adjusts the duration.
If the snapshot stays unchanged during a download, check that the desktop user
can write the driver's LED attributes under `/sys/class/leds/valve-leds[N]`.
Load the driver before starting Steam. If Steam was already running, restart the
client after loading the driver so it can discover the new LEDs.
The device reports colours, brightness and effects without identifying the writer
or active game. Noctalia does not install or load the kernel module. The standalone
probe is optional; the shell does not launch it. Tests can point the reader at a
private snapshot file using `NOCTALIA_STEAM_LED_DEVICE`.

For a persistent setup, register the driver with DKMS and `AUTOINSTALL="yes"`,
keeping headers installed for each kernel you boot. Add `leds-valve-shim` to
`/etc/modules-load.d/noctalia-steam-leds.conf` so it loads before Steam starts.
Desktop write permissions need to be reapplied whenever the device is created.
The udev rule should match the shim's `misc` device: it is registered after all
17 LEDs and their custom attribute groups exist. The local permission helper at
`/usr/local/libexec/noctalia-steam-led-permissions` reads the desktop account from
`/etc/noctalia-steam-led-user` and grants ownership of the shim's writable LED
attributes to that account. The snapshot device stays read-only.

Check a persistent installation with `dkms status -m leds-valve-shim` and
`modinfo -F filename leds-valve-shim`. The installed source and DKMS configuration
live under `/usr/src/leds-valve-shim-<version>`; kernel updates rebuild that source
through the distribution's DKMS hooks. This setup is separate from the shell's
local installer and does not require restarting the shell or Steam if the working
driver is already loaded.

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
for a spinner, and an optional new title. Start and update also accept one JSON
object, which is the way to pick an icon or report a status:

```sh
noctalia msg island-activity-start '{"id":"build","title":"Building","icon":"hammer","progress":75}'
```

Icons are the shell's Tabler glyph names. Starting an id that already exists
restarts it. An activity nobody updates for an hour is dropped, so a script that
dies doesn't leave it behind. Script activities share the downloads slot in the
activity priority setting, and the card's heading reads "In Progress" when one is
present. Without an icon, a script activity shows a terminal symbol.

To show the same green completion halo with **Transfer finished**, report 100%
before ending a running activity. Ending below 100%, ending a spinner, ending a
paused or failed activity, or expiring an abandoned activity simply removes it.

```sh
noctalia msg island-activity-update backup 100
noctalia msg island-activity-end backup
```

The optional JSON `status` is `running`, `paused`, or `failed`. Omitting it keeps
the current status, so progress updates alone cannot resume a paused or failed
job. Failure removes the job from the active list and shows the red notice once.
The id is retained until ended or expired; setting `running` retries the same job
with its title and progress intact. Starting the id again resets it completely.
Status display does not itself pause or retry the underlying process.

```sh
noctalia msg island-activity-update '{"id":"backup","status":"paused"}'
noctalia msg island-activity-update '{"id":"backup","status":"running"}'
noctalia msg island-activity-update '{"id":"backup","status":"failed"}'
```

## Quick pills

Scripts and keybinds can flash a short pill, like the Focus pill, with any icon:

```sh
noctalia msg osd-show clipboard-check Copied
noctalia msg osd-show shield-lock "VPN connected"
```

The icon is a Tabler glyph name. The pill fades after a moment. With the Island off
it shows as a regular OSD, and it shows even when `[osd.kinds]` hides the built-in
OSDs.

## Screenshot and recording menu

Capture is keyboard driven. Bind `noctalia msg capture-menu` to **Shift+Print**
to open the compact **Screenshot** and **Record** menu, or choose **Screenshot &
Recording** in the launcher. The hover Island has no capture launcher button.
Screenshots offer **Region**, **Window** and **Monitor**; recordings offer Region
and Monitor. Both support an optional 3, 5 or 10 second delay.
Tab moves between controls and the arrow keys change the focused group. Enter
activates a control; Escape closes the menu or cancels a selection
or countdown.

Window selection gives the visible application beneath the pointer a subtle tint
and shows its window title above the keyboard hints. Long titles are truncated to
keep the caption within the monitor. Click to
choose it, or cycle with Tab/Shift+Tab or the left/right arrows and press Enter.
The selected window is brought forward, and its bounds are checked again after the
countdown so a moved or resized window is captured at its new size. Closing the
selected window cancels the capture. Window selection currently uses Hyprland IPC
with visibility information and native window capture support; other desktops
report it unavailable. Window screenshots preserve native pixels and exclude the
Island, selection overlay, other windows and cursor.

Choose the capture area first. The Island then counts down with a Cancel button,
removes its capture controls and takes a fresh frame. Delayed screenshots stay
live during selection even when normal screenshots use the freeze preference.
The usual save, clipboard and annotation settings still apply. Pending captures
are cancelled when the session locks, configuration reloads or outputs change.
Playing media artwork keeps animating behind the menu and countdown. Ordinary
notifications wait behind these controls; critical alerts retain priority.

Recording adds **Off**, **Desktop** and **Mic** audio choices. Desktop records the
default output's monitor source; Mic uses the default microphone input. A missing
input reports an error instead of substituting desktop audio. Silent recording
does not need an audio server. Recordings use `wf-recorder`, appear in the existing
Screen Live Activity and save under the Videos directory's `Recordings` folder.
`noctalia msg record-stop` also cancels a recording that is still counting down.
The existing direct recording commands retain desktop audio by default.

## Capture previews

When the shell saves a screenshot, its "Screenshot saved" notification carries a
thumbnail at the card's right, like a macOS screenshot thumbnail. Clicking the card
opens the image, and its buttons are Markup (the shell's annotator) and Show in
Folder. `noctalia msg island-focus` reaches the same buttons from the keyboard:
Tab to one, Enter to use it, Escape to dismiss.

When a recording finishes, the Island shows **Recording saved** with its duration,
file size and a video thumbnail. **Play** opens the saved video in the default player;
**Show in Folder** opens its containing folder. The preview returns to the previous
activity after five seconds, or stays available while hovered or keyboard focused.
Its green completion glow lasts five seconds even when the card is held open.
Ongoing captures and urgent alerts keep their privacy or warning colour, and playing
media keeps its animated artwork underneath. Reduced motion uses a steady glow.

The thumbnail is decoded in the background with `ffmpeg`, with a three-second limit.
If it is unavailable or decoding fails, a video symbol appears and both actions still
work. A late thumbnail never reopens a dismissed notification or extends its lifetime.

## Battery indicators

Connecting a Bluetooth device or wired battery peripheral briefly shows a compact
card with its name, matching device symbol and charge percentage when available.
Unknown charge stays unlabelled. Headphones and speakers show **Audio output**
only when the active, available PipeWire output matches their Bluetooth address;
matching a friendly device name alone is not enough. The card updates if the
output changes during its lifetime, without extending the preview.

Click an audio device's card to open Control Centre's sound controls inside the
Island, or another Bluetooth device's card to open the Bluetooth page. A subtle
hover highlight and a tooltip describe the action. Hovering keeps the card in place
until its original deadline; it does not extend the preview. Wired battery cards
remain informational. Opening a card consumes its preview on every monitor while
the original battery glow continues.

`noctalia msg island-focus` selects a visible, actionable connection card. Tab and
Shift+Tab retain that action, Enter or Space opens it, and Escape returns keyboard
input to the desktop. Expiry, removal or replacement releases keyboard focus;
battery updates preserve it. Connections arriving while keyboard media controls
are already in use leave those controls in focus.

**Settings → Dynamic Island → Media → Device connection preview** controls the
card duration (five seconds by default, zero to disable). The existing
`bluetooth_preview_seconds` and `bluetooth_preview_monitor` keys control duration
and monitor targeting. The newest connection replaces the previous card, removal
dismisses it, and older connections never replay. Notifications, volume changes,
transfer notices, expanded controls and open panels retain priority. The preview
expires behind them, and playing media keeps its artwork background. System
charging retains its dedicated charging card.

The island reuses Noctalia's UPower and Bluetooth battery services. A system
battery appears in the compact capsule while charging or below its existing
warning threshold; peripheral batteries appear whenever they report a charge.
The circular fill represents the actual percentage, with a gentle charging pulse
that respects disabled animations. Low charge uses the theme's error colour.
The clock stays centred, and notifications and OSDs retain priority.

With the outer progress ring enabled, connecting a Bluetooth battery or wired
battery device, or plugging in the system charger, adds a soft ten-second glow
around the Island. It pulses green above 60%, amber from 20% through 60%, and red
below 20%. The small device indicator remains inside; battery charge no longer
draws a progress bar around the capsule. Percentage updates do not restart the
glow. It follows the Bluetooth preview monitor setting, independently of the
compact preview duration, and stays steady when animations are disabled. Recording
startup, desktop sharing, critical alerts and transfer notices take priority.

Hovering adds battery rows beneath the calendar, media or download content, with
device names, percentages, status and available time estimates. Healthy system
batteries are also visible here. Up to four devices are shown, prioritising low
and charging batteries. Bluetooth devices shared with UPower are deduplicated;
disconnected devices and absent/unknown packs do not create a false 0% indicator.
There is no new battery polling process or duplicate low-battery notification.
When the compact battery ring is present alongside a download, the download ring
remains visible and its percentage/count is available in the expanded view.

## Network connection cards

Connecting to Wi-Fi shows a centred **Connected to** card with the network name;
wired connections show **Ethernet connected**. A connection must settle for
750 ms before appearing. A lost connection must last 2.5 seconds before the Island
shows **Connection lost**, followed by **Connection restored** when the same
network returns. Short dropouts, startup, signal changes, scans, IP renewals and
VPN metadata stay quiet. This reports the network link, not Internet reachability.

Click the card to open Control Centre's network controls. The same action works
with `noctalia msg island-focus`, then Enter or Space. Hovering keeps the card
compact without extending its deadline, and opening it consumes the preview on
all monitors. Escape, expiry or replacement returns keyboard input to the desktop.

Cards last five seconds by default. **Settings → Dynamic Island → OSD → Network
connection preview** sets their duration, with zero disabling them. The
`network_preview_seconds` and `network_preview_monitor` settings also support
per-monitor overrides. Monitor targeting offers all outputs, the focused output
at the event, or a specific connector.

Notifications, OSDs, transfer notices, device connections, expanded controls and
open panels retain priority. Network cards expire behind them without replaying,
and playing media keeps its animated artwork beneath the card.

## Privacy indicators

Active microphone, camera and screen-sharing captures reported by Noctalia's
privacy service share one rotating slot in the compact island, alongside unread
history when present. It cycles every five seconds and pauses over the hovered
icon so its tooltip and click target stay put. The bell uses the same 16 px glyph
size as the privacy icons, both in the slot and when shown on its own. These icons
retain their status colours on hover and press, without a background highlight.
Hovering the island shows the capture icons in a centred row alongside media,
downloads and batteries.
Each icon's tooltip names the apps; multiple streams from the same app share an icon.
The existing `shell.privacy` filters apply; stopped captures disappear.

The orange microphone icon opens a Live Activity inside the Island. It names the
capturing apps and shows each linked microphone with a live input meter, an explicit
muted state and a mute/unmute button. Controls affect that input device, including
other apps using it, and never silently fall back to an unrelated default input.
The meter uses a fixed -60 to 0 dBFS scale. Passive monitors exist only while the
card is visible and the input is unmuted; audio samples are not retained. If no
input route can be resolved, the card shows an unavailable message instead of a mute button.
The card joins the expanded activity selector while capture is active, preserves
playing media artwork, and disappears when capture ends. Its settings button opens
the full Audio controls. External microphone mute changes update the card in place.

The green camera icon opens a Camera Live Activity with each capturing app,
its elapsed time, and **Open App** to reach its camera controls. It uses both
PipeWire capture links and the existing direct V4L2 device detection. Parallel
streams from one app share a row until its last stream ends. Camera and screen
sessions have independent timers, even when the same app uses both. The camera
card joins the activity selector and supports `island-focus`, preserving animated
media artwork, notification priority and the existing `cam_filter_regex` setting.
Its elapsed timer starts when the shell first detects the unfiltered capture,
survives configuration reloads, and resets after filtering or a shell restart.
Apps without an open window show feedback in the card.

The purple screen-sharing icon opens a Screen Live Activity. Each capturing app
has its own elapsed timer and **Open app** button. Timers start when the shell first
detects an active, unfiltered capture; multiple streams from one app share the timer
until its last stream ends. Reloading configuration or changing monitors preserves
the elapsed time. Restarting the shell or filtering the app starts a new observation.
Apps without an open window show feedback in the card.

Recordings started by the shell appear in the same activity with **Stop recording**.
Clicking the compact recording timer opens the card; stopping requires the explicit
button. While the encoder finishes, the card shows **Saving…** and disables Stop.
Stopping a shell recording leaves other apps' capture sessions running. External
capture controls remain in their owning apps. The card joins the activity selector,
supports `island-focus`, and keeps playing media artwork animated underneath.
Notifications and OSDs retain their normal priority while recording continues.
Starting a recording gives two gentle red pulses over 4.8 seconds, then the steady
red recording icon and timer carry its status. Opening the card, changing monitors
or reloading settings does not replay the cue. Reduced motion uses a brief steady
red glow with the same timeout. Microphone and camera activity use steady amber
and green icons without an outer red glow. Saving retains its five-second green
confirmation when no screen-sharing session or urgent alert takes priority.

The Camera and Screen activities' settings buttons open the Privacy tab in
Control Centre, which groups current captures by type and app. Volume and other OSDs keep their own layout. Hovering an icon
keeps it still for clicking; hovering the clock opens the expanded view.

Desktop sharing adds a purple glow around the Island for as long as a screen
capture link is present. It gently pulses without fading completely out, stays
visible with auto-hide, and follows the capsule into hosted panels such as Control
Centre. Reduced motion holds a steady glow. Recording startup and critical alerts
take colour priority; purple returns after the recording's two startup pulses.
The media artwork continues underneath, and the usual `screen_filter_regex`
applies. Stopping the last screen capture clears the sharing glow.

Screen detection uses PipeWire capture links and app metadata, including portal
screen sharing and remote desktop apps that use this path. An unlinked source or
an idle remote server does not activate it. Direct DRM, X11 or compositor capture
that bypasses PipeWire is outside this coverage; this is a capture indicator, not
a detector of every remote login. Cameras also include direct V4L2 device users.
The existing privacy OSDs are retained and no new polling process is added.

## Text capture, Keep Awake and Focus

Search the launcher for **Copy Text from Screen**, then drag over the text to copy.
Recognition runs locally with Tesseract; the Island briefly confirms completion.
Escape cancels selection. Empty results and recognition failures leave the clipboard
unchanged. Locking cancels pending text capture. No screenshot file or annotation
editor is created, regardless of the normal screenshot settings.

Install `tesseract` and the language data you need (`tesseract-data-eng` on Arch
for English). The advanced Screenshot settings expose recognition languages
(default `eng`, or multiple codes such as `eng+deu`) and an optional language-data
folder. The corresponding keys are `shell.screenshot.text_languages` and
`shell.screenshot.text_data_directory`. A missing engine or language model reports
failure without replacing the clipboard.

Control Centre's Power tab offers **Keep Awake** for 15, 30 or 60 minutes, until
turned off, or Off. Timed choices replace the previous deadline and release the
idle inhibitor when they expire. Choosing Until off cancels an existing deadline.
Launcher actions also provide the three timed choices. This prevents automatic
idle sleep using the existing compositor/logind support; it does not block manual
suspend. Timers are local to the running shell and reset on restart.

Timed Keep Awake sessions also appear as an Island Live Activity. When it is the
only activity, the compact capsule shows a cup and the remaining time. Alongside
media, downloads or timers it uses the existing split bubbles, activity cycle and
expanded selector; its default priority follows those activities. The expanded
card offers **+15 min**, **End** and a link to Power controls. Adding time extends
the current deadline rather than starting again from now. Countdown ticks preserve
the buttons and keyboard focus, and playing media retains its animated artwork.
The card disappears on expiry, End or switching to Until off. Control Centre,
launcher actions and IPC share the same timer; reloads preserve its deadline.

Control Centre's Focus tab offers **Work**, **Gaming** and **Sleep**. Each has an
editable comma-separated list of allowed app names or desktop IDs, an optional
critical-alert exception and a local-time schedule. App matching ignores case,
surrounding spaces and an optional `.desktop` suffix; it requires the full name.
Silenced notifications remain in history according to normal history rules, and
are not replayed when Focus ends. Existing notification-filter DND bypasses still
apply. Plain Do Not Disturb keeps its existing behavior without preset exceptions.

Schedules are disabled initially. Set the times and days, enable the schedule,
and choose **Save Focus**. Overnight times belong to their starting day, so a
Monday 22:00 to 07:00 interval ends Tuesday morning. Equal start/end times and an
empty day selection are rejected for enabled schedules. Overlapping schedules
prefer Sleep, then Gaming, then Work. Schedules are checked every 15 seconds.
A manual preset or Off lasts until the scheduled profile next changes; **Follow
schedules** resumes immediately. Saved rules persist; manual choices reset on
shell restart. The new brief Island feedback preserves playing media artwork.

**Focus while recording** in Control Centre's Focus tab is off by default.
When enabled, recordings started by the shell temporarily silence ordinary
notification banners and sounds, while critical alerts and explicit DND bypasses
remain allowed. The recording indicator stays visible. Suppressed notifications
remain in history under the normal retention rules and are not replayed afterward.
The temporary Focus ends when the recorder exits, including errors, before the
saved recording preview is posted. Cancelling selection or a countdown does not
change Focus. External screen sharing does not activate this option.

Your existing Focus or Do Not Disturb choice is kept underneath, and schedules
continue to advance. A manual Focus or DND choice takes precedence for the rest
of the recording, including across configuration reloads. The next recording can
activate the temporary Focus again. The saved setting is
`notification.focus.while_recording`; `focus-status` reports `recording` and
`automatic: true` while the temporary Focus is active.

IPC equivalents for scripts and keybinds:

```sh
noctalia msg text-capture
noctalia msg caffeine-for 30
noctalia msg caffeine-status
noctalia msg focus-set work
noctalia msg focus-set auto
noctalia msg focus-status
noctalia msg panel-open control-center privacy
```

## Timer and Pomodoro

The island integrates the existing [official Timer](https://github.com/noctalia-dev/official-plugins/tree/main/timer)
(`noctalia/timer`, tested with 1.2.1) and [community Pomodoro Timer](https://github.com/noctalia-dev/community-plugins/tree/main/pomodoro)
(`thepunkoff/pomodoro`, tested with 1.3.0). Enable them in Settings → Plugins and
start a countdown from the plugin's own panel; no bar or desktop widget is required.

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
or notification, active download, countdown, microphone, camera or screen capture, recording, or timed Keep Awake,
the command opens the existing calendar panel.

Bind `noctalia msg island-focus` to a shortcut in your compositor.

## Scope

This is the first native port, not complete Orbit parity. Noctalia's native panel contents now live inside the morphing capsule. The
standalone Settings window, persistent plugin windows, lock screen and desktop
editors retain their own hosts; they are not regular shell panels. Orbit's exact
glass deck styling, spectrum visualizer, notch mode, and smart hiding remain to be ported. Track announcements
use Noctalia's scrolling labels instead of Orbit's three-pass marquee. The hover calendar follows Orbit: today is centred between the three previous
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

The expanded media view also shows the system tray beneath its playback controls.
In Cupertino appearance the tray shares one centered row with privacy and unread
notification icons; other hover widgets remain in the idle view. Tray activation
and app menus work in both views. The tray respects `hover_show_tray` and takes no
space when there are no tray items.

The media row shows up to three tray apps, giving pinned apps priority and keeping
surviving apps in the same order. Extra apps open behind a small chevron in the
existing tray drawer below the Island. The media controls and privacy indicators
stay visible while the drawer is open. Hidden/passive app preferences still apply,
and explicitly enabling the tray's `drawer` option retains its pinned-only layout.
The overflow drawer grows and fades from its top edge on the Island's spring timing.
Its compact symbols have padded click targets and visible hover, press and keyboard
focus feedback. Turning off shell animations makes the drawer appear and dismiss
immediately.

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

### Glass

**Settings → Dynamic Island → Glass** (`[island] glass = true`) makes the Island,
and every panel it opens (Control Center, launcher, notifications), translucent over
the compositor's blur, with the same tint as the shell's glass panels. It needs a
compositor that blurs behind shell surfaces (`ext-background-effect-v1`); without one
the Island stays solid.

On Hyprland with the hyprglass plugin and **Glass** managed in the Hyprland settings,
the switch also turns on hyprglass's layer glass, so the Island gets Liquid Glass
(refraction, rim light) along with the dock and OSDs. The setting lives on the Island
bar (`[bar.<name>.island] glass`); the top-level `[island]` table only counts for the
legacy single Island.

hyprglass draws layer glass inside each surface's blur region, and up to v0.9.1 it does
so all-or-nothing per pixel, with at most 16 region rectangles. Rounded shapes therefore
end on a jagged edge. The local `noctalia/alpha-coverage` branch of hyprglass adds
`layers:alpha_coverage` (default 0.45), which fades the glass with the surface's own
alpha, and takes 64 region rectangles instead of 16. When the shell finds that option it
merges each glass region down to 64 rectangles, choosing the merges that add the least
area, so the region hugs the curve within a pixel or two and the drawn, antialiased
outline shapes the glass edge. A
bounding box would also glass whatever is drawn in its corners, such as the capture
glow. Without the option, the shell keeps exact strip regions. The wallpaper opts out with an empty region.

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
Orbit-specific recording shortcuts were removed during migration. The new capture
menu can be bound with `noctalia msg capture-menu`; existing capture shortcuts
remain in their current configuration.
Backups of the old startup, shortcut and idle files are in
`~/.local/state/noctalia-island/migration-backup/20260926-203655`.

For the current installation and session instructions, see [SETUP.md](SETUP.md).

The greeter output layout also mirrors the desktop: DP-1 at (0,0), DP-2 at
(350,1440) with a 180-degree transform and scale 1 on both outputs.

## Outer progress ring

Progress traces the Island’s edge rather than a small ring round the compact activity
icon (**Settings → Dynamic Island → Layout → Outer progress ring**, on by default). It
follows the capsule as it expands and scales. The split bubble does the same: a timer or
download beside the capsule rings the bubble’s own rim, with its symbol in the middle.
Turn the setting off for the small icon ring in the capsule.

Active timers take priority over downloads.
For multiple downloads, the outline shows their average progress; if any total is
unknown, a moving segment indicates activity. Battery connections use the
ten-second charge-coloured glow described above. Notifications, OSD cards and recording hide the
outline. Expanded rows retain their individual progress indicators.

```toml
[island]
outer_progress_ring = false   # the small icon ring instead
```
