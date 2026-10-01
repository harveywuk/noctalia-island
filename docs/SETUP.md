# Setting up Dynamic Noctalia

This guide installs Dynamic Noctalia for one user on Hyprland, alongside any distribution
`noctalia` package, and gets to a working desktop: a Dynamic Island in place of the top bar,
a floating dock with live window previews, and macOS-style lock screen and palette. It also
covers upgrades, rollback and the optional matching login screen.

Everything here installs under your home directory except the optional greeter.

## Tested combination

The fork tracks a specific Hyprland release; newer releases may change the Lua API or plugin
ABI. This combination is the one the integration tests and the reference desktop run.

| Component | Version |
|-----------|---------|
| Base | Noctalia 5.2.0 (upstream `main` at the fork point) |
| OS | Arch Linux / CachyOS, x86_64 |
| Compositor | Hyprland 0.56.2 with a Lua configuration (`hyprland.lua`) |
| Session manager | uwsm 0.27 (optional; plain `Hyprland` works) |
| Test compositor | labwc 0.20 (only for `tests/hyprland_smoke.py`) |

### Hyprland plugins (optional)

Settings shows controls for these plugins only when they are loaded. Noctalia never installs
or loads plugins. Each must be built against your exact Hyprland version. The reference
desktop builds them from these commits and loads them with `hl.plugin.load()`:

| Plugin | Repository | Commit |
|--------|------------|--------|
| Dynamic Cursors | <https://github.com/VirtCode/hypr-dynamic-cursors> | `5a224284` |
| Kinetic scrolling | <https://github.com/savonovv/hypr-kinetic-scroll> | `657a8a7c` |
| Edge hover | <https://github.com/gfhdhytghd/hypr-edgehover> | `77b5e144` |
| Hyprglass | <https://github.com/hyprnux/hyprglass> | `77636c57` |
| Hyprspace | <https://github.com/KZDKM/Hyprspace> | `0799be74` |

`hyprpm add <repository>` builds them too. Pinning these commits avoids ABI breakage after
upstream plugin changes.

## 1. Dependencies

