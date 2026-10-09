# Noctalia companion symbols

These vector symbols fill the gaps in the shell's Cupertino set. They follow the
SF-style design direction used by the Island: rounded geometry, restrained detail,
balanced outlines and clear filled states. Apple's [SF Symbols design guidance](https://developer.apple.com/design/human-interface-guidelines/sf-symbols)
is the visual reference; the drawings and the bundled open-source Cupertino font
are the actual assets used by the shell.

The source of truth is the SVG files and `symbols.json`. Each SVG uses a 24-unit
grid, filled contours with nonzero winding and `currentColor`. Rounded strokes are
expanded into paths so SVG and font rendering agree. The shell applies its normal
tint, optical sizing and hit targets.

Battery bodies and variants of existing camera, Bluetooth, cloud, headphones,
music, sun, Wi-Fi, calendar, globe, keyboard, photo, desktop, clipboard, link and
paintbrush symbols derive from the bundled MIT-licensed Cupertino font
to preserve their silhouettes. Other companion drawings use the project's
[MIT licence](LICENSE). Keep the [Cupertino licence](../../fonts/cupertino-icons-license.txt)
with any redistribution of the derived artwork. No SF Symbols font data or SVG
exports were used.

The battery family keeps five distinct charge steps, a charging bolt and a warning
mark. Off states use a diagonal slash with a clear gap through the underlying
symbol. Device identities stay consistent between Bluetooth, Batteries and the
Island. Signal levels remain distinct, including the disconnected state.
Settings uses a horizontal Island capsule and a top-bar silhouette; utility badges
keep a clear gap between the action mark and its underlying symbol.

Run `python3 tools/build_noctalia_symbols.py` from the repository root after editing
these files (requires `fontTools`). It rebuilds `assets/fonts/noctalia-cupertino.ttf`
and its companion metadata without network access. The checked-in font is used for
ordinary builds and installs. The generation is deterministic and preserves the
original Cupertino glyph outlines, metrics and codepoints.

Keep existing manifest codepoints stable. The F000–F0FF range is reserved for this
set; the builder rejects collisions with upstream glyphs. New symbols may list
compatible shell names in `aliases`. Use the `noctalia:` prefix to select a
companion explicitly, or a shared name such as `bluetooth-device-mouse` to follow
the shell's semantic aliases. Both are available through the glyph picker.

Review at 16, 24 and 36 pixels, then on native Compact and Comfortable cards,
including fractional display scale. `glyph_registry_test` checks rendered coverage
and battery progression; the Island smoke suite captures actual connected-device
and battery states in both themes.
