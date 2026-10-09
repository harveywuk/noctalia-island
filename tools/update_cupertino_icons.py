#!/usr/bin/env python3
"""Refresh the pinned Cupertino font, name map and licences. No Flutter runtime needed."""

import hashlib
import json
import re
import urllib.request
from pathlib import Path

from build_noctalia_symbols import build

PACKAGES_REV = "d8d9e97d2d02bf785084e7dc9409a79a49e75967"
FLUTTER_REV = "01d3eecf3e5de0eb6cdf6a28a71d4139842c0179"
ROOT = Path(__file__).resolve().parents[1] / "assets/fonts"
PACKAGE = f"https://raw.githubusercontent.com/flutter/packages/{PACKAGES_REV}/third_party/packages/cupertino_icons"
FLUTTER = f"https://raw.githubusercontent.com/flutter/flutter/{FLUTTER_REV}"


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as response:
        return response.read()


def main():
    font = fetch(f"{PACKAGE}/assets/CupertinoIcons.ttf")
    source = fetch(f"{FLUTTER}/packages/flutter/lib/src/cupertino/icons.dart")
    icons = {
        name: f"U+{int(cp, 16):04X}"
        for name, cp in re.findall(
            r"static const IconData (\w+)\s*=\s*IconData\(\s*(0x[0-9a-fA-F]+)",
            source.decode(),
        )
    }
    if not icons or any(not 0xF000 <= int(cp[2:], 16) <= 0xFFFF for cp in icons.values()):
        raise ValueError("Cupertino codepoints no longer fit the reserved glyph range")
    # Fetch everything before replacing any bundled files.
    licence = fetch(f"{PACKAGE}/LICENSE")
    map_licence = fetch(f"{FLUTTER}/LICENSE")
    (ROOT / "CupertinoIcons.ttf").write_bytes(font)
    (ROOT / "cupertino-icons-license.txt").write_bytes(licence)
    (ROOT / "cupertino-map-license.txt").write_bytes(map_licence)
    (ROOT / "cupertino.json").write_text(json.dumps(icons, indent=2, sort_keys=True) + "\n")
    (ROOT / "cupertino-source.json").write_text(json.dumps({
        "package_revision": PACKAGES_REV,
        "map_revision": FLUTTER_REV,
        "font_url": f"{PACKAGE}/assets/CupertinoIcons.ttf",
        "map_url": f"{FLUTTER}/packages/flutter/lib/src/cupertino/icons.dart",
        "font_sha256": hashlib.sha256(font).hexdigest(),
        "map_source_sha256": hashlib.sha256(source).hexdigest(),
    }, indent=2) + "\n")
    print(f"Bundled Cupertino font and {len(icons)} icon names")
    build(ROOT)


if __name__ == "__main__":
    main()
