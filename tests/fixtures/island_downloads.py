"""Controlled desktop progress publisher on the private test bus."""
import json
import sys
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
for line in sys.stdin:
    data = json.loads(line)
    properties = {key: GLib.Variant('b' if isinstance(value, bool) else 's' if isinstance(value, str) else 'd', value)
                  for key, value in data['properties'].items()}
    bus.emit_signal(None, '/com/canonical/unity/launcherentry/test',
                    'com.canonical.Unity.LauncherEntry', 'Update',
                    GLib.Variant('(sa{sv})', (data['uri'], properties)))
    bus.flush_sync(None)
    print('ok', flush=True)
