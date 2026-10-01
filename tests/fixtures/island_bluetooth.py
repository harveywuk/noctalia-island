"""One Bluetooth battery on a private test bus; stdin changes its properties."""
import json
import sys
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
adapter = '/org/bluez/hci0'
device = adapter + '/dev_AA_BB_CC_DD_EE_FF'
interfaces = {
    'org.bluez.Adapter1': (adapter, {'Address': ('s', '00:11:22:33:44:55'), 'Alias': ('s', 'Test adapter'),
                                  'Powered': ('b', True), 'Discovering': ('b', False)}),
    'org.bluez.Device1': (device, {'Address': ('s', 'AA:BB:CC:DD:EE:FF'), 'Alias': ('s', 'Test headphones'),
                                 'Adapter': ('o', adapter), 'Connected': ('b', False), 'Paired': ('b', False),
                                 'Trusted': ('b', False), 'Icon': ('s', 'audio-headphones')}),
    'org.bluez.Battery1': (device, {'Percentage': ('y', 75)}),
}
def properties(interface):
    return {key: GLib.Variant(kind, value) for key, (kind, value) in interfaces[interface][1].items()}
for interface, (path, values) in interfaces.items():
    xml = '<node><interface name="' + interface + '">' + ''.join(
        f'<property name="{key}" type="{kind}" access="read"/>' for key, (kind, _) in values.items()
    ) + '</interface></node>'
    info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
    bus.register_object(path, info, None,
                        lambda _b, _s, _p, iface, key: properties(iface)[key], None)
info = Gio.DBusNodeInfo.new_for_xml('''<node><interface name="org.freedesktop.DBus.ObjectManager">
<method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method>
</interface></node>''').interfaces[0]
def method(_b, _s, _p, _i, _m, _args, invocation):
    objects = {}
    for interface, (path, _) in interfaces.items():
        objects.setdefault(path, {})[interface] = properties(interface)
    invocation.return_value(GLib.Variant('(a{oa{sa{sv}}})', (objects,)))
bus.register_object('/', info, method, None, None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
              GLib.Variant('(su)', ('org.bluez', 0)), None, Gio.DBusCallFlags.NONE, -1, None)
def update(_fd, _condition):
    line = sys.stdin.readline()
    if not line:
        loop.quit()
        return False
    for key, value in json.loads(line).items():
        interface = 'org.bluez.Battery1' if key == 'Percentage' else 'org.bluez.Device1'
        path, values = interfaces[interface]
        kind, _ = values[key]
        values[key] = (kind, value)
        bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                        GLib.Variant('(sa{sv}as)', (interface, {key: GLib.Variant(kind, value)}, [])))
    bus.flush_sync(None)
    print('ok', flush=True)
    return True
GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN, update)
loop = GLib.MainLoop()
print('ok', flush=True)
loop.run()
