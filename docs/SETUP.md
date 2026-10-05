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
| `polkit` | Privileged actions; the starter configuration enables Noctalia's integrated agent |
| `adw-gtk-theme` | GTK 3 light and dark base themes for generated colours |
| `qt6ct`, `kvantum` | Qt settings and the KvMojave base used by the generated Kvantum theme |
| `upower` | Batteries, including Bluetooth device batteries in the Island |
| `ddcutil` (optional) | External monitor brightness |
| `labwc`, `grim`, `tesseract`, `tesseract-data-eng`, `python-pillow`, `mpv` (optional) | Running the integration tests |

```sh
sudo pacman -S --needed hyprland xdg-desktop-portal-hyprland wf-recorder kitty \
  polkit upower adw-gtk-theme qt6ct kvantum
```

## 2. Build and install

```sh
git clone --branch feature/orbit-island https://github.com/harveywuk/noctalia-island.git dynamic-noctalia
cd dynamic-noctalia
scripts/install-local.sh
```

The script configures a release build in `build-release/`, enables and runs the unit tests
even when reusing an existing build, and installs
to `~/.local/lib/dynamic-noctalia`. It links `~/.local/bin/noctalia` to the installed binary
and writes `~/.config/systemd/user/dynamic-noctalia.service` if that unit does not exist yet.
It also installs the start hook under the XDG data directory when none exists. Existing units
and hooks are preserved. The binary and assets are staged together before replacing the
installation; a failed build, test or staging step leaves the installed tree untouched.
Options:

| Option | Default |
|--------|---------|
| `--prefix DIR` / `NOCTALIA_PREFIX` | `~/.local/lib/dynamic-noctalia` |
| `--unit NAME` / `NOCTALIA_UNIT` | `dynamic-noctalia.service` |
| `NOCTALIA_BUILD_DIR` | `build-release` |
| `--jobs N` / `NOCTALIA_BUILD_JOBS` | `6` build/test jobs |
| `--skip-tests` | run tests |
| `--no-restart` | restart the unit if it is running |

Assets are installed with the binary. Copying only `noctalia` is not enough.

## 3. Start it with Hyprland

The installer supplies the start hook. For an existing installation, inspect and back up the
old hook before updating it. Keep its custom `NOCTALIA_UNIT` default if applicable:

```sh
cp ~/.local/share/dynamic-noctalia/start-shell.sh ~/.local/share/dynamic-noctalia/start-shell.sh.backup
install -m755 examples/hyprland/start-shell.sh ~/.local/share/dynamic-noctalia/start-shell.sh
```

```lua
-- hyprland.lua (use your own home directory)
hl.on("hyprland.start", function()
    hl.exec_cmd("/home/you/.local/share/dynamic-noctalia/start-shell.sh")
end)
```

Under UWSM, the hook leaves environment import, session ordering and portal activation to
the session manager. With plain Hyprland, it imports the required display variables itself.
It queues an idempotent shell start without blocking session activation. It does not restart
portals or start a second authentication agent. A custom unit can be selected with
`NOCTALIA_UNIT`; the installer embeds its selected unit as the new hook's default.

Noctalia runs as a user service and finds the graphical session through logind for locking
before suspend and brightness control.

Do not also start `noctalia` from `exec-once` or an XDG autostart entry, or two shells will run.

### UWSM environment and application shortcuts

Install the example environment fragments:

```sh
install -Dm644 examples/uwsm/env ~/.config/uwsm/noctalia-env
install -Dm644 examples/uwsm/env-hyprland ~/.config/uwsm/noctalia-env-hyprland
```

Add this line once to `~/.config/uwsm/env`, preserving its existing settings:

```sh
. "${XDG_CONFIG_HOME:-$HOME/.config}/uwsm/noctalia-env"
```

Add this line once to `~/.config/uwsm/env-hyprland`:

```sh
. "${XDG_CONFIG_HOME:-$HOME/.config}/uwsm/noctalia-env-hyprland"
```

The fragments select qt6ct and Qt's Wayland/X11 backend order. Optional cursor settings are
commented until that theme is installed. They take effect at the next login. Keep these
variables in UWSM's files instead of duplicating them across `environment.d`, compositor
configuration and service overrides. Launch application shortcuts through `uwsm app --`, for
example `uwsm app -- kitty`, and use `uwsm stop` for session logout. See
[Hyprland's UWSM guidance](https://wiki.hypr.land/useful-utilities/uwsm/).

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
- a dark shell while application colours follow light/dark mode
- GTK 3/4, Qt and Kvantum templates, with the dependencies above
- Noctalia's integrated authentication agent

Run only one authentication agent. If `hyprpolkitagent.service` is enabled, stop and disable
it before starting this configuration:

```sh
systemctl --user disable --now hyprpolkitagent.service
```

The lock screen and Island use their macOS-style appearance by default. The first-run wizard
then asks for your location, avatar and wallpaper.

Changes made in Settings are written to `~/.local/state/noctalia/settings.toml` and override
`config.toml` without editing it. Settings → System → Backups saves and restores both.

### Optional matching icons, cursor and sounds

The starter uses installed system fonts and icons. SF Pro is a personal installation and is
not bundled. The matching cursor has its own reproducible build instructions in
[CURSOR_THEME.md](CURSOR_THEME.md); enable the cursor lines in the UWSM fragments after
installing it.

For the optional [WhiteSur icon theme](https://github.com/vinceliuice/WhiteSur-icon-theme),
install both `WhiteSur-light` and `WhiteSur-dark`, then install the repository's mode hook:

```sh
install -Dm755 scripts/sync-icon-theme.py ~/.local/bin/noctalia-icon-theme
```

Add it to `[hooks]` in the Noctalia configuration, incorporating it into any existing
`theme_mode_changed` command rather than replacing that command:

```toml
[hooks]
theme_mode_changed = '"$HOME/.local/bin/noctalia-icon-theme"'
```

The hook checks that the chosen theme exists, updates GTK and existing Qt settings, and
preserves settings-file symlinks. `--light NAME --dark NAME` selects another installed pair.

Generate the original sound theme with NumPy and ffmpeg installed:

```sh
python3 scripts/make-cupertino-sounds.py
```

Then select `cupertino` in Sound settings, or set `[audio] sound_theme = "cupertino"`.
No Apple sound recordings are included. Open GTK 3 and Qt/Kvantum applications may need
reopening after a colour-mode change; see the [GTK/Qt guide](user/templates/official/gtk-qt.mdx).

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

The script keeps the previous binary and assets together in a sibling directory named
`dynamic-noctalia.previous-<timestamp>.<suffix>` and prints its exact path. Backups are retained
until you remove them. It restarts the running unit unless `--no-restart` is set.
After a Hyprland upgrade, rebuild every plugin
against the new version before logging back in, or Hyprland refuses to load them.

## Rolling back

```sh
(
set -e
install_prefix=~/.local/lib/dynamic-noctalia
previous_install=/path/printed/by/the/installer
test -x "$previous_install/bin/noctalia" && test -d "$previous_install/share/noctalia/assets"
systemctl --user stop dynamic-noctalia.service
mv "$install_prefix" "$install_prefix.before-rollback-$(date +%Y%m%d-%H%M%S)"
cp -a "$previous_install" "$install_prefix"
systemctl --user start dynamic-noctalia.service
)
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
