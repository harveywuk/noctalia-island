# Hyprland integration checks

Dynamic Noctalia supports Hyprland's Lua dispatch interface and the older
Hyprlang interface. Lua selectors are escaped before dispatch, including named
workspaces containing quotes, backslashes, or control characters. Window focus
and raising use the same interface as the running compositor.

Open the keyboard-driven capture menu with `noctalia msg capture-menu`. For a Lua
configuration, bind it with
`hl.bind("SHIFT + Print", hl.dsp.exec_cmd("noctalia msg capture-menu"))`.

## Automated coverage

`hyprland_workspace_backend_test` checks workspace snapshots, event handling,
legacy focus/raise commands, Lua focus/raise commands, and selector escaping.
It runs in the normal Meson test suite.

For an interactive check without leaving the current desktop:

```sh
meson compile -C build-rishot
python3 tests/hyprland_smoke.py
```

This opt-in smoke test requires Hyprland 0.56 with Lua configuration, Labwc,
a working GPU render node, Kitty, Grim, PipeWire, WirePlumber, pipewire-pulse,
pactl, wf-recorder, FFprobe, GDBus, xdg-desktop-portal-hyprland, a C compiler,
Wayland client development files, and wayland-scanner. It must be able to open
local display sockets and access the render node. Set `WLR_RENDER_DRM_DEVICE`
to choose a GPU explicitly.

The test starts a private headless Labwc parent and nested Hyprland with its own
D-Bus, settings, PipeWire server, and silent audio sink. It disables seat access
and systemd environment imports. It does not switch the logged-in desktop or
use the session's audio server. All child processes are stopped on completion.
Screenshots, recordings, and logs remain under `build-rishot/hyprland-smoke-*`.

For Island animation and idle-work regressions, run
`python3 tests/hyprland_smoke.py --island-performance-only`. This also requires
Python Pillow and `pw-play`. It measures complete ten-second intervals with real
audio on the private sink: playing media with a desktop widget, auto-hidden Islands,
Control Centre, paused media, and reduced motion. It checks artwork after reload,
reveal and panel return, plus keyboard transport controls. Hidden and stationary
states must settle below render and timer-wake limits; CPU usage is recorded for
comparison, without a machine-dependent pass threshold. Results are saved in
`island-performance.json` beside the captures. Use `NOCTALIA_TEST_BINARY` to select
a build, or add `NOCTALIA_TEST_MEASURE_ONLY=1` to record an older build's baseline
without enforcing the new idle-work and reload-artwork checks.

For transfer feedback and interruption checks, run
`python3 tests/hyprland_smoke.py --island-completion-only`. It needs Python Pillow
and Tesseract with English data (the existing `NOCTALIA_TEST_TESSDATA` override
selects its data directory). The suite captures the green halo and labels, checks
the five-second lifetime, duplicate and overlapping finishes, return to media or
remaining downloads, and OSD/notification/panel/keyboard interruptions. It samples
the spring resize between media, transfers and notices to catch black artwork
frames. It also checks animated artwork beneath completion and alerts, playback changes while a
notice remains visible, amber pause/resume, red failure timing and retry, activity
rotation, reduced motion, and saves light/dark and fractional-scale references.
Like the other compositor suites, this uses the GPU; run it when the desktop can
spare that workload. `NOCTALIA_TEST_BINARY` selects the build to test.

For notice actions, run `python3 tests/hyprland_smoke.py --island-transfer-actions-only`.
It checks app/title tooltips, desktop ID and WM-class matching, click and Enter/Space,
keyboard release on expiry/replacement, cancelled pointer gestures, closed apps,
and Steam failure actions using private fixture windows and a private Steam log.
Running and paused rows also cover Tab/Shift+Tab, focus through progress updates
and reordered rows, keyboard release on removal, and artwork beneath row feedback.
It uses the same screenshot/OCR dependencies as the completion suite, plus Kitty.

For battery connection feedback, run
`python3 tests/hyprland_smoke.py --island-battery-glow-only`. Private BlueZ and UPower
fixtures check the ten-second halo, charge colour boundaries, percentage updates,
disconnects, wired devices and charger transitions. GPU captures verify pulsing,
reduced motion, media artwork, notification priority and fractional scaling.

