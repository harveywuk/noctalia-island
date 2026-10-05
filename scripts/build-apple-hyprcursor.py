#!/usr/bin/env python3
"""Install ful1e5/apple_cursor as one theme holding both XCursors and scalable hyprcursors.

The XCursors are the project's release build. The hyprcursors are generated from its SVG sources
with the hotspots, aliases and frame delays of its configs/x.build.toml, recoloured the way its
render.json does, so both formats come from the same release and draw the same cursor.
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile
import tomllib
import zipfile

# render.json: the sources draw fills in #00FF00 and outlines in #0000FF.
VARIANTS = {'macOS': {'#00ff00': '#000000', '#0000ff': '#ffffff'},
            'macOS-White': {'#00ff00': '#ffffff', '#0000ff': '#000000'}}
CANVAS = 256  # Every source SVG is 256x256; build.toml hotspots are in that space.


def recolor(svg, colors):
    return re.sub(r'#[0-9a-fA-F]{6}', lambda m: colors.get(m[0].lower(), m[0]), svg)


def cursors(source, config):
    settings = config['cursors']
    fallback = settings['fallback_settings']
    names = {c['x11_name'] for key, c in settings.items() if key != 'fallback_settings'}
    for key, cursor in settings.items():
        if key == 'fallback_settings':
            continue
        png = cursor['png']
        if '*' in png:  # 'wait-*.png' -> the frames in svg/wait/
            frames_dir = png.split('-*')[0]
            frames = [f'{frames_dir}/{p.name}' for p in sorted((source / 'svg' / frames_dir).glob('*.svg'))]
        else:
            frames = [png.removesuffix('.png') + '.svg']
        yield {
            'name': cursor['x11_name'],
            'frames': frames,
            'hotspot': (cursor.get('x_hotspot', fallback['x_hotspot']) / CANVAS,
                        cursor.get('y_hotspot', fallback['y_hotspot']) / CANVAS),
            'delay': cursor.get('x11_delay', fallback['x11_delay']),
            # An alias that is also a real shape would define that shape twice.
            'aliases': [a for a in cursor.get('x11_symlinks', []) if a not in names],
        }


def write_hyprcursors(theme_dir, variant, source, config):
    out = theme_dir / 'hyprcursors'
    out.mkdir()
    colors = VARIANTS[variant]
    for cursor in cursors(source, config):
        meta = ['resize_algorithm = none',
                f'hotspot_x = {cursor["hotspot"][0]:.4f}',
                f'hotspot_y = {cursor["hotspot"][1]:.4f}', '']
        meta += [f'define_override = {alias}' for alias in cursor['aliases']]
        meta.append('')
        animated = len(cursor['frames']) > 1
        with zipfile.ZipFile(out / f'{cursor["name"]}.hlc', 'w', zipfile.ZIP_DEFLATED) as archive:
            for frame in cursor['frames']:
                name = Path(frame).name
                archive.writestr(name, recolor((source / 'svg' / frame).read_text(), colors))
                meta.append(f'define_size = 0, {name}' + (f', {cursor["delay"]}' if animated else ''))
            archive.writestr('meta.hl', '\n'.join(meta) + '\n')
    (theme_dir / 'manifest.hl').write_text(
        f'name = {variant}\ndescription = Apple cursors (ful1e5/apple_cursor) as hyprcursors\n'
        'version = 2.0.1\ncursors_directory = hyprcursors\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='apple_cursor checkout')
    parser.add_argument('--release', type=Path, required=True, help='unpacked macOS.tar.xz release')
    parser.add_argument('--variant', choices=VARIANTS, default='macOS')
    parser.add_argument('--icons-dir', type=Path, default=Path.home() / '.local/share/icons')
    args = parser.parse_args()
    config = tomllib.loads((args.source / 'configs/x.build.toml').read_text())
    destination = args.icons_dir / args.variant
    with tempfile.TemporaryDirectory(prefix='.apple-cursor-', dir=args.icons_dir) as tmp:
        stage = Path(tmp) / args.variant
        shutil.copytree(args.release / args.variant, stage, symlinks=True)
        write_hyprcursors(stage, args.variant, args.source, config)
        if destination.exists():
            shutil.rmtree(destination)
        stage.rename(destination)
    print(destination)


if __name__ == '__main__':
    main()
