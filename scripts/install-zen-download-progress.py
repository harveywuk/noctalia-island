#!/usr/bin/python3
"""Install the local host; enable the signed browser add-on separately in Zen."""
import json
import pathlib
import shutil

root = pathlib.Path(__file__).resolve().parent
target = pathlib.Path.home() / '.local/share/noctalia/native-messaging/zen-download-progress.py'
manifest = pathlib.Path.home() / '.mozilla/native-messaging-hosts/firefox_launcherentry_integration.json'
if manifest.exists() and json.loads(manifest.read_text()).get('path') != str(target):
    raise SystemExit(f'An existing host is configured at {manifest}; leaving it intact.')
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(root / 'zen-download-progress.py', target)
target.chmod(0o755)
manifest.parent.mkdir(parents=True, exist_ok=True)
manifest.write_text(json.dumps({
    'name': 'firefox_launcherentry_integration',
    'description': 'Zen download progress for the Noctalia island',
    'path': str(target),
    'type': 'stdio',
    'allowed_extensions': ['firefox_launcherentry_integration@mpostaire.github.io'],
}, indent=2) + '\n')
print(f'Installed {manifest}')
print('Enable in Zen: https://addons.mozilla.org/firefox/addon/launcherentry-integration/')
