#!/usr/bin/env python3
"""Exercise apply/undo against the spaced INI files written by desktop theme hooks."""
import configparser
import os
from pathlib import Path
import subprocess
import sys
import tempfile


scripts = Path(sys.argv[1]).resolve()


def read(path):
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    parser.read(path)
    return parser


with tempfile.TemporaryDirectory(prefix="noctalia-kvantum-test-") as folder:
    root = Path(folder)
    config = root / "config"
    base = root / "data/Kvantum/KvMojave"
    base.mkdir(parents=True)
    (base / "KvMojave.kvconfig").write_text("[General]\nauthor=fixture\n")
    (base / "KvMojave.svg").write_text('<svg xmlns="http://www.w3.org/2000/svg"/>\n')
    env = dict(os.environ, XDG_CONFIG_HOME=str(config), XDG_DATA_DIRS=str(root / "data"))

    def prepare():
        theme = config / "Kvantum/Noctalia"
        theme.mkdir(parents=True, exist_ok=True)
        (theme / "overrides.kvconfig").write_text(
            "[%Noctalia]\naccent=#0072e3\naccent_dark=#0067d9\n"
            "accent_light=#77aff1\nwindow=#242426\n[General]\nroundness=6\n"
        )

    def run(name):
        subprocess.run(["bash", str(scripts / name)], env=env, check=True,
                       stdout=subprocess.DEVNULL)

    prepare()
    kv = config / "Kvantum/kvantum.kvconfig"
    kv.write_text("[General]\ntheme = PreviousTheme\nkeep = yes\n")
    qt = config / "qt6ct/qt6ct.conf"
    qt.parent.mkdir()
    target = root / "qt-settings.conf"
    original = "[Appearance]\n  style = Fusion  \nicon_theme = WhiteSur-dark\n[Fonts]\ngeneral = SF Pro\n"
    target.write_text(original)
    qt.symlink_to(target)

    run("apply.sh")
    assert qt.is_symlink(), "apply must preserve config symlinks"
    assert read(qt)["Appearance"]["style"] == "kvantum"
    assert read(qt)["Appearance"]["icon_theme"] == "WhiteSur-dark"
    assert read(qt)["Fonts"]["general"] == "SF Pro"
    assert read(kv)["General"]["keep"] == "yes"
    assert not (config / "qt5ct/qt5ct.conf").exists(), "do not create unused Qt config"
    before = target.read_bytes(), target.stat().st_mtime_ns
    run("apply.sh")
    assert (target.read_bytes(), target.stat().st_mtime_ns) == before, "repeat apply must be a no-op"

    # Python's strict parser is also what the desktop's icon-switching hook uses.
    target.write_text(target.read_text().replace("style=kvantum", "style = kvantum\nstyle=kvantum"))
    run("apply.sh")
    assert read(qt)["Appearance"]["style"] == "kvantum", "repair existing duplicate keys"
    # Another writer can add whitespace between apply and undo.
    target.write_text(target.read_text().replace("style=kvantum", " style = kvantum "))
    kv.write_text(kv.read_text().replace("theme=Noctalia", "theme = Noctalia"))
    run("undo.sh")
    assert qt.is_symlink()
    assert read(qt)["Appearance"]["style"] == "Fusion", "restore the original spaced value"
    assert read(kv)["General"]["theme"] == "PreviousTheme"
    assert read(kv)["General"]["keep"] == "yes"

    prepare()
    target.write_text("[Appearance]\nicon_theme = WhiteSur-dark\n")
    run("apply.sh")
    run("undo.sh")
    assert "style" not in read(qt)["Appearance"], "remove style when none existed before"

    prepare()
    target.write_text(original)
    run("apply.sh")
    target.write_text(target.read_text().replace("style=kvantum", "style=kvantum\nstyle = Windows"))
    edited = target.read_bytes()
    run("undo.sh")
    assert target.read_bytes() == edited, "undo must preserve a later user choice"

print("Kvantum config apply/undo passed")
