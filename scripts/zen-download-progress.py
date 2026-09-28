#!/usr/bin/python3
"""Native host for the signed LauncherEntry Integration Firefox/Zen add-on.

Only aggregate count/progress crosses this bridge; no filenames or URLs are read.
The add-on's progress describes its first active download, not a byte-weighted total.
"""
import json
import math
import os
import struct
import sys

from gi.repository import Gio, GLib


def parse_message(payload):
    count_text, progress_text = json.loads(payload).split(":", 1)
    count, progress = int(count_text), float(progress_text)
    if not 0 <= count <= 1_000_000 or not math.isfinite(progress):
        raise ValueError("Invalid progress")
    return count, max(0.0, min(1.0, progress))


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    loop = GLib.MainLoop()
    buffer = bytearray()
    state = [0, 0.0]

    def publish():
        count, progress = state
        properties = {
            "count": GLib.Variant("x", count),
            "count-visible": GLib.Variant("b", count > 0),
            "progress": GLib.Variant("d", progress),
            # Show a just-started or unknown-size transfer too.
            "progress-visible": GLib.Variant("b", count > 0),
        }
        bus.emit_signal(None, "/com/canonical/unity/launcherentry/zen",
                        "com.canonical.Unity.LauncherEntry", "Update",
                        GLib.Variant("(sa{sv})", ("application://zen.desktop", properties)))

    def owner_changed(connection, sender, path, interface, signal, parameters):
        if parameters.unpack()[2]:
            publish()

    bus.signal_subscribe("org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                         "/org/freedesktop/DBus", "com.canonical.Unity",
                         Gio.DBusSignalFlags.NONE, owner_changed)

    def readable(fd, condition):
        chunk = os.read(fd, 4096)
        if not chunk:
            loop.quit()
            return False
        buffer.extend(chunk)
        while len(buffer) >= 4:
            length = struct.unpack("=I", buffer[:4])[0]
            if not 0 < length <= 4096:
                loop.quit()
                return False
            if len(buffer) < length + 4:
                break
            payload = bytes(buffer[4:4 + length])
            del buffer[:4 + length]
            try:
                state[:] = parse_message(payload)
                publish()
            except (ValueError, TypeError, AttributeError, UnicodeError):
                pass
        return True

    GLib.io_add_watch(sys.stdin.fileno(), GLib.IO_IN | GLib.IO_HUP | GLib.IO_ERR, readable)
    loop.run()


if __name__ == "__main__":
    main()
