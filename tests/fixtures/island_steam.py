"""Private fake Steam process and content log; never starts the Steam client."""
import ctypes
import datetime
import os
import pathlib
import sys

assert ctypes.CDLL(None).prctl(15, b'steam', 0, 0, 0) == 0
home = pathlib.Path.home()
root = home / '.steam/steam'
(root / 'logs').mkdir(parents=True)
(root / 'steamapps').mkdir()
(home / '.steam/steam.pid').write_text(str(os.getpid()))
(root / 'steamapps/appmanifest_42.acf').write_text('"AppState" { "name" "Test game" }')
for line in sys.stdin:
    with (root / 'logs/content_log.txt').open('a') as log:
        log.write(datetime.datetime.now().strftime('[%Y-%m-%d %H:%M:%S]') + ' AppID 42 App update changed : ' + line)
    print('ok', flush=True)
