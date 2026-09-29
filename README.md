# Dynamic Noctalia

An independent Noctalia fork with a dynamic island interface.

Dynamic Noctalia brings media, notifications, downloads, timers and desktop controls
into an adaptive capsule at the top of your screen. It is built on
[Noctalia](https://github.com/noctalia-dev/noctalia), the native C++/Wayland desktop
shell, and currently follows the Noctalia **5.2.0** base.

This is an independently maintained fork, not an official Noctalia release.

## The dynamic island

- Compact media activity with artwork and a five-band audio visualiser that reacts
  to desktop audio. Hover to reveal track information, seeking and playback controls.
- Expandable notifications, an unread indicator and access to notification history.
- Download progress, timer controls, battery information and privacy indicators.
- Calendar, launcher and control-centre panels that open through the Island.
- Screenshot annotation and screen recording, with a recording timer you can click
  to stop and save.

The underlying Noctalia shell still provides its bar, dock, settings, wallpaper,
lock screen, tray, clipboard and plugin support. The Island is optional.

See the [Island guide](docs/ORBIT_ISLAND.md) for configuration and behaviour, and
[the capture guide](docs/CAPTURE.md) for recording and annotation tools.

## Build and run

Follow [BUILDING.md](BUILDING.md) to install dependencies and build this checkout.
Upstream packages install upstream Noctalia; they do not include this fork's Island.

The executable and configuration paths retain the `noctalia` name for compatibility.
Enable the Island in your Noctalia configuration:

```toml
[island]
enabled = true
```

The [example Island profile](examples/orbit-island.toml) also disables the regular
bar and dock. Review it before applying it, as it contains other desktop preferences.

General shell configuration is described in [example.toml](example.toml),
[the bundled user documentation](docs/user/) and the
[upstream documentation](https://docs.noctalia.dev/noctalia/).
Fork-specific options are documented in the Island guide.

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for the inherited architecture, code style
and build checks. Keep contributions focused and include relevant validation.

Report fork-specific problems in
[this fork's issue tracker](https://github.com/harveywuk/noctalia-island/issues).
Include the build version, compositor, reproduction steps and relevant logs.

## Credits and licence

Dynamic Noctalia builds on the work of the
[Noctalia developers and contributors](https://github.com/noctalia-dev/noctalia/graphs/contributors).
The original shell, artwork and dependencies retain their credits in
[CREDITS.md](CREDITS.md). The Island's original design draws on the local Orbit
Quickshell implementation described in its guide.

MIT licensed; see [LICENSE](LICENSE). Upstream copyright notices are preserved.
