"""UPower fixture on the private test bus. Commands update, add or remove batteries."""
import json
import sys
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
name = 'org.freedesktop.UPower'
path = '/org/freedesktop/UPower'
device_path = path + '/devices/battery_BAT0'
interface = name + '.Device'
types = dict(NativePath='s', Vendor='s', Model='s', Serial='s', Type='u', PowerSupply='b',
             IsPresent='b', State='u', Percentage='d', TimeToEmpty='x', TimeToFull='x',
             EnergyRate='d', Energy='d', EnergyFull='d', EnergyFullDesign='d',
             ChargeThresholdSupported='b')
values = dict(NativePath='BAT0', Vendor='Test', Model='Wireless mouse', Serial='aa:bb:cc:dd:ee:ff',
              Type=2, PowerSupply=True, IsPresent=False, State=2, Percentage=65., TimeToEmpty=7200,
              TimeToFull=1800, EnergyRate=0., Energy=30., EnergyFull=50., EnergyFullDesign=50.,
              ChargeThresholdSupported=False)
xml = '<node><interface name="'+interface+'">'+''.join(
    f'<property name="{key}" type="{kind}" access="read"/>' for key, kind in types.items()
)+'</interface></node>'
device_info = Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0]
devices = {}
registrations = {}
def register(target, props):
    devices[target] = props
    registrations[target] = bus.register_object(target, device_info, None,
        lambda _b, _sender, _path, _iface, key: GLib.Variant(types[key], devices[_path][key]), None)
register(device_path, values)
manager_info = Gio.DBusNodeInfo.new_for_xml('''<node><interface name="org.freedesktop.UPower">
<method name="EnumerateDevices"><arg type="ao" direction="out"/></method>
<method name="GetDisplayDevice"><arg type="o" direction="out"/></method>
<property name="OnBattery" type="b" access="read"/>
</interface></node>''').interfaces[0]
def method(_b, _sender, _path, _iface, name, _args, invocation):
    invocation.return_value(GLib.Variant('(ao)', (list(devices),)) if name == 'EnumerateDevices'
                            else GLib.Variant('(o)', (device_path,)))
bus.register_object(path, manager_info, method, lambda *_: GLib.Variant('b', True), None)
bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'RequestName',
              GLib.Variant('(su)', (name, 0)), None, Gio.DBusCallFlags.NONE, -1, None)
def update(_fd, _condition):
    line = sys.stdin.readline()
    if not line:
        loop.quit()
        return False
    command = json.loads(line)
    if 'add' in command:
        target = path + '/devices/' + command['add']
        props = dict(values, NativePath=command['add'], Serial='', PowerSupply=False,
                     TimeToEmpty=0, TimeToFull=0, EnergyFull=0., EnergyFullDesign=0.)
        props.update(command['properties'])
        register(target, props)
        bus.emit_signal(None, path, name, 'DeviceAdded', GLib.Variant('(o)', (target,)))
    elif 'remove' in command:
        target = path + '/devices/' + command['remove']
        bus.unregister_object(registrations.pop(target))
        devices.pop(target)
        bus.emit_signal(None, path, name, 'DeviceRemoved', GLib.Variant('(o)', (target,)))
    else:
        target = path + '/devices/' + command['device'] if 'device' in command else device_path
        changes = command['properties'] if 'device' in command else command
        devices[target].update(changes)
        bus.emit_signal(None, target, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                        GLib.Variant('(sa{sv}as)', (interface, {key: GLib.Variant(types[key], value)
                                                              for key, value in changes.items()}, [])))
    bus.flush_sync(None)
    print('ok', flush=True)
    return True
GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN, update)
loop = GLib.MainLoop()
print('ok', flush=True)
loop.run()
