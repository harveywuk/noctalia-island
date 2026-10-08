# Hyprland integration checks

Dynamic Noctalia supports Hyprland's Lua dispatch interface and the older
Hyprlang interface. Lua selectors are escaped before dispatch, including named
workspaces containing quotes, backslashes, or control characters. Window focus
and raising use the same interface as the running compositor.

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
