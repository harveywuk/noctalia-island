#!/usr/bin/env python3
"""Exercise installer failures, complete rollback snapshots, and session ownership in a private HOME."""
import json
import configparser
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
STUB = '''#!/usr/bin/python3
import json, os, pathlib, sys
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['SETUP_TEST_LOG'], 'a') as log:
    log.write(json.dumps([name, *args]) + '\\n')
if name == 'uwsm':
    sys.exit(0 if os.environ.get('SETUP_TEST_UWSM') == '1' else 1)
if name == 'systemctl':
    sys.exit(1 if 'is-active' in args else 0)
if name == 'meson':
    state = pathlib.Path(os.environ['SETUP_TEST_STATE'])
    if args[0] in ('setup', 'configure'):
        prefix = next(a.split('=', 1)[1] for a in args if a.startswith('--prefix='))
        state.write_text(prefix)
    elif args[0] == 'test':
        sys.exit(int(os.environ.get('SETUP_TEST_FAIL', '0')))
    elif args[0] == 'install':
        if os.environ.get('SETUP_TEST_INSTALL_FAIL'):
            sys.exit(1)
        root = pathlib.Path(args[args.index('--destdir') + 1] + state.read_text())
        (root / 'bin').mkdir(parents=True)
        binary = root / 'bin/noctalia'
        binary.write_text('#!/bin/sh\\nexit 0\\n')
        binary.chmod(0o755)
        assets = root / 'share/noctalia/assets'
        assets.mkdir(parents=True)
        (assets / 'generation').write_text('new assets')
'''


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='noctalia-setup-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / 'tools'
        self.bin.mkdir()
        for name in ('meson', 'systemctl', 'uwsm', 'dbus-update-activation-environment', 'gsettings'):
            path = self.bin / name
            path.write_text(STUB)
            path.chmod(0o755)
        self.prefix = self.root / 'installed shell %test'
        self.env = dict(os.environ, HOME=str(self.root), XDG_CONFIG_HOME=str(self.root / 'config'),
                        XDG_DATA_HOME=str(self.root / 'data'), PATH=str(self.bin) + ':' + os.environ['PATH'],
                        NOCTALIA_PREFIX=str(self.prefix), NOCTALIA_BUILD_DIR=str(self.root / 'build'),
                        NOCTALIA_UNIT='test-shell.service', SETUP_TEST_LOG=str(self.root / 'calls'),
                        SETUP_TEST_STATE=str(self.root / 'prefix'))

    def run_script(self, path, *args, success=True):
        result = subprocess.run(['bash', str(REPO / path), *args], env=self.env, text=True, capture_output=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result

    def calls(self):
        path = self.root / 'calls'
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def old_install(self):
        (self.prefix / 'bin').mkdir(parents=True)
        (self.prefix / 'bin/noctalia').write_text('old binary')
        assets = self.prefix / 'share/noctalia/assets'
        assets.mkdir(parents=True)
        (assets / 'generation').write_text('old assets')

    def test_existing_build_enables_tests_and_preserves_complete_install(self):
        self.old_install()
        build = self.root / 'build'; build.mkdir(); (build / 'build.ninja').touch()
        self.run_script('scripts/install-local.sh', '--no-restart', '--jobs', '2')
        calls = self.calls()
        self.assertIn('-Dtests=enabled', calls[0])
        self.assertEqual(calls[0][1], 'configure')
        self.assertLess(next(i for i,c in enumerate(calls) if c[:2] == ['meson','test']),
                        next(i for i,c in enumerate(calls) if c[:2] == ['meson','install']))
        backups = list(self.root.glob('installed shell %test.previous-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'bin/noctalia').read_text(), 'old binary')
        self.assertEqual((backups[0] / 'share/noctalia/assets/generation').read_text(), 'old assets')
        self.assertEqual((self.prefix / 'share/noctalia/assets/generation').read_text(), 'new assets')
        unit = (self.root / 'config/systemd/user/test-shell.service').read_text()
        self.assertIn('ExecStart="', unit)
        self.assertIn('%%test/bin/noctalia"', unit)
        hook = (self.root / 'data/dynamic-noctalia/start-shell.sh').read_text()
        self.assertIn('NOCTALIA_UNIT:-test-shell.service', hook)

    def test_failed_tests_leave_installation_and_services_untouched(self):
        self.old_install(); self.env['SETUP_TEST_FAIL'] = '1'
        self.run_script('scripts/install-local.sh', success=False)
        self.assertEqual((self.prefix / 'bin/noctalia').read_text(), 'old binary')
        self.assertFalse(list(self.root.glob('*.previous-*')))
        self.assertFalse(any(c[0] == 'systemctl' or c[:2] == ['meson','install'] for c in self.calls()))

    def test_preserves_existing_unit_and_start_hook(self):
        unit = self.root / 'config/systemd/user/test-shell.service'
        hook = self.root / 'data/dynamic-noctalia/start-shell.sh'
        for path in (unit, hook):
            path.parent.mkdir(parents=True); path.write_text('personal configuration\n')
        self.run_script('scripts/install-local.sh', '--skip-tests', '--no-restart')
        self.assertFalse(any(c[:2] == ['meson','test'] for c in self.calls()))
        for path in (unit, hook):self.assertEqual(path.read_text(), 'personal configuration\n')

    def test_failed_staging_preserves_the_previous_installation(self):
        self.old_install(); self.env['SETUP_TEST_INSTALL_FAIL'] = '1'
        self.run_script('scripts/install-local.sh', success=False)
        self.assertEqual((self.prefix / 'bin/noctalia').read_text(), 'old binary')
        self.assertFalse(list(self.root.glob('.dynamic-noctalia-install.*')))
        self.assertFalse(any(c[0] == 'systemctl' for c in self.calls()))

    def test_invalid_arguments_do_not_start_build(self):
        for args in (('--prefix',), ('--jobs','0'), ('--unit','bad/unit'), ('--prefix',str(self.root))):
            self.run_script('scripts/install-local.sh', *args, success=False)
        self.assertEqual(self.calls(), [])

    def test_uwsm_owns_environment_and_portals(self):
        self.env.update(SETUP_TEST_UWSM='1', HYPRLAND_INSTANCE_SIGNATURE='test', WAYLAND_DISPLAY='wayland-test')
        self.run_script('examples/hyprland/start-shell.sh')
        self.assertEqual(self.calls(), [['uwsm','check','is-active','compositor-only'],
                                       ['systemctl','--user','--no-block','start','test-shell.service']])

    def test_plain_session_imports_environment_before_start(self):
        self.env.update(SETUP_TEST_UWSM='0', HYPRLAND_INSTANCE_SIGNATURE='test', WAYLAND_DISPLAY='wayland-test')
        self.run_script('examples/hyprland/start-shell.sh')
        calls = self.calls()
        self.assertEqual(calls[-1], ['systemctl','--user','--no-block','start','test-shell.service'])
        self.assertEqual(calls[-2][0], 'dbus-update-activation-environment')
        self.assertFalse(any('restart' in c for c in calls))

    def test_missing_display_does_not_start_services(self):
        self.env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        self.env.pop('WAYLAND_DISPLAY', None)
        self.run_script('examples/hyprland/start-shell.sh', success=False)
        self.assertEqual(self.calls(), [])

    def test_icon_switch_preserves_links_and_unrelated_settings(self):
        for mode in ('light', 'dark'):
            theme = self.root / f'data/icons/WhiteSur-{mode}'
            theme.mkdir(parents=True); (theme / 'index.theme').write_text('[Icon Theme]\n')
        qt = self.root / 'config/qt6ct/qt6ct.conf'; qt.parent.mkdir(parents=True)
        target = self.root / 'qt-settings'
        target.write_text('[Appearance]\nstyle = kvantum\n[Fonts]\ngeneral = Example Font\n')
        qt.symlink_to(target)
        for mode in ('light', 'dark'):
            subprocess.run(['python3', str(REPO / 'scripts/sync-icon-theme.py'), mode], env=self.env, check=True)
            settings = configparser.ConfigParser(); settings.read(qt)
            self.assertTrue(qt.is_symlink())
            self.assertEqual(settings['Appearance']['icon_theme'], f'WhiteSur-{mode}')
            self.assertEqual(settings['Appearance']['style'], 'kvantum')
            self.assertEqual(settings['Fonts']['general'], 'Example Font')
            for gtk in ('gtk-3.0', 'gtk-4.0'):
                settings.read(self.root / f'config/{gtk}/settings.ini')
                self.assertEqual(settings['Settings']['gtk-icon-theme-name'], f'WhiteSur-{mode}')
        self.assertFalse((self.root / 'config/qt5ct').exists())

    def test_missing_icon_theme_changes_nothing(self):
        result = subprocess.run(['python3', str(REPO / 'scripts/sync-icon-theme.py'),
                                 'light', '--light', 'noctalia-nonexistent-test-theme'],
                                env=self.env, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'config').exists())
        self.assertEqual(self.calls(), [])


if __name__ == '__main__':
    unittest.main()