Install the build dependencies from [BUILDING.md](../BUILDING.md#arch). Dynamic Noctalia's
own features add these runtime tools:

| Package | Used for |
|---------|----------|
| `hyprland` | Compositor and every Hyprland settings page |
| `xdg-desktop-portal-hyprland` | Screenshots, screen sharing, file pickers |
| `wf-recorder` | Screen recording |
| `kitty` | Default terminal (change it in Settings → System → Default applications) |
| `polkit` and an agent (e.g. `hyprpolkitagent`) | Privileged actions such as greeter appearance sync |
| `upower` | Batteries, including Bluetooth device batteries in the Island |
| `ddcutil` (optional) | External monitor brightness |
| `labwc`, `grim`, `tesseract`, `tesseract-data-eng`, `python-pillow`, `mpv` (optional) | Running the integration tests |

```sh
sudo pacman -S --needed hyprland xdg-desktop-portal-hyprland wf-recorder kitty \
  polkit hyprpolkitagent upower
```

## 2. Build and install

```sh
git clone <your fork URL> dynamic-noctalia
cd dynamic-noctalia
scripts/install-local.sh
```

The script configures a release build in `build-release/`, runs the unit tests, and installs
to `~/.local/lib/dynamic-noctalia`. It links `~/.local/bin/noctalia` to the installed binary
and writes `~/.config/systemd/user/dynamic-noctalia.service` if that unit does not exist yet.
Options:

| Option | Default |
|--------|---------|
| `--prefix DIR` / `NOCTALIA_PREFIX` | `~/.local/lib/dynamic-noctalia` |
| `--unit NAME` / `NOCTALIA_UNIT` | `dynamic-noctalia.service` |
| `NOCTALIA_BUILD_DIR` | `build-release` |
| `--skip-tests` | run tests |
| `--no-restart` | restart the unit if it is running |

Assets are installed with the binary. Copying only `noctalia` is not enough.

## 3. Start it with Hyprland

Copy the start hook and call it once Hyprland is ready:

```sh
install -Dm755 examples/hyprland/start-shell.sh ~/.local/share/dynamic-noctalia/start-shell.sh
```

```lua
-- hyprland.lua (use your own home directory)
hl.on("hyprland.start", function()
    hl.exec_cmd("/home/you/.local/share/dynamic-noctalia/start-shell.sh")
end)
```

The hook exports the Wayland and Hyprland variables to the systemd user manager, clears
sockets left by other compositors, restarts the shell unit and restarts the portal. The shell
runs outside your login session, as user services do. It finds your graphical session through
logind, so locking before suspend and brightness control work as usual.

Do not also start `noctalia` from `exec-once` or an XDG autostart entry, or two shells will run.

## 4. Try the starter configuration

```sh
mkdir -p ~/.config/noctalia
[ -f ~/.config/noctalia/config.toml ] && cp ~/.config/noctalia/config.toml{,.bak}
cp examples/starter.toml ~/.config/noctalia/config.toml
noctalia config validate
systemctl --user restart dynamic-noctalia.service
```

[examples/starter.toml](../examples/starter.toml) sets up:

- an Island bar presentation instead of the top bar
- a floating, magnifying dock with live window previews
- the bundled macOS palette following light/dark mode

The lock screen and Island use their macOS-style appearance by default. The first-run wizard
then asks for your location, avatar and wallpaper.

Changes made in Settings are written to `~/.local/state/noctalia/settings.toml` and override
`config.toml` without editing it. Settings → System → Backups saves and restores both.

## 5. Optional: matching login screen

The matching login screen is a fork of
[noctalia-greeter](https://github.com/noctalia-dev/noctalia-greeter) 1.5.0, kept in its own
repository on branch `feature/cupertino`. On Arch, build a package that replaces the
distribution `noctalia-greeter` (the PKGBUILD builds from a local checkout at
`~/Projects/noctalia-greeter`; set `NOCTALIA_GREETER_SRC` to use another path):

```sh
cd noctalia-greeter/packaging/arch
makepkg -f
sudo pacman -U noctalia-greeter-cupertino-*.pkg.tar.zst
```

Nothing changes until the next login screen. To roll back from a TTY, reinstall the
distribution package from the pacman cache:

```sh
sudo pacman -U /var/cache/pacman/pkg/noctalia-greeter-<version>-x86_64.pkg.tar.zst
```

To keep the fork but use the original card, set `[appearance].layout = "classic"` in
`/var/lib/noctalia-greeter/greeter.toml` (owned by the greeter user; edit with `sudoedit`).

## Upgrading

```sh
cd dynamic-noctalia
git pull
scripts/install-local.sh
```

The script keeps the previous binary as `bin/noctalia.previous-<timestamp>` (the newest five
are retained) and restarts the running unit. After a Hyprland upgrade, rebuild every plugin
against the new version before logging back in, or Hyprland refuses to load them.

## Rolling back

```sh
prefix=~/.local/lib/dynamic-noctalia
cp -p "$(ls -1t $prefix/bin/noctalia.previous-* | head -1)" $prefix/bin/noctalia
systemctl --user restart dynamic-noctalia.service
```

A `settings.toml` written by a newer build may contain keys an older build reports as
unknown. Take a backup in Settings → System → Backups before upgrading if you might roll back.

## Troubleshooting

| Symptom | Check |
|---------|-------|
| Shell does not start | `journalctl --user -u dynamic-noctalia.service -b`; the unit needs `WAYLAND_DISPLAY`, which the start hook exports |
| Two bars or duplicate notifications | Another `noctalia` is running from autostart or `exec-once` |
| Hyprland settings pages are missing | `HYPRLAND_INSTANCE_SIGNATURE` was not exported; rerun the start hook |
| Plugin settings are missing | The plugin is not loaded: `hyprctl plugin list` |
| Screen does not lock on suspend | Log shows `logind session lock monitor active`; if not, `loginctl` must list a graphical session for your user |
| Window previews show app icons only | The compositor lacks foreign-toplevel image capture (Hyprland 0.56 has it) |
