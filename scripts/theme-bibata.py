#!/usr/bin/env python3
"""Build Bibata cursor variants from Noctalia's rendered palette, then select them.

Requires Python GObject, Cairo, librsvg, and the original SVG Bibata theme.
The source theme is never modified. --build-only does not touch desktop settings.
"""
import argparse
import configparser
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

import cairo
import gi

gi.require_version('Rsvg', '2.0')
gi.require_foreign('cairo')
from gi.repository import Rsvg

SIZES = (24, 32, 48, 64, 96)
VERSION = 1


def recolor(svg, colors):
    mapping = {'#008080': colors['accent'], '#002020': colors['interior'],
               '#001010': colors['shadow']}
    return re.sub(r'#[0-9a-fA-F]{6}', lambda m: mapping.get(m[0].lower(), m[0]), svg)


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == content:
        return
    with tempfile.NamedTemporaryFile('w', dir=path.parent, delete=False) as f:
        f.write(content)
        tmp = Path(f.name)
    tmp.chmod(path.stat().st_mode & 0o777 if path.exists() else 0o644)
    tmp.replace(path)


def build_xcursor(directory, output):
    frames = json.loads((directory / 'metadata.json').read_text())
    chunks = []
    for frame in frames:
        handle = Rsvg.Handle.new_from_data((directory / frame['filename']).read_bytes())
        for size in SIZES:
            surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, size, size)
            viewport = Rsvg.Rectangle()
            viewport.x = viewport.y = 0
            viewport.width = viewport.height = size
            handle.render_document(cairo.Context(surface), viewport)
            surface.flush()
            # Cairo ARGB32 is native-endian and premultiplied, like Xcursor pixels.
            pixels = bytes(surface.get_data())
            if sys.byteorder != 'little':
                import array
                words = array.array('I', pixels)
                words.byteswap()
                pixels = words.tobytes()
            nominal = frame['nominal_size']
            hx = min(size - 1, round(frame['hotspot_x'] * size / nominal))
            hy = min(size - 1, round(frame['hotspot_y'] * size / nominal))
            header = struct.pack('<9I', 36, 0xfffd0002, size, 1,
                                 size, size, hx, hy, frame.get('delay', 0))
            chunks.append((size, header + pixels))
    # Preserve animation order within every nominal size.
    chunks.sort(key=lambda item: item[0])
    offset = 16 + 12 * len(chunks)
    toc = bytearray()
    for size, data in chunks:
        toc.extend(struct.pack('<3I', 0xfffd0002, size, offset))
        offset += len(data)
    output.write_bytes(struct.pack('<4I', 0x72756358, 16, 0x10000, len(chunks))
                       + toc + b''.join(data for _, data in chunks))


def build(source, icons, colors):
    digest = hashlib.sha256(json.dumps([VERSION, colors], sort_keys=True).encode()).hexdigest()[:12]
    name = 'Bibata-Material-Noctalia-' + digest
    destination = icons / name
    if (destination / 'palette.json').exists():
        return name
    icons.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.bibata-build-', dir=icons) as tmp:
        stage = Path(tmp) / name
        stage.mkdir()
        scalable = stage / 'cursors_scalable'
        shutil.copytree(source / 'cursors_scalable', scalable, symlinks=True)
        for svg in scalable.rglob('*.svg'):
            if not svg.is_symlink():
                svg.write_text(recolor(svg.read_text(), colors))
        archives = stage / 'hyprcursors'
        archives.mkdir()
        for archive in (source / 'hyprcursors').iterdir():
            if archive.is_symlink():
                (archives / archive.name).symlink_to(os.readlink(archive))
                continue
            with zipfile.ZipFile(archive) as original, zipfile.ZipFile(
                    archives / archive.name, 'w', zipfile.ZIP_DEFLATED) as generated:
                for entry in original.infolist():
                    data = original.read(entry.filename)
                    if entry.filename.endswith('.svg'):
                        data = recolor(data.decode(), colors).encode()
                    generated.writestr(entry, data)
        cursors = stage / 'cursors'
        cursors.mkdir()
        for original in (source / 'cursors').iterdir():
            if original.is_symlink():
                (cursors / original.name).symlink_to(os.readlink(original))
            else:
                build_xcursor(scalable / original.name, cursors / original.name)
        (stage / 'index.theme').write_text(
            f'[Icon Theme]\nName={name}\nComment=Bibata matching Noctalia\nInherits=Adwaita\n')
        (stage / 'manifest.hl').write_text(
            f'name = {name}\ndescription = Theme-coloured Bibata Material\n'
            'version = 1\ncursors_directory = hyprcursors\n')
        (stage / 'palette.json').write_text(json.dumps(colors, indent=2) + '\n')
        stage.rename(destination)
    return name


