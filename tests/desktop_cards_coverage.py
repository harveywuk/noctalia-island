"""Private fixtures and interaction checks for the additional desktop widget families."""
import http.server
import datetime as dt
import json
import os
from pathlib import Path
import shlex
import subprocess
import threading
import time

from PIL import Image, ImageChops
import ocr


class Coverage:
    def __init__(self, base, out, env, config, header, processes):
        self.base, self.out, self.env = base, out, env
        self.config, self.header, self.processes = config, header, processes
        self.server = None
        self.requests = []
        self.lamp_on = False

    def prepare(self):
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_GET(self):
                if self.path == '/api/states':
                    assert self.headers.get('Authorization') == 'Bearer fixture-token'
                    body = json.dumps([
                        dict(entity_id='light.desk', state='on' if owner.lamp_on else 'off',
                             attributes=dict(friendly_name='Desk lamp')),
                        dict(entity_id='sensor.room', state='21 C', attributes=dict(friendly_name='Room temperature')),
                        dict(entity_id='person.fixture', state='home', attributes=dict(friendly_name='Fixture location',
                                                                                     latitude=51.5, longitude=-.12)),
                    ])
                else:
                    body = '<rss><channel><item><title>Fixture headline</title><link>https://example.test/story</link>' \
                           '<pubDate>Today</pubDate><enclosure url="https://example.test/episode.ogg"/>' \
                           '</item></channel></rss>'
                self.send_response(200)
                self.end_headers()
                self.wfile.write(body.encode())

            def do_POST(self):
                assert self.headers.get('Authorization') == 'Bearer fixture-token'
                data = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.requests.append((self.path, data))
                owner.lamp_on = not owner.lamp_on
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'[]')

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f'http://127.0.0.1:{self.server.server_port}'
        self.env.update(no_proxy='127.0.0.1,localhost', NO_PROXY='127.0.0.1,localhost')
        self.album = self.base/'album'
        self.album.mkdir()
        Image.new('RGB', (640, 480), (220, 70, 50)).save(self.album/'Amber.png')
        Image.new('RGB', (640, 480), (40, 90, 220)).save(self.album/'Blue.png')
        self.notes = self.base/'Notes.md'
        self.notes.write_text('- [2026-10-05 12:00] Fixture note\n')
        activity = self.base/'state/noctalia/screen_time.json'
        activity.parent.mkdir(parents=True, exist_ok=True)
        hours = [0]*24
        hours[9], hours[10] = 1800, 900
        activity.write_text(json.dumps({dt.date.today().isoformat(): dict(
            apps={'fixture-editor': 2700}, hourly=hours, app_hourly={'fixture-editor': hours})}))
        self.token = self.base/'token'
        self.token.write_text('fixture-token\n')
        self.token.chmod(0o600)
        self.videos = self.base/'videos'
        self.videos.mkdir()
        (self.videos/'Fixture movie.mp4').write_bytes(b'fixture, never played')
        self.open_log = self.base/'opened.jsonl'
        self.open_log.write_text('')
        self.bin = self.base/'bin'
        self.bin.mkdir()
        opener = self.bin/'xdg-open'
        opener.write_text('#!/usr/bin/env python3\nimport json,sys\n'
                          f'with open({str(self.open_log)!r}, "a") as f: f.write(json.dumps(sys.argv[1:])+"\\n")\n')
        opener.chmod(0o755)
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']
        fixture = Path(__file__).parent/'fixtures/island_battery.py'
        battery = subprocess.Popen(['python3', str(fixture)], env=self.env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        self.processes.append(battery)
        assert battery.stdout.readline().strip() == 'ok'
        battery.stdin.write(json.dumps(dict(IsPresent=True, Model='Test laptop', Percentage=65))+'\n')
        battery.stdin.flush()
        assert battery.stdout.readline().strip() == 'ok'
        self.battery = battery
        self.page('personal', [
            ('reminders', dict(items=['Buy bread', 'Review widgets', 'Take a walk'], list_name='Fixture tasks')),
            ('photos', dict(folder_path=str(self.album))),
            ('notes', dict(file_path=str(self.notes))),
            ('journal', {}),
        ])

    def page(self, name, cards):
        blocks = []
        for index, (kind, settings) in enumerate(cards):
            x, y = 248 + (index % 2)*512, 248 + (index // 2)*488
            key = f'{name}_{kind}'
            block = f'[desktop_widgets.widget.{key}]\ntype="{kind}"\noutput="HEADLESS-1"\n' \
                    f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n' \
                    f'[desktop_widgets.widget.{key}.settings]\ncard_size="large"\n' \
                    'background_padding=14\nbackground_radius=16\n'
            for option, value in settings.items():
                if isinstance(value, dict):
                    block += option + '={' + ','.join(json.dumps(k)+'='+json.dumps(v) for k, v in value.items()) + '}\n'
                else:
                    block += option+'='+json.dumps(value)+'\n'
            blocks.append(block)
        self.config.write_text(self.header+''.join(blocks))
        if hasattr(self, 'validate'):
            self.validate()

    def exercise(self, binary, run, msg, shot, click, park, key):
        self.validate = lambda: run([binary, 'config', 'validate'])
        def text(name, label, **limits):
            point = ocr.find(self.out/(name+'.png'), label, **limits)
            assert point, f'{label} missing from {name}'
            return point

        def opened():
            return [json.loads(line)[0] for line in self.open_log.read_text().splitlines()]

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        for mode in ('dark', 'light'):
            msg('theme-mode-set', mode)
            park()
            shot('personal-'+mode)
        text('personal-light', 'Fixture note')
        click(*text('personal-light', 'Buy bread'))
        park()
        shot('reminder-completed')
        text('reminder-completed', '2 remaining')
        states = list((self.base/'state').rglob('*.toml'))
        assert any('Buy bread' in path.read_text() for path in states), 'reminder was not persisted'
        before = shot('photo-before')
        click(948, 436)
        park()
        after = shot('photo-after')
        assert ImageChops.difference(before.crop((560,60,940,370)), after.crop((560,60,940,370))).getbbox(), 'photo next did not change image'
        text('photo-after', 'Blue')
        # Journal opens the shell editor and creates a dated entry only after typing.
        journal = self.base/'data/noctalia/Journal.md'
        click(948, 546)
        shot('journal-editor')
        text('journal-editor', 'Journal.md')
        assert not journal.exists(), 'opening Journal wrote an entry without user input'
        click(1050, 840)
        for code in (35, 18, 38, 38, 24):
            key(code)  # hello
        key(1)
        time.sleep(1)
        assert journal.exists() and 'hello' in journal.read_text(), 'journal entry did not save'
        assert self.notes.read_text() == '- [2026-10-05 12:00] Fixture note\n', 'Journal changed Notes'
        shot('journal-saved')
        text('journal-saved', 'hello')
        self.notes.write_text('Plain text note\n')
        time.sleep(.5)
        shot('notes-updated')
        text('notes-updated', 'Plain text note')
        marker = self.base/'shortcut-ran'
        self.page('actions', [
            ('shortcuts', dict(entries={'Run fixture': 'touch '+shlex.quote(str(marker))})),
            ('contacts', dict(entries={'Fixture person': 'mailto:fixture@example.test'})),
            ('reading_list', dict(entries={'Read fixture': 'https://example.test/article'})),
            ('tips', {}),
        ])
        time.sleep(2)
        shot('actions')
        assert not marker.exists(), 'shortcut ran on load'
        click(*text('actions', 'Run fixture'))
        time.sleep(.5)
        assert marker.exists(), 'shortcut click failed'
        click(*text('actions', 'Fixture person'))
        click(*text('actions', 'Read fixture'))
        time.sleep(.5)
        assert 'mailto:fixture@example.test' in opened() and 'https://example.test/article' in opened(), 'configured URI actions failed'
        self.page('status', [
            ('batteries', {}), ('screen_time', {}),
            ('video_library', dict(folder_path=str(self.videos))), ('stocks', {}),
        ])
        time.sleep(2)
        park()
        shot('status')
        text('status', '65%')
        text('status', 'Enable Screen Time')
        text('status', 'Alpha Vantage')
        click(*text('status', 'Fixture movie'))
        time.sleep(.5)
        assert str(self.videos/'Fixture movie.mp4') in opened(), 'video action failed'
        self.battery.stdin.write('{"Percentage": 42}\n')
        self.battery.stdin.flush()
        assert self.battery.stdout.readline().strip() == 'ok'
        time.sleep(.5)
        shot('battery-update')
        text('battery-update', '42%')
        self.header = self.header.replace('[shell]\n', '[shell]\nscreen_time_enabled=true\n')
        self.page('activity', [('screen_time', {})])
        time.sleep(2)
        park()
        shot('screen-time-active')
        text('screen-time-active', '45m')
        text('screen-time-active', 'fixture-editor')
        self.page('remote', [
            ('news', dict(feed_url=self.url+'/feed')),
            ('podcasts', dict(feed_url=self.url+'/podcast')),
            ('home', dict(server_url=self.url, token_file=str(self.token), entities=['light.desk', 'sensor.room'])),
            ('find_my', dict(server_url=self.url, token_file=str(self.token), entities=['person.fixture'])),
        ])
        time.sleep(3)
        park()
        shot('remote')
        assert not self.requests, 'loading Home triggered a control action'
        click(*text('remote', 'Fixture headline', max_y=300))
        click(*text('remote', 'Fixture headline', min_x=550, max_y=300))
        click(*text('remote', 'Fixture location', min_y=500))
        click(*text('remote', 'Desk lamp', min_y=500))
        time.sleep(1)
        assert self.requests == [('/api/services/homeassistant/toggle', {'entity_id': 'light.desk'})], 'Home did not toggle exactly the selected device'
        assert 'https://example.test/story' in opened() and 'https://example.test/episode.ogg' in opened(), 'feed actions did not choose article and audio URLs'
        assert any(url.startswith('https://www.openstreetmap.org/') for url in opened()), 'location did not open its map'
        shot('home-toggled')
        click(*text('remote', 'Room temperature', min_y=500))
        assert len(self.requests) == 1, 'sensor row triggered a device action'
        # Completion state survives destruction and recreation of the widget.
        self.page('restored', [('reminders', dict(items=['Buy bread', 'Review widgets'], list_name='Fixture tasks'))])
        time.sleep(2)
        shot('reminder-restored')
        text('reminder-restored', '1 remaining')
        print('PASS: 16 widget families, themes, reminders persistence, photos, journal editor, local actions, battery events, RSS/podcasts, Home controls and location maps', flush=True)
        print(self.out, flush=True)

    def close(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
