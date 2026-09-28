# Bibata following Noctalia colours

`scripts/theme-bibata.py` builds a separate cursor theme from the installed
`Bibata-Material-Teal` SVG assets. It preserves the original theme, animation
metadata, and aliases. The recolouring maps teal outlines to `primary`, dark
interiors to `on_primary`, and shadow details to `shadow`. Other semantic colours
(such as the prohibited-action mark) are preserved.

The generator produces SVG Hyprcursor archives and Xcursor files at 24, 32, 48,
64, and 96 pixels. Variants are cached by palette under `~/.local/share/icons`.
Changing the selected theme name refreshes compositor and GTK cursor caches.
Some applications that cache their own cursors may require reopening.

Dependencies: Python, PyGObject, Pycairo, librsvg, GSettings, and Umbriel.
No internet access is needed to generate a variant.

## This installation

- Generator: `~/.local/share/noctalia-cursor-theme/theme-bibata.py`
- Template: `~/.config/noctalia/templates/bibata-cursor.json`
- Rendered colours: `~/.cache/noctalia/cursor-palette.json`
- Hook: `[theme.templates.user.bibata_cursor]` in Noctalia's config
- Original settings: `~/.local/share/noctalia-cursor-theme/backup-20260928-080434`

Noctalia renders the palette template and runs the generator after theme changes.
It selects the generated variant in Umbriel, GTK3/4, GSettings and the default
Xcursor theme. Cursor size is left unchanged. A stable alias is exported through
the user service environment and `environment.d`, and in Noctalia's `cursor.conf`
service override so subsequently launched applications can find it.

For a generation-only check, without changing desktop settings:

```sh
python3 scripts/theme-bibata.py \
  --palette ~/.cache/noctalia/cursor-palette.json --build-only
```

To revert, remove `[theme.templates.user.bibata_cursor]` from Noctalia's config,
restore the backed-up cursor settings, select `Bibata-Material-Teal` in GSettings,
and remove the generated cursor `environment.d` and service override entries.
Reload the configuration and user service manager. The original cursor assets
remain untouched.

Validation: all 88 Xcursor shapes and aliases load with libXcursor; their decoded
hotspots are within bounds. All generated Hyprcursor archives pass ZIP validation.
The pointer SVG was visually checked, and Noctalia's template application was
used to activate the current palette.
