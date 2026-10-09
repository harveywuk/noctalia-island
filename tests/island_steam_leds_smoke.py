"""Actual LED snapshots through the service and GPU, on a private bus and compositor."""
import json
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    from island_completion_smoke import prepare as completion_prepare
    completion_prepare(base, cfg, env)
    env['NOCTALIA_STEAM_LED_DEVICE'] = str(base/'steam-leds.snapshot')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    original = (cfg/'config.toml').read_text()
    ring = (477, 5, 803, 78)

    def fixture(name):
        proc = subprocess.Popen([sys.executable, str(repo/'tests/fixtures'/name)], env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        helpers.append(proc)
        return proc

    def command(proc, value):
        proc.stdin.write(value+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def led(**values):
        command(leds, json.dumps(values)); time.sleep(.3)

    def event(value):
        command(steam, value); time.sleep(2.3)

    def raw_event(value, app=42):
        with (pathlib.Path(env['HOME'])/'.steam/steam/logs/content_log.txt').open('a') as log:
            log.write(time.strftime('[%Y-%m-%d %H:%M:%S]')+f' AppID {app} '+value+'\n')
        time.sleep(2.3)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def count(image, color, box=ring):
        return sum((g > 130 and g > r+60 and g > b+35) if color == 'green'
                   else (r > 130 and r > g+60 and r > b+35)
                   for r, g, b in image.crop(box).getdata())

    def present(name, color, box=ring):
        image = shot(name)
        assert count(image, color, box) > 25, (name, color, 'LED pattern missing')
        return image

    def absent(name):
        image = shot(name)
        assert count(image, 'green') < 10 and count(image, 'red') < 10, (name, 'LED pattern remained')

    def configure(split=False, reduced=False, outer=True):
        text = original.replace('split_activities=false', 'split_activities='+str(split).lower())
        if split:
            text = text.replace('downloads-media-timers', 'media-downloads-timers')
        text = text.replace('[island]\n', '[island]\nouter_progress_ring='+str(outer).lower()+'\n')
        (cfg/'config.toml').write_text(text+'\n[shell.animation]\nenabled='+str(not reduced).lower()+'\n')
        msg('config-reload'); time.sleep(.8)

    try:
        protocol = repo/'protocols/virtual-keyboard-unstable-v1.xml'
        run(['wayland-scanner', 'client-header', str(protocol), str(base/'keyboard-client.h')])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/'keyboard-code.c')])
        run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_keyboard.c'), str(base/'keyboard-code.c'),
             '-lwayland-client', '-lxkbcommon', '-o', str(base/'keyboard')])
        keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, text=True)
        helpers.append(keyboard)
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        dispatch('hl.dsp.cursor.move({x=1100,y=600})')
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        steam = fixture('island_steam.py')
        leds = fixture('island_steam_leds.py'); assert leds.stdout.readline().strip() == 'ok'
        event('Running Update,Downloading,')
        time.sleep(1)
        first = present('led-compact-green', 'green')
        scaled = shot('led-compact-scaled', 'TEST-2').resize((1280, 720))
        assert count(scaled, 'green') > 25
        # Steam's observed default intensity and colours, without a test tint.
        led(color=[1, 90, 255], brightness=55)
        shot('led-steam-blue')
        led(color=[0, 255, 0], brightness=255)
        led(color=[255, 0, 0])
        present('led-compact-red', 'red')
        assert count(shot('led-replaced-green'), 'green') < 10

        led(color=[0, 255, 0], lit=17)
        full = present('led-full-pattern', 'green')
        assert count(full, 'green') > count(first, 'green')*1.4
        # A full LED bar cannot synthesize the five-second completion notice.
        time.sleep(.5); present('led-full-still-active', 'green')
        led(lit=8)
        before = shot('led-artwork-before').crop((580, 17, 700, 23)); time.sleep(.4)
        after = shot('led-artwork-after').crop((580, 17, 700, 23))
        assert ImageChops.difference(before, after).getbbox(), 'LED updates froze media artwork'

        # The same pattern follows the expanded geometry and fractional scale.
        msg('island-focus'); time.sleep(.8)
        image = present('led-expanded', 'green', (440, 4, 840, 260))
        led(color=[255, 0, 0]); present('led-expanded-updated', 'red', (440, 4, 840, 260))
        command(keyboard, '1'); time.sleep(.8)

        configure(split=True)
        led(color=[0, 255, 0]); time.sleep(.5)
        present('led-split', 'green', (760, 5, 880, 90))
        configure(outer=False)
        present('led-inner-ring', 'green', (485, 12, 540, 69))
        configure(reduced=True)
        absent('led-reduced-motion')
        configure()
        present('led-motion-restored', 'green')

        raw_event('App update changed : Running Update,Downloading,', 43)
        absent('led-ambiguous-games')
        raw_event('App update changed : None', 43)
        present('led-single-game-again', 'green')
        raw_event('update canceled : Priority (Suspended)')
        assert count(shot('led-paused'), 'green') < 10
        event('Running Update,Downloading,'); present('led-unpaused', 'green')
        publisher = fixture('island_downloads.py')
        command(publisher, json.dumps({'uri': 'application://steam.desktop',
                                      'properties': {'progress': .35, 'progress-visible': True}}))
        time.sleep(.3); absent('led-native-progress-priority')
        command(publisher, json.dumps({'uri': 'application://steam.desktop',
                                      'properties': {'progress-visible': False}}))
        time.sleep(2.3); present('led-native-progress-hidden', 'green')
        event('Running Update,Validating,'); absent('led-verifying-fallback')
        event('Running Update,Downloading,'); present('led-resumed', 'green')
        led(frozen=True); time.sleep(5.4); absent('led-stale-fallback')
        led(frozen=False); time.sleep(2.2); present('led-recovered', 'green')
        led(short=True); absent('led-malformed-fallback')
        led(short=False); time.sleep(2.2); present('led-valid-again', 'green')
        led(enabled=False); absent('led-disabled-fallback')
        led(enabled=True); time.sleep(2.2); present('led-enabled-again', 'green')
        leds.terminate(); leds.wait(timeout=5)
        pathlib.Path(env['NOCTALIA_STEAM_LED_DEVICE']).unlink()
        time.sleep(.3); absent('led-device-removed')
        event('None'); absent('led-transfer-ended')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: LED colours, extent, live updates, expanded and split geometry, inner ring, fractional '
              'scale, artwork, reduced motion, ambiguous games, pauses, native progress, phase gating, stale/malformed/disabled/unavailable fallback '
              'and recovery without invented percentages or completion', flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