For device connection cards, run
`python3 tests/hyprland_smoke.py --island-connection-only`. It uses the same private
hardware services with fake audio sinks to check confirmed output identity,
unknown and late battery data, expiry, disconnects, wired devices, interruptions,
media artwork, monitor targeting and fractional scaling. It also checks card hover,
pointer and keyboard actions, Control Centre destinations, consumed previews and
focus release on expiry, disconnection and replacement. It needs Kitty, Pillow and
Tesseract with English data, as in the transfer suites.

For network connection cards, run
`python3 tests/hyprland_smoke.py --island-network-only`. A private NetworkManager
fixture checks quiet startup, signal changes, short dropouts, sustained loss and
restoration, Wi-Fi/Ethernet handover and original expiry deadlines. GPU checks
cover live artwork, pointer and keyboard actions, the Control Centre network tab,
focus release, interruptions, monitor targeting, fractional scaling, reduced
motion and disabled previews. It uses the same Kitty, Pillow and Tesseract
dependencies and does not change the host network.

For status-card transitions, run
`python3 tests/hyprland_smoke.py --island-card-motion-only`. The private Bluetooth
and NetworkManager fixtures drive device names, late network names and transfer
results while GPU captures sample the outgoing and incoming text opacity. It also
checks rapid replacements, steady metadata and identical completion messages,
live artwork, interrupted fades, expiry, reduced motion, reload and 150% scaling.
The fixture slows animations to make intermediate frames measurable.

For continuous media artwork, run
`python3 tests/hyprland_smoke.py --island-media-motion-only`. Native GPU captures
follow a distinctive cover through expansion, collapse and reversal in both card
sizes, checking that it stays visible and moves through intermediate positions.
The check also covers playback changes, hosted panel return, notification
interruption, reduced motion and fractional scaling on private outputs.

For expanded headers and hover actions, run
`python3 tests/hyprland_smoke.py --island-expanded-only`. The private desktop checks
the in-place section overview, hidden sections, keyboard focus and navigation, both densities
and themes, timed Keep Awake controls, routed microphone mute/unmute, camera app
feedback and fractional panel layout. The camera fixture uses a private fake `/proc`
tree and does not access host camera hardware.

For the desktop sharing indicator, run
`python3 tests/hyprland_smoke.py --island-sharing-only`. Private PipeWire capture
links check idle sources, concurrent captures and removal of the last capture.
GPU checks cover the persistent purple pulse, auto-hide, OSDs, capture changes
inside hosted panels, returning to the Island, filters, reduced motion, live media
artwork and 150% scaling. The video-class test nodes use audio DSP ports to exercise
the privacy classifier without capturing the host desktop; this does not validate
a particular remote desktop application's capture backend.

The capture check (`--island-capture-only`) also samples the two recording startup
pulses, their expiry, the steady recording icon, restoration of a concurrent
sharing glow, reloads, fractional scaling and reduced motion. The privacy and
camera checks verify their coloured icons remain visible without a red outer glow.

For track announcements, run
`python3 tests/hyprland_smoke.py --island-track-only`. A private MPRIS player
queues metadata before playback and reuses browser track IDs and URLs. GPU checks
cover title/artist text, playback and clock ticks, pause/resume, activity priority,
alert interruptions, panel return, artwork, reduced motion and a scaled display.
It uses the same Pillow and Tesseract dependencies as the other text checks.

For the microphone Live Activity, run
`python3 tests/hyprland_smoke.py --island-microphone-only`. Real private PipeWire
sources and a test tone exercise input levels, routed mute/unmute, multiple inputs
from one app, external mute changes and monitor teardown. GPU checks cover the
orange indicator, app identity, keyboard navigation, fractional output scaling,
media artwork and urgent notification priority. Pillow and English Tesseract data
are required. Audio devices and captures are isolated from the host session.

For the shell utilities, run
`python3 tests/hyprland_smoke.py --utilities-only`. This exercises real local OCR
and clipboard delivery, cancelled and empty selections, timed Keep Awake controls,
Focus app exceptions, live privacy details and window activation. It also checks
fractional output scaling and animated media beneath Focus feedback. Tesseract
with English data, Kitty and Pillow are required; `NOCTALIA_TEST_TESSDATA` selects
the test language folder. Unit tests cover timer expiry, schedule boundaries,
overnight days, overlap priority and notification filtering.

