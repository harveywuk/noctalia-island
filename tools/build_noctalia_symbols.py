#!/usr/bin/env python3
"""Compile the editable companion SVGs into the bundled Cupertino-based font.

Requires fontTools. Ordinary builds use the checked-in outputs; no network access
or additional runtime dependency is needed.
"""

import argparse
import hashlib
import json
import xml.etree.ElementTree as ET
from pathlib import Path

from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "assets/icons/noctalia"
FONTS = ROOT / "assets/fonts"


def build(output):
    font = TTFont(FONTS / "CupertinoIcons.ttf", recalcTimestamp=False)
    manifest = json.loads((SOURCES / "symbols.json").read_text())
    existing = font.getBestCmap()
    glyph_order = list(font.getGlyphOrder())
    unit = font["head"].unitsPerEm / 24
    metadata = {}
    source_hash = hashlib.sha256((SOURCES / "symbols.json").read_bytes())
    for name, entry in sorted(manifest.items()):
        cp = int(entry["codepoint"][2:], 16)
        if not 0xF000 <= cp <= 0xF0FF or cp in existing:
            raise ValueError(f"{name}: codepoint is occupied or outside the companion range")
        svg = (SOURCES / f"{name}.svg").read_bytes()
        source_hash.update(svg)
        root = ET.fromstring(svg)
        if root.attrib.get("viewBox") != "0 0 24 24":
            raise ValueError(f"{name}: expected a 24-unit design grid")
        pen = TTGlyphPen(None)
        # The originals centre their 512-unit em on y=192. Keep that baseline;
        # Glyph's optical centring still handles each symbol's individual bounds.
        transformed = TransformPen(Cu2QuPen(pen, max_err=.25), (unit, 0, 0, -unit, 0, 21 * unit))
        paths = 0
        for element in root:
            kind = element.tag.rsplit("}", 1)[-1]
            if kind == "title":
                continue
            if kind != "path" or any(key in element.attrib for key in ("stroke", "transform")):
                raise ValueError(f"{name}: use flattened, filled vector paths")
            if element.attrib.get("fill-rule", "nonzero") != "nonzero":
                raise ValueError(f"{name}: paths must use nonzero winding")
            parse_path(element.attrib["d"], transformed)
            paths += 1
        if not paths:
            raise ValueError(f"{name}: no paths")
        glyph_name = "noctalia_" + name.replace("-", "_")
        glyph = pen.glyph()
        glyph.recalcBounds(font["glyf"])
        if glyph.xMax <= glyph.xMin or glyph.yMax <= glyph.yMin:
            raise ValueError(f"{name}: empty symbol")
        font["glyf"][glyph_name] = glyph
        glyph_order.append(glyph_name)
        font["hmtx"].metrics[glyph_name] = (font["head"].unitsPerEm, glyph.xMin)
        for table in font["cmap"].tables:
            if table.isUnicode():
                table.cmap[cp] = glyph_name
        for alias in [name, *entry["aliases"]]:
            if alias in metadata:
                raise ValueError(f"Duplicate symbol name: {alias}")
            metadata[alias] = entry["codepoint"]
        existing[cp] = glyph_name
    font.setGlyphOrder(glyph_order)
    for name_id, value in {
        1: "Noctalia Cupertino", 2: "Regular", 3: "Noctalia Cupertino 1.0",
        4: "Noctalia Cupertino", 6: "NoctaliaCupertino-Regular",
    }.items():
        for record in font["name"].names:
            if record.nameID == name_id:
                record.string = value.encode(record.getEncoding())
    output.mkdir(parents=True, exist_ok=True)
    font.save(output / "noctalia-cupertino.ttf")
    (output / "noctalia-symbols.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
    (output / "noctalia-symbols-source.json").write_text(json.dumps({
        "base_font_sha256": hashlib.sha256((FONTS / "CupertinoIcons.ttf").read_bytes()).hexdigest(),
        "companion_sources_sha256": source_hash.hexdigest(),
        "generated_font_sha256": hashlib.sha256((output / "noctalia-cupertino.ttf").read_bytes()).hexdigest(),
        "symbol_count": len(manifest),
    }, indent=2) + "\n")
    print(f"Built {len(manifest)} companion symbols ({len(metadata)} names) alongside Cupertino")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=FONTS)
    build(parser.parse_args().output)
