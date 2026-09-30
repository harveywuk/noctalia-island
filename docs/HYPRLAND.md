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
