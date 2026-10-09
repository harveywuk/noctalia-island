# Shell icon fonts

Shared shell names use Cupertino Icons and the Noctalia companion symbols. The
companion set fills the missing battery, device, sharing, network and system states
with matching vector drawings. Tabler remains available for brand identities and
existing custom/plugin icons. App and tray artwork supplied by applications is
unaffected.

`CupertinoIcons.ttf` is bundled unmodified from Flutter's `cupertino_icons` package
under the [MIT licence](cupertino-icons-license.txt). The generated `cupertino.json`
name map comes from Flutter's Cupertino icon declarations, with the corresponding
[BSD licence](cupertino-map-license.txt). Pinned source URLs, revisions and SHA-256
hashes are recorded in [cupertino-source.json](cupertino-source.json). This font is
Cupertino Icons, not Apple's SF Symbols. There is no Flutter runtime dependency.

The shell renders `noctalia-cupertino.ttf`, a generated font containing all original
Cupertino glyphs plus the [editable companion SVGs](../icons/noctalia/README.md).
Their SF-style proportions and rounded geometry follow the existing design
reference. No Apple font or SF Symbols export is bundled. Companion artwork uses
the project's MIT licence; variants derived from Cupertino retain its MIT notice.
`noctalia-symbols.json` records the companion names, and
`noctalia-symbols-source.json` records the source and generated-font hashes.

To refresh the pinned assets, run `python3 tools/update_cupertino_icons.py` from the
repository root with Python's `fontTools` package available. This maintenance step
downloads upstream files and rebuilds the companion font. Ordinary builds
and the installed shell use the checked-in assets without accessing the network.
Meson's asset installation and portable bundles include the fonts, SVGs and licences.

The registry accepts shared names such as `media-play`, explicit names such as
`cupertino:play_fill`, `noctalia:device-airpods` and `tabler:player-play-filled`, and existing `U+...`/`0x...`
codepoint literals. Original Tabler codepoints stay unchanged. Cupertino glyph IDs
use U+F0000 through U+F0FFF internally so both fonts can coexist in scene nodes and
texture caches. The renderer converts those IDs back to the original font's
codepoints. New custom icons should use names instead of hard-coded numeric IDs.

`glyph_registry_test` checks both runtime fonts, every Cupertino and companion
name, shared aliases, filled variants, fractional-scale metrics, visibly increasing
battery fill and saved Tabler codepoint compatibility.
