# Apple cursors as Xcursors and Hyprcursors

`scripts/build-apple-hyprcursor.py` installs [ful1e5/apple_cursor](https://github.com/ful1e5/apple_cursor)
as one theme that holds both formats:

- `cursors/`: the project's release Xcursors (v2.0.1, 16 to 96 px), unchanged.
- `hyprcursors/` and `manifest.hl`: scalable SVG Hyprcursors generated from the project's SVG
  sources. Hotspots, aliases and frame delays come from its `configs/x.build.toml`, and colours
  are replaced as its `render.json` does, so both formats draw the same cursor.

Hyprland draws the SVGs at any size. Clients that load Xcursors directly get the release images.
The `wait` and `progress` cursors are 40-frame animations at 20 ms per frame.

The cursors keep Apple's colours and do not follow the Noctalia palette.

Dependencies: Python 3.11 or newer (for `tomllib`). Hyprland reads the result through
libhyprcursor.

## Building

```sh
git clone --depth 1 https://github.com/ful1e5/apple_cursor
gh release download v2.0.1 -R ful1e5/apple_cursor -p macOS.tar.xz && tar -xJf macOS.tar.xz
python3 scripts/build-apple-hyprcursor.py --source apple_cursor --release .
```

This writes `~/.local/share/icons/macOS`. Pass `--variant macOS-White` for the white cursors, and
`--icons-dir` to install somewhere else.

## Selecting the theme

Set `macOS` everywhere a cursor theme is named:

- `XCURSOR_THEME` and `HYPRCURSOR_THEME` in the compositor (`hl.env` in Hyprland), in
  `environment.d`, in the user service environment, and in any Noctalia service override
- `org.gnome.desktop.interface cursor-theme`, and `gtk-cursor-theme-name` for GTK 3 and 4
- `Inherits` in `~/.icons/default/index.theme`, the Xcursor fallback
- `[input.cursor] theme` in Umbriel's config

Then run `hyprctl setcursor macOS <size>`. Applications that are already open keep the cursor
they loaded until they are reopened. A remote desktop window shows the remote machine's cursors.

Validation: libhyprcursor loads the theme without warnings. All 34 cursor shape protocol names
(`default`, `pointer`, `text`, `wait`, `progress`, the resize shapes, and so on) resolve to a shape
or an alias in both formats. The rendered shapes were checked visually.