Camera Live Activities have a focused GPU check:
`python3 tests/hyprland_smoke.py --island-camera-only`. It checks the green indicator,
direct V4L2 device detection through an isolated process fixture, real PipeWire links,
per-app elapsed time, overlapping streams, keyboard and pointer app focus, scaled
output, media artwork, alerts, independent camera/screen sessions, filters, reload,
reduced motion and the compact camera/microphone/unread indicator cycle. It uses
Kitty, Pillow and English Tesseract data without accessing a real webcam.
Use `--island-privacy-rotation-only` for the compact microphone/camera/screen/unread
cycle, hover hold and consistent bell size on both outputs.

Screen sharing and recording Live Activities have a focused GPU check:
`python3 tests/hyprland_smoke.py --island-capture-only`. It exercises real PipeWire
capture links, overlapping streams, per-app timers, pointer and keyboard app focus,
fractional scaling, artwork, urgent alerts, a shell recording with its explicit Stop
button, saved audio/video, filters, configuration reload and reduced motion.

For the timed Keep Awake Live Activity, run
`python3 tests/hyprland_smoke.py --island-awake-only`. This checks live countdowns,
adding time with a press held across ticks, End, Control Centre synchronization,
indefinite mode, keyboard controls, the split bubble and a scaled output. It also
checks artwork, urgent alerts, reload, reduced motion and a real one-minute expiry.
The test uses Pillow and English Tesseract data in an isolated desktop.

For the screenshot and recording menu, run
`python3 tests/hyprland_smoke.py --island-capture-menu-only`. It checks keyboard
navigation, animated media artwork, a fresh screenshot after delayed selection,
cancellation across countdown ticks, silent/desktop/microphone recordings and
monitor capture at fractional scale with rotation. Configuration reload, locking
and output removal must cancel pending captures. The suite uses private audio sources,
Kitty, Pillow, Tesseract, `wf-recorder` and `ffprobe` without using host inputs.

For Window screenshots and keyboard entry, run
`python3 tests/hyprland_smoke.py --island-window-capture-only`. It checks Shift+Print,
keyboard selection, highlighted click/release, delayed resizing, closed windows,
fractional scale with rotation, cancellation, and the hover Island without a capture
button. `window_capture_test` covers visibility, stacking and malformed IPC geometry.

For temporary recording Focus, run
`python3 tests/hyprland_smoke.py --island-recording-focus-only`. It uses a private
desktop and audio server to check the opt-in toggle, quiet notification history,
urgent alerts, recording and saved-preview visibility, manual Focus/DND restoration,
user overrides, reloads, cancelled selections/countdowns, encoder failure and a
scaled monitor. `focus_state_test`, `screen_recorder_test` and
`config_schema_roundtrip_test` cover state, lifecycle and persistence.

For recording results, run
`python3 tests/hyprland_smoke.py --island-recording-result-only`. It checks a real video
thumbnail, duration and size, Play and keyboard folder actions, animated artwork,
five-second completion glow, fractional scaling, decoder failure and timeout, dismissal
during decoding, and reduced motion. It uses the same private desktop and audio server,
plus `ffmpeg`; file-opening actions are captured by a fixture.

It checks:

- Island surfaces on two outputs, one at 150% scale and 180-degree rotation.
- Noctalia workspace switching between `1` and `Review "B"`.
- Real Lua window focus and raise dispatches, backed by command-generation unit tests.
- A saved screenshot and region recording at 400×300.
- Whole-monitor recording at 1920×1080 on the scaled, rotated output.
- Video duration, dimensions, and an audio stream in both recordings.
- Hyprland portal startup and advertised monitor/window screen-sharing support.

The portal probe checks backend availability; it does not establish a browser
screen-sharing stream. After logging into a real Hyprland session, check browser
source selection and streaming, physical monitor placement, refresh rates/VRR,
and suspend/resume. Virtual outputs cannot validate those hardware paths.

The keyboard assistant can be bound with
`hl.bind("CTRL + SPACE", hl.dsp.exec_cmd("noctalia msg panel-toggle assistant"))`.
Choose another chord if that combination is already used by an input method or app.
The shell does not replace existing compositor bindings automatically.

