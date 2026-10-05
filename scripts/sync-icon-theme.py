#!/usr/bin/env python3
"""Sync an installed icon theme across GTK and Qt from the theme_mode_changed hook."""
import argparse
import configparser
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', nargs='?', default=os.environ.get('NOCTALIA_THEME_MODE'), choices=('light', 'dark'))
    parser.add_argument('--light', default='WhiteSur-light')
    parser.add_argument('--dark', default='WhiteSur-dark')
    args = parser.parse_args()
    if args.mode is None:
        parser.error('set NOCTALIA_THEME_MODE or pass light/dark')
    theme = args.light if args.mode == 'light' else args.dark
    if not theme or '/' in theme or '\\' in theme or theme in ('.', '..'):
        parser.error('theme must be an installed theme name')
    data = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    roots = [data / 'icons', Path.home() / '.icons']
    roots += [Path(p) / 'icons' for p in os.environ.get('XDG_DATA_DIRS', '/usr/local/share:/usr/share').split(':') if p]
    if not any((root / theme / 'index.theme').is_file() for root in roots):
        parser.error(f'icon theme is not installed: {theme}')
    config = Path(os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
    targets = [(config / gtk / 'settings.ini', 'Settings', 'gtk-icon-theme-name', True)
               for gtk in ('gtk-3.0', 'gtk-4.0')]
    targets += [(config / qt / (qt + '.conf'), 'Appearance', 'icon_theme', False) for qt in ('qt5ct', 'qt6ct')]
    for path, section, key, create in targets:
        if not create and not path.exists():
            continue
        settings = configparser.ConfigParser(interpolation=None, strict=False)
        settings.optionxform = str
        settings.read(path)
        if not settings.has_section(section):
            settings.add_section(section)
        if settings[section].get(key) == theme:
            continue
        settings[section][key] = theme
        # Resolve before writing so a linked settings file remains linked.
        target = path.resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=target.parent, delete=False) as output:
                temporary = Path(output.name)
                settings.write(output)
            if target.exists():
                temporary.chmod(target.stat().st_mode & 0o777)
            temporary.replace(target)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    if shutil.which('gsettings'):
        subprocess.run(['gsettings', 'set', 'org.gnome.desktop.interface', 'icon-theme', theme], check=True)


if __name__ == '__main__':
    main()
