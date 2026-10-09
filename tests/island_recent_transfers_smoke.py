"""Recent transfer history through real private Steam logs, rendering and input."""
import csv
import io
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    from island_transfer_actions_smoke import prepare as actions_prepare
    actions_prepare(base, cfg, env)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.5)

    def click(x, y):
        move(x, y); command(pointer, 'press'); command(pointer, 'release'); time.sleep(.6)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def text(name, region=(485, 0, 797, 520), psm='6', output='TEST-1'):
        screenshot = shot(name, output)
        if output == 'TEST-2':
            screenshot = screenshot.resize((1280, 720))
        crop = screenshot.crop(region)
        crop = crop.resize((crop.width*3, crop.height*3))
        words = []
        for suffix, sample in (('text', crop), ('labels', crop.convert('L').point(lambda p: 255 if p > 110 else 0)),
                               ('titles', crop.convert('L').point(lambda p: 255 if p > 175 else 0))):
            path = out/(name+'-'+suffix+'.png'); sample.save(path)
            words.append(run(['tesseract', str(path), 'stdout', '--tessdata-dir', tessdata, '--psm', psm]).lower())
        return '\n'.join(words)

    def check_rows(name, expected, active=False):
        y = 206 if active else 151
        for index, word in enumerate(expected):
            title = text(name+f'-row-{index}', (518, y+55*index, 750, y+55*index+22), '7')
            assert word in title, (name, index, title)

    def click_word(word, name):
        path = out/(name+'-words.png')
        crop = shot(name).crop((450, 0, 830, 680)).resize((760, 1360))
        words = []
        for sample in (crop, crop.convert('L').point(lambda p: 255 if p > 110 else 0),
                       crop.convert('L').point(lambda p: 255 if p > 175 else 0)):
            sample.save(path)
            data = run(['tesseract', str(path), 'stdout', '--tessdata-dir', tessdata, '--psm', '11',
                        '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=288'])
            # The runner merges stderr; Tesseract can print its DPI estimate
            # before the TSV header on these small, moving artwork surfaces.
            header = data.find('level\tpage_num\t')
            assert header >= 0, data
            data = data[header:]
            words = [item for item in csv.DictReader(io.StringIO(data), delimiter='\t')
                     if (item.get('text') or '').lower().strip() == word]
            if words:
                break
        assert words, (name, word, data)
        item = min(words, key=lambda item: int(item['top']))
        click(450+(int(item['left'])+int(item['width'])/2)/2,
              (int(item['top'])+int(item['height'])/2)/2)

    def events(items):
        with (root/'logs/content_log.txt').open('a') as log:
            for app, message in items:
                log.write(time.strftime('[%Y-%m-%d %H:%M:%S]')+f' AppID {app} '+message+'\n')
        time.sleep(2.3)

    def current_class():
        return json.loads(ctl('-j', 'activewindow')).get('class', '')

    def focus_other():
        move()
        window = next(item for item in json.loads(ctl('-j', 'clients')) if item['class'] == 'transfer-other')
        dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
        wait(lambda: current_class() == 'transfer-other', 'Other window focused')

    def open_history():
        move(); move(735, 12); click(725, 39)

    try:
        for kind, protocol, libs in (
            ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
            ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
        ):
            run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
            run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
            run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
                 *libs, '-o', str(base/kind)])
            helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, text=True))
        pointer, keyboard = helpers
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'History fixture'
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        steam = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_steam.py')], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        helpers.append(steam); command(steam, 'None'); time.sleep(2.5)
        root = pathlib.Path(env['HOME'])/'.steam/steam'
        for app, name in ((42, 'Darktide'), (43, 'Hades II'), (44, 'Stardew Valley'),
                          (45, 'Baldurs Gate 3'), (46, 'Hollow Knight')):
            (root/f'steamapps/appmanifest_{app}.acf').write_text(f'"AppState" {{ "name" "{name}" }}')
        events([(app, 'App update changed : Running Update,Downloading,') for app in (42, 43, 44, 45)])
        began = time.monotonic()
        events([(44, 'update finished : No Error'), (43, 'update finished : Not enough disk space'),
                (42, 'update finished : No Error')])
        notice = text('batch-failure-priority', (590, 18, 760, 65))
        assert 'hades' in notice and 'not enough disk space' in notice, notice
        time.sleep(5.2); move(735, 12)
        shot('recent-three-with-active')
        assert 'baldurs' in text('history-active-title', (518, 116, 760, 140), '7')
        assert 'recent transfers' in text('history-heading', (478, 178, 600, 201), '7')
        check_rows('history-in-order', ('darktide', 'hades', 'stardew'), active=True)
        assert 'not enough disk space' in text('history-error-detail', (518, 282, 797, 306), '7')
        move(735, 732)
        assert 'darktide' in text('recent-three-scaled', (518, 206, 750, 228), '7', 'TEST-2')
        move(735, 12)
        before = shot('history-artwork-before').crop((560, 16, 720, 24)); time.sleep(.45)
        after = shot('history-artwork-after').crop((560, 16, 720, 24))
        assert ImageChops.difference(before, after).getbbox(), 'History stopped the artwork'

        # Duplicate, pause and cancellation leave the three stored results alone.
        events([(42, 'update finished : No Error'), (46, 'App update changed : Running Update,Downloading,'),
                (46, 'update canceled : Priority (Suspended)'), (46, 'update canceled : User canceled'),
                (45, 'update canceled : User canceled')])
        move(); compact = text('history-does-not-own-compact', (470, 0, 810, 100))
        assert 'recent transfers' not in compact and 'darktide' not in compact, compact
        move(735, 12)
        assert 'history fixture' in text('media-remains-default'), 'History displaced the media card'
        click(725, 39)
        rows = text('history-only')
        check_rows('history-only-order', ('darktide', 'hades', 'stardew'))
        assert 'hollow' not in rows, rows
        assert 'downloading' not in rows, rows
        history_only = shot('history-no-progress-ring')
        assert sum(b > 170 and g > 60 and r < 70 for r,g,b in history_only.crop((818, 35, 824, 270)).getdata()) < 10

        kitty = ['kitty', '--config', 'NONE', '-o', 'confirm_os_window_close=0']
        target = start(kitty+['--class', 'transfer-steam', '-e', 'sleep', '300'], 'steam-window.log')
        start(kitty+['--class', 'transfer-other', '-e', 'sleep', '300'], 'other-window.log')
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) >= 2, 'Fixture windows')
        focus_other(); open_history(); click(640, 165)
        wait(lambda: current_class() == 'transfer-steam', 'History row activates Steam')
        focus_other(); open_history(); msg('island-focus'); time.sleep(.6)
        command(keyboard, 15); command(keyboard, 15); command(keyboard, 28)
        wait(lambda: current_class() == 'transfer-steam', 'Keyboard history row activates Steam')

        # A fourth result evicts just the oldest, even when it shares the source app.
        focus_other()
        events([(46, 'App update changed : Running Update,Downloading,'), (46, 'update finished : No Error')])
        time.sleep(5.2); open_history()
        rows = text('history-bounded-three')
        check_rows('history-in-order-after-eviction', ('hollow', 'darktide', 'hades'))
        assert 'stardew' not in rows, rows
        msg('config-reload'); time.sleep(.8); open_history()
        assert 'hollow' in text('history-survives-config-reload')

        # Keep a row selected across the minute boundary; only its age text updates.
        msg('island-focus'); time.sleep(.6)
        command(keyboard, 15); command(keyboard, 15)
        time.sleep(max(0, began+62-time.monotonic()))
        aged = text('history-ages-update', (742, 206, 800, 229), '7')
        # At this font size Tesseract can read the numeral 1 as a lowercase i.
        assert '1m' in aged or 'im' in aged, aged
        command(keyboard, 28)
        wait(lambda: current_class() == 'transfer-steam', 'Age tick preserved keyboard history selection')
        focus_other(); target.terminate(); target.wait(timeout=5); time.sleep(1)
        open_history(); click(640, 165)
        assert current_class() == 'transfer-other', 'History launched or activated a missing app'

        # Without media or active jobs, the clock/calendar remain the default.
        player.terminate(); player.wait(timeout=5); move(); time.sleep(1)
        move(640, 20)
        idle = text('idle-history-entry')
        assert 'recent transfers' in idle and 'hollow' not in idle, idle
        click_word('recent', 'idle-open-history')
        assert 'hollow' in text('idle-history-open')
        assert shell.poll() is None
        print('PASS: three chronological results including batched Steam finishes, error priority, '
              'quiet compact/media/idle routing, history-only access, artwork and fractional scale, '
              'duplicate/pause/cancellation filtering, bounded eviction, source actions, ages and config reload', flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