`python3 tests/hyprland_smoke.py --island-assistant-only` checks the assistant with
an isolated compositor and a loopback streaming AI fixture. It covers keyboard
submission, draft restoration, follow-up context, copy, cancellation and stale
responses, retries, empty/failed responses, long answers, both densities, light
and dark themes, media artwork continuity, and fractional monitor placement.
No external AI service or microphone is used by this check.

For local dictation, enable AI voice in Settings, then optionally bind a second
shortcut (choose keys that are free in your own configuration):

```lua
hl.bind("CTRL + ALT + SPACE", hl.dsp.exec_cmd("noctalia msg assistant-voice start"), {dont_inhibit=true})
hl.bind("CTRL + ALT + SPACE", hl.dsp.exec_cmd("noctalia msg assistant-voice finish"), {release=true, dont_inhibit=true, ignore_mods=true, non_consuming=true})
```

Hold the shortcut while speaking, release it to transcribe, review the text, then
press Enter. Finish only affects a held recording and never opens a closed panel;
releasing after Escape is harmless. Very short taps cancel recording.
Ctrl+D toggles dictation inside the assistant; Ctrl+period cancels and Escape closes.
For a toggle shortcut instead, use `noctalia msg panel-open assistant /dictate`.

`noctalia msg assistant-voice status` reports voice phase, hold state and whether a
draft or answer is present, without exposing their contents.

For a controlled shell resource and real local speech benchmark, set
`NOCTALIA_ASSISTANT_PROFILE_ONLY=1` and `NOCTALIA_PROFILE_ASSISTANT_CONFIG` to a
TOML file containing the local `[shell.launcher.ai]` and voice settings, then run
the same assistant check. The benchmark uses a synthesized sentence on the
private audio server and sends its greeting to the configured loopback Strata
endpoint. `assistant-performance.json` records process memory, CPU, available DRM
engine time, UI cycles, transcription, first text and speech startup. It measures
the renderer selected by the test compositor, which can differ from the desktop.
`NOCTALIA_PROFILE_STRATA_PID` optionally records the model process tree's peak PSS.

`tools/shell_profile.py --process shell=PID --seconds 20 --nvidia --output sample.json`
also samples a running desktop without changing it. CPU 100% means one logical
core. PSS apportions shared memory; RSS counts all resident mappings. NVIDIA
framebuffer allocation is separate from system RAM, and unavailable activity
counters are reported as null rather than zero.


## Dock keyboard and arrangement

Bind the dock independently from the launcher:

```lua
hl.bind("SUPER + ALT + D", hl.dsp.exec_cmd("noctalia msg dock-focus"))
```

The shortcut reveals the enabled dock on the active output without changing its
visibility setting. Arrows along the dock, Tab/Shift+Tab and Home/End select an
item. Enter opens or activates it. Menu or Shift+F10 opens the app menu, where
Up/Down and Enter select actions. Escape closes the menu first, then leaves the
dock. Keyboard focus has a quiet outline that is absent during pointer use.

Hold an app briefly and drag to arrange it. A running app dropped into the pinned
group becomes a pin. Drag a pin away until **Remove from Dock** appears, then
release to remove its shortcut. Returning to the dock or pressing Escape cancels
removal. Unpinning never closes an app or removes its desktop entry.

App names remain available when window previews are enabled. The default preview
delay is 900 ms, giving the name tooltip time to appear first; `preview_delay_ms`
remains configurable. Disable `window_previews` to keep names alone. A smaller
`icon_size` retains the same layout and interaction model for a compact dock.

`python3 tests/hyprland_smoke.py --dock-refinement-only` checks native keyboard
activation, menus, drag pin/unpin/reorder, Escape and return cancellation,
name-before-preview timing, compact/reduced motion and a fractional output.
The existing `--dock-motion-only` and `--dock-preview-only` scenarios cover launch,
auto-hide, magnification, live previews and window identity.

## Launcher refinement checks

`NOCTALIA_TEST_BINARY=build-release/noctalia python3 tests/hyprland_smoke.py --launcher-refinement-only`
checks the native launcher on private GPU-backed outputs: recent items, contextual
actions, filter and Escape restoration, pointer entry/back, long action lists,
Compact/Comfortable spacing, light/dark appearance and fractional display scaling.
Fixtures launch marker commands inside the private test directory.
