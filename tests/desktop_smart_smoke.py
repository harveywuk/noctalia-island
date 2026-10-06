"""Smart Rotate against fixture calendar data and a real MPRIS service on a private bus."""
import datetime as dt
import json
import subprocess
import sys
import time
import tomllib
from pathlib import Path

import ocr
from PIL import Image, ImageChops
from desktop_stacks_smoke import Stacks


class Smart(Stacks):
    def prepare(self):
        # Only the child shell's timezone changes. Always exercise the morning
        # window without touching the machine clock or waiting until tomorrow.
        offset = dt.datetime.now(dt.timezone.utc).hour - 7
        self.env['TZ'] = f'UTC{offset:+d}'
        blocks = []
        for kind in ('clock', 'weather', 'media_player', 'calendar', 'stack'):
            blocks.append(f'[desktop_widgets.widget.{kind}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          'cx=640.0\ncy=512.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{kind}.settings]\ncard_size="large"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if kind == 'stack':
                blocks.append('members=["clock", "weather", "media_player", "calendar"]\n'
                              'smart_rotate=false\nauto_rotate=false\n')
        self.config.write_text(self.header+''.join(blocks))
        self.event = self.base/'calendars/local/smart.ics'

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def saved(key):
            for file in (self.base/'state').rglob('*.toml'):
                value = tomllib.loads(file.read_text()).get('desktop_stacks', {}).get(key)
                if value:
                    return json.loads(value).get('stack')
            return None

        def page():
            return saved('pages') or 'clock'

        def wait_page(expected, timeout=18):
            deadline = time.monotonic()+timeout
            while time.monotonic() < deadline:
                if page() == expected:
                    return
                time.sleep(.2)
            shot('failed-'+expected)
            raise AssertionError(f'expected {expected}, got {page()}')

        def stays(expected, seconds=12):
            deadline = time.monotonic()+seconds
            while time.monotonic() < deadline:
                assert page() == expected, f'{expected} unexpectedly rotated to {page()}'
                time.sleep(.2)

        def player_command(command):
            run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest',
                 '--object-path', '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.'+command])

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        self.env['ISLAND_TEST_ART'] = (Path(__file__).resolve().parents[1]/'assets/noctalia-wallpaper.png').as_uri()
        self.env['ISLAND_TEST_EVENTS'] = str(self.out/'player-actions.log')
        self.env['ISLAND_TEST_TICK'] = '1'
        self.env['ISLAND_TEST_TITLE'] = 'Smart Rotate playback'
        with (self.out/'player.log').open('w') as log:
            player = subprocess.Popen([sys.executable, str(Path(__file__).parent/'fixtures/island_player.py')],
                                      env=self.env, stdout=log, stderr=log)
        self.processes.append(player)
        time.sleep(2)
        stays('clock', 3)
        player_command('Pause')
        msg('desktop-widgets-edit')
        shot('editor')
        click(640, 512)
        click(732, 92)
        park()
        point = None
        for attempt in range(8):
            shot('settings')
            Image.open(self.out/'settings.png').crop((220, 450, 530, 900)).save(self.out/'settings-control.png')
            point = ocr.find(self.out/'settings-control.png', 'Smart Rotate', exact=True)
            if point:
                point = (point[0]+220, point[1]+450)
            if point:
                break
            pointer('move 680 820')
            pointer('scroll 3')
        assert point, 'Smart Rotate toggle missing from stack inspector'
        click(707, point[1])
        park()
        shot('smart-enabled')
        stays('clock', 3)  # Preview must not rotate even though it is morning.
        msg('desktop-widgets-exit')
        state = tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']['widget']['stack']
        assert state['settings']['smart_rotate'] and not state['settings']['auto_rotate']
        wait_page('weather')
        morning = saved('mornings')
        assert morning and len(morning) == 8, 'morning priority was not remembered'
        shot('morning-weather')
        click(833, 721)  # Pin the weather page.
        park()
        assert saved('pins') == 'weather'
        player_command('Play')
        stays('weather')
        click(833, 721)  # Unpin, but leave the pointer over the card.
        pointer('move 640 512')
        stays('weather')
        park()
        wait_page('media_player')
        shot('active-playback')
        assert ocr.find(self.out/'active-playback.png', 'Smart Rotate playback'), 'selected media card did not update'
        backdrop_area = (448, 385, 485, 560)
        def backdrop_changed(before, after):
            return ImageChops.difference(before.crop(backdrop_area), after.crop(backdrop_area)).getbbox() is not None
        playing = shot('stack-flow')
        time.sleep(1.2)
        assert backdrop_changed(playing, shot('stack-flow-moving')), 'visible stack artwork did not animate'
        player_command('Pause')
        still = shot('stack-flow-paused')
        time.sleep(1.2)
        assert not backdrop_changed(still, shot('stack-flow-still')), 'paused stack artwork did not settle'
        player_command('Play')
        msg('desktop-widgets-edit')
        shot('media-preview')
        now = dt.datetime.now(dt.timezone.utc)
        stamp = lambda value: value.strftime('%Y%m%dT%H%M%SZ')
        self.event.write_text('BEGIN:VCALENDAR\nVERSION:2.0\nPRODID:-//Noctalia//Smart Test//EN\n'
                              'BEGIN:VEVENT\nUID:smart-rotate-meeting\n'
                              f'DTSTART:{stamp(now+dt.timedelta(minutes=10))}\n'
                              f'DTEND:{stamp(now+dt.timedelta(minutes=40))}\n'
                              'SUMMARY:Smart Rotate meeting\nEND:VEVENT\nEND:VCALENDAR\n')
        stays('media_player')
        shot('preview-still-media')
        msg('desktop-widgets-exit')
        park()
        wait_page('calendar')
        shot('upcoming-event')
        assert ocr.find(self.out/'upcoming-event.png', 'Smart Rotate meeting'), 'calendar priority lacks the event'
        self.event.unlink()
        wait_page('media_player')
        shot('event-removed')
        click(601, 721)  # Explicitly select Clock while playback is active.
        park()
        wait_page('clock')
        stays('clock')
        shot('manual-choice')
        click(833, 721)
        park()
        assert saved('pins') == 'clock'
        msg('desktop-widgets-hide')
        msg('desktop-widgets-show')
        stays('clock')
        assert saved('pins') == 'clock', 'pin lost during reconstruction'
        click(833, 721)
        park()
        wait_page('media_player')
        player_command('Pause')
        msg('desktop-widgets-hide')
        msg('desktop-widgets-show')
        stays('media_player')
        assert saved('mornings') == morning, 'morning weather was repeated after reconstruction'
        shot('settled')
        print('PASS: Smart Rotate opt-in, weather persistence, MPRIS playback, calendar priority, pin/hover/manual pauses and editor previews', flush=True)
