"""Publish the shim's snapshot ABI to a private file, never to the real driver."""
import json
import os
import pathlib
import struct
import sys
import threading
import time

path = pathlib.Path(os.environ['NOCTALIA_STEAM_LED_DEVICE'])
state = dict(color=[0, 255, 0], lit=8, enabled=True, effect=1, brightness=255, frozen=False, short=False)
lock = threading.Lock()
stopped = threading.Event()


def publish():
    seq = 0
    while not stopped.is_set():
        with lock:
            values = dict(state)
        if not values['frozen']:
            seq += 1
            data = struct.pack('=IHHQQ8B', 0x564c4544, 1, 100, seq, time.monotonic_ns(),
                               int(values['enabled']), values['effect'], values['brightness'], 10, 4, 32, 3, 5)
            for i in range(17):
                data += bytes((values['color'] if i >= 17-values['lit'] else [0, 0, 0])+[255])
            pending = path.with_suffix('.pending')
            pending.write_bytes(data[:8] if values['short'] else data)
            pending.replace(path)
        stopped.wait(.05)


thread = threading.Thread(target=publish)
thread.start()
print('ok', flush=True)
try:
    for line in sys.stdin:
        with lock:
            state.update(json.loads(line))
        print('ok', flush=True)
finally:
    stopped.set()
    thread.join()