def set_ini(path, section, values):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config.read(path)
    if not config.has_section(section):
        config.add_section(section)
    for key, value in values.items():
        config[section][key] = value
    import io
    stream = io.StringIO()
    config.write(stream)
    atomic_write(path, stream.getvalue())


def activate(name, icons, config_home, home):
    # A stable alias is used by environment variables; GSettings uses the unique
    # name to invalidate client caches whenever the palette changes.
    alias = icons / 'Bibata-Material-Noctalia'
    temp_alias = icons / '.Bibata-Material-Noctalia.new'
    temp_alias.unlink(missing_ok=True)
    temp_alias.symlink_to(name)
    temp_alias.replace(alias)
    for base in (home / '.icons', icons):
        set_ini(base / 'default/index.theme', 'Icon Theme', {'Inherits': name})
    for gtk in ('gtk-3.0', 'gtk-4.0'):
        set_ini(config_home / gtk / 'settings.ini', 'Settings', {'gtk-cursor-theme-name': name})
    compositor_config = config_home / 'umbriel/config.toml'
    text = compositor_config.read_text()
    pattern = r'(?ms)(^\[input\.cursor\]\s*\n)(.*?)(?=^\[|\Z)'
    def update_section(match):
        body = match[2]
        replacement = f'theme = "{name}"'
        if re.search(r'(?m)^theme\s*=', body):
            body = re.sub(r'(?m)^theme\s*=.*$', replacement, body)
        else:
            body = replacement + '\n' + body
        return match[1] + body
    updated, count = re.subn(pattern, update_section, text)
    if count != 1:
        raise RuntimeError('Expected one [input.cursor] section in Umbriel configuration')
    # Validate the complete config before publishing it.
    with tempfile.NamedTemporaryFile('w', suffix='.toml', dir=compositor_config.parent) as tmp:
        tmp.write(updated)
        tmp.flush()
        subprocess.run(['/usr/local/bin/umbriel', 'validate', '-c', tmp.name], check=True,
                       stdout=subprocess.DEVNULL)
    atomic_write(compositor_config, updated)
    atomic_write(config_home / 'environment.d/90-noctalia-cursor.conf',
                 'XCURSOR_THEME=Bibata-Material-Noctalia\nHYPRCURSOR_THEME=Bibata-Material-Noctalia\n')
    subprocess.run(['gsettings', 'set', 'org.gnome.desktop.interface', 'cursor-theme', name], check=True)
    subprocess.run(['systemctl', '--user', 'set-environment',
                    'XCURSOR_THEME=Bibata-Material-Noctalia',
                    'HYPRCURSOR_THEME=Bibata-Material-Noctalia'], check=True)
    subprocess.run(['/usr/local/bin/umbriel', 'msg', 'config-reload'], check=True)


def main():
    home = Path.home()
    config_home = Path(os.environ.get('XDG_CONFIG_HOME', home / '.config'))
    data_home = Path(os.environ.get('XDG_DATA_HOME', home / '.local/share'))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--palette', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=data_home / 'icons/Bibata-Material-Teal')
    parser.add_argument('--icons-dir', type=Path, default=data_home / 'icons')
    parser.add_argument('--build-only', action='store_true')
    args = parser.parse_args()
    colors = json.loads(args.palette.read_text())
    colors = {key: colors[key].lower() for key in ('accent', 'interior', 'shadow')}
    if any(re.fullmatch(r'#[0-9a-f]{6}', value) is None for value in colors.values()):
        raise ValueError('Cursor palette colours must be #rrggbb')
    args.icons_dir.mkdir(parents=True, exist_ok=True)
    with (args.icons_dir / '.noctalia-cursor.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        name = build(args.source, args.icons_dir, colors)
        if not args.build_only:
            activate(name, args.icons_dir, config_home, home)
    print(name, flush=True)


if __name__ == '__main__':
    main()
