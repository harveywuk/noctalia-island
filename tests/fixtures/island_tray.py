"""A StatusNotifierItem with a solid square icon and a one-entry dbusmenu, for tray tests.

Activate and menu events are appended to $ISLAND_TEST_EVENTS.
"""
import os

from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
name = f'org.kde.StatusNotifierItem-{os.getpid()}-1'
size = 22
pixels = bytes([255, 255, 255, 255]) * size * size  # ARGB, opaque white.
item = {
    'Category': GLib.Variant('s', 'ApplicationStatus'),
    'Id': GLib.Variant('s', 'island-tray-test'),
    'Title': GLib.Variant('s', 'Tray test'),
    'Status': GLib.Variant('s', 'Active'),
    'IconName': GLib.Variant('s', ''),
    'IconPixmap': GLib.Variant('a(iiay)', [(size, size, pixels)]),
    'Menu': GLib.Variant('o', '/MenuBar'),
    'ItemIsMenu': GLib.Variant('b', False),
    'ToolTip': GLib.Variant('(sa(iiay)ss)', ('', [], 'Tray test', '')),
}
item_xml = '<node><interface name="org.kde.StatusNotifierItem">' + ''.join(
    f'<property name="{key}" type="{value.get_type_string()}" access="read"/>' for key, value in item.items()
) + ''.join(
    f'<method name="{method}"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>'
    for method in ('Activate', 'SecondaryActivate', 'ContextMenu')
) + '<method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method></interface></node>'

menu_xml = '''<node><interface name="com.canonical.dbusmenu">
<method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
<arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
<method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/>
<arg type="a(ia{sv})" direction="out"/></method>
<method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
<method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/>
<arg type="u" direction="in"/></method>
<method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
<method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
<method name="AboutToShowGroup"><arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/></method>
<property name="Version" type="u" access="read"/><property name="Status" type="s" access="read"/>
</interface></node>'''
entry = {'label': GLib.Variant('s', 'Fixture action'), 'enabled': GLib.Variant('b', True),
         'visible': GLib.Variant('b', True)}


def log(text):
    with open(os.environ['ISLAND_TEST_EVENTS'], 'a') as events:
        events.write(text + '\n')


def item_method(connection, sender, path, interface, method, parameters, invocation):
    log(method)
    invocation.return_value(None)


def menu_method(connection, sender, path, interface, method, parameters, invocation):
    if method == 'GetLayout':
        child = GLib.Variant('(ia{sv}av)', (1, entry, []))
        root = (0, {'children-display': GLib.Variant('s', 'submenu')}, [child])
        invocation.return_value(GLib.Variant('(u(ia{sv}av))', (1, root)))
    elif method == 'GetGroupProperties':
        invocation.return_value(GLib.Variant('(a(ia{sv}))', ([(1, entry)],)))
    elif method == 'GetProperty':
        invocation.return_value(GLib.Variant('(v)', (entry.get(parameters.unpack()[1], GLib.Variant('s', '')),)))
    elif method == 'Event':
        log(f'MenuEvent {parameters.unpack()[0]} {parameters.unpack()[1]}')
        invocation.return_value(None)
    elif method == 'EventGroup':
        for event in parameters.unpack()[0]:
            log(f'MenuEvent {event[0]} {event[1]}')
        invocation.return_value(GLib.Variant('(ai)', ([],)))
    elif method == 'AboutToShow':
        invocation.return_value(GLib.Variant('(b)', (False,)))
    else:
        invocation.return_value(GLib.Variant('(aiai)', ([], [])))


def menu_property(connection, sender, path, interface, key):
    return GLib.Variant('u', 3) if key == 'Version' else GLib.Variant('s', 'normal')


bus.register_object('/StatusNotifierItem', Gio.DBusNodeInfo.new_for_xml(item_xml).interfaces[0], item_method,
                    lambda c, s, p, i, key: item[key], None)
bus.register_object('/MenuBar', Gio.DBusNodeInfo.new_for_xml(menu_xml).interfaces[0], menu_method,
                    menu_property, None)


def register(*_):
    bus.call_sync('org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher', 'org.kde.StatusNotifierWatcher',
                  'RegisterStatusNotifierItem', GLib.Variant('(s)', (name,)), None, Gio.DBusCallFlags.NONE, -1, None)


Gio.bus_own_name_on_connection(bus, name, Gio.BusNameOwnerFlags.NONE, None, None)
Gio.bus_watch_name_on_connection(bus, 'org.kde.StatusNotifierWatcher', Gio.BusNameWatcherFlags.NONE, register, None)
GLib.MainLoop().run()
