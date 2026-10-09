#!/usr/bin/env python3
"""Read virtual Valve LED snapshots to check whether Steam drives the interface.

Protocol: https://github.com/anna-oake/leds-valve-shim
This records LED state only. It does not infer a download percentage, write to
sysfs, load a module, or modify Steam. Run as your normal desktop user.
"""

import argparse
import json
import os
import select
import struct
import sys
import time


HEADER = struct.Struct("=IHHQQ8B")
SIZE = 100
EFFECTS = ("off", "manual", "normal", "rainbow", "breath", "patrol", "factory", "demo")


def decode(data):
    if len(data) != SIZE:
        raise ValueError(f"Expected {SIZE} bytes, received {len(data)}")
    magic, version, size, seq, timestamp, *state = HEADER.unpack_from(data)
    if (magic, version, size) != (0x564C4544, 1, SIZE):
        raise ValueError("Unsupported Valve LED snapshot format")
    enabled, effect, brightness, delay, offset, level, patrol, shift = state
    if enabled not in (0, 1) or effect >= len(EFFECTS):
        raise ValueError("Invalid Valve LED snapshot state")
    return dict(seq=seq, monotonic_ns=timestamp, enabled=bool(enabled),
                effect=EFFECTS[effect], brightness_scale=brightness, delay=delay,
                breath_offset=offset, breath_level=level, patrol_num=patrol,
                color_shift=shift,
                pixels=[list(data[i:i + 4]) for i in range(HEADER.size, SIZE, 4)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/valve-leds-shim")
    parser.add_argument("--seconds", type=float, default=120)
    args = parser.parse_args()
    if not 0 < args.seconds <= 3600:
        parser.error("--seconds must be between 0 and 3600")
    try:
        fd = os.open(args.device, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
        with os.fdopen(fd, "rb", buffering=0) as device:
            poll = select.poll()
            poll.register(device, select.POLLIN)
            deadline = time.monotonic() + args.seconds
            previous = None
            changes = 0
            while True:
                snapshot = decode(device.read(SIZE))
                if snapshot["seq"] != previous:
                    print(json.dumps(snapshot), flush=True)
                    changes += previous is not None
                    previous = snapshot["seq"]
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                events = poll.poll(min(1000, max(1, int(remaining * 1000))))
                if any(flags & (select.POLLERR | select.POLLHUP | select.POLLNVAL)
                       for _, flags in events):
                    raise OSError("Valve LED device disconnected")
            print(f"Observed {changes} LED state changes; these do not identify the writer.",
                  file=sys.stderr)
    except (OSError, ValueError) as error:
        print(f"LED probe: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
