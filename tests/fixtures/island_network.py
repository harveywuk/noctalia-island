"""NetworkManager on the private test bus. No host devices or connections are used."""
import json
import sys
import time
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
name = 'org.freedesktop.NetworkManager'
root = '/org/freedesktop/NetworkManager'
wifi, wired = root+'/Devices/1', root+'/Devices/2'
wifi_active, wired_active = root+'/ActiveConnection/1', root+'/ActiveConnection/2'
ap = root+'/AccessPoint/1'
state = dict(kind='wifi', connected=True, resolving=False, ssid='Home', strength=80, frequency=5180)
objects = {}


def register(path, interface, properties, methods=''):
    objects[path, interface] = properties
    xml = '<node><interface name="'+interface+'">'+''.join(
        f'<property name="{key}" type="{kind}" access="read"/>' for key, (kind, _) in properties.items()
    )+methods+'</interface></node>'
    info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
    bus.register_object(path, info, method,
                        lambda _b, _s, p, i, k: GLib.Variant(*objects[p, i][k]), None)


def method(_b, _s, path, interface, name, args, invocation):
    if name == 'GetDevices':
        invocation.return_value(GLib.Variant('(ao)', ([wifi, wired],)))
    elif name == 'ListConnections':
        invocation.return_value(GLib.Variant('(ao)', ([],)))
    elif name == 'GetAccessPoints':
        invocation.return_value(GLib.Variant('(ao)', ([ap],)))
    else:
        invocation.return_value(None)
        if name == 'RequestScan':
            objects[wifi, root_interface+'.Device.Wireless']['LastScan'] = ('x', int(time.monotonic()*1000))
            emit(wifi, root_interface+'.Device.Wireless', ['LastScan'])


def emit(path, interface, keys=None):
    props = objects[path, interface]
    bus.emit_signal(None, path, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                    GLib.Variant('(sa{sv}as)', (interface,
                        {k: GLib.Variant(*v) for k, v in props.items() if keys is None or k in keys}, [])))


root_interface = name
register(root, name, {'PrimaryConnection': ('o', wifi_active), 'ActiveConnections': ('ao', [wifi_active]),
                     'WirelessEnabled': ('b', True), 'State': ('u', 70), 'Connectivity': ('u', 4)},
         '<method name="GetDevices"><arg type="ao" direction="out"/></method>')
register(root+'/Settings', name+'.Settings', {},
         '<method name="ListConnections"><arg type="ao" direction="out"/></method>')
register(root+'/AgentManager', name+'.AgentManager', {},
         '<method name="Register"><arg type="s" direction="in"/></method>'
         '<method name="RegisterWithCapabilities"><arg type="s" direction="in"/><arg type="u" direction="in"/></method>'
         '<method name="Unregister"/>')
for path, active, kind, interface in ((wifi, wifi_active, 2, 'wlan0'), (wired, wired_active, 1, 'eth0')):
    register(path, name+'.Device', {'DeviceType': ('u', kind), 'Interface': ('s', interface),
                                   'State': ('u', 100 if kind == 2 else 30),
                                   'ActiveConnection': ('o', active if kind == 2 else '/'), 'Ip4Config': ('o', '/')})
    register(active, name+'.Connection.Active', {'Type': ('s', '802-11-wireless' if kind == 2 else '802-3-ethernet'),
             'State': ('u', 2 if kind == 2 else 4), 'Devices': ('ao', [path]), 'Connection': ('o', '/'),
             'Default': ('b', kind == 2), 'Ip4Config': ('o', '/')})
register(wifi, name+'.Device.Wireless', {'ActiveAccessPoint': ('o', ap), 'LastScan': ('x', 1), 'AccessPoints': ('ao', [ap])},
         '<method name="GetAccessPoints"><arg type="ao" direction="out"/></method>'
         '<method name="RequestScan"><arg type="a{sv}" direction="in"/></method>')
register(ap, name+'.AccessPoint', {'Ssid': ('ay', list(b'Home')), 'Strength': ('y', 80), 'Frequency': ('u', 5180),
                                 'WpaFlags': ('u', 0), 'RsnFlags': ('u', 0)})
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
              GLib.Variant('(su)', (name, 0)), None, Gio.DBusCallFlags.NONE, -1, None)


def update(_fd, _condition):
    line = sys.stdin.readline()
    if not line:
        loop.quit()
        return False
    state.update(json.loads(line))
    active = state['connected'] or state['resolving']
    primary = (wifi_active if state['kind'] == 'wifi' else wired_active) if active else '/'
    for path, connection, kind in ((wifi, wifi_active, 'wifi'), (wired, wired_active, 'wired')):
        selected = active and state['kind'] == kind
        objects[path, name+'.Device'].update(
            State=('u', 100 if selected and state['connected'] else 40 if selected else 30),
            ActiveConnection=('o', connection if selected else '/'))
        objects[connection, name+'.Connection.Active'].update(
            State=('u', 2 if selected and state['connected'] else 1 if selected else 4),
            Default=('b', selected and state['connected']))
    objects[wifi, name+'.Device.Wireless']['ActiveAccessPoint'] = ('o', ap if active and state['kind'] == 'wifi' else '/')
    objects[ap, name+'.AccessPoint'].update(Ssid=('ay', list(state['ssid'].encode())),
        Strength=('y', state['strength']), Frequency=('u', state['frequency']))
    objects[root, name].update(PrimaryConnection=('o', primary if state['connected'] else '/'),
                              ActiveConnections=('ao', [primary] if active else []),
                              State=('u', 70 if state['connected'] else 40 if active else 20))
    emit(wifi, name+'.Device.Wireless', ['ActiveAccessPoint'])
    emit(ap, name+'.AccessPoint')
    emit(root, name, ['PrimaryConnection', 'ActiveConnections', 'State'])
    bus.flush_sync(None)
    print('ok', flush=True)
    return True


GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN, update)
loop = GLib.MainLoop()
print('ok', flush=True)
loop.run()
