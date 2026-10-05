"""System tray in the expanded Island.

A fixture StatusNotifierItem (a white square with a one-entry menu) must appear in the hover
view; left click activates it, right click opens its menu from the Island (which stays expanded
meanwhile), and choosing the entry reaches the app.
"""
import os
import pathlib
import subprocess
import sys
import time


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace(
        '[island]\nenabled=true\n',
        '[island]\nenabled=true\nheight=64\nclock_size=24\nreserve_space=true\n', 1)
    text = text.replace('[shell]\n', '[shell]\noffline_mode=true\n', 1)
    path.write_text(text)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    from PIL import Image

    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
    events_path = base/'tray-events.txt'
    events_path.write_text('')
    env['ISLAND_TEST_EVENTS'] = str(events_path)

    def command(value):
        pointer.stdin.write(value+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.3)
        command('relative 1 0'); command('relative -1 0'); time.sleep(.4)

    def click(button=''):
        prefix = button+'-' if button else ''
        command(prefix+'press'); command(prefix+'release'); time.sleep(.8)

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)]); return path

    def events():
        return events_path.read_text()

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        fixture = start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'tray.log'); time.sleep(2)
        move(1100, 600); time.sleep(1)
        compact = Image.open(shot('tray-compact')).convert('RGB')
        xs = [x for x in range(compact.width) if max(compact.getpixel((x, 20))) < 12]
        ys = [y for y in range(0, 200) if max(compact.getpixel((compact.width//2, y))) < 12]
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        expanded = Image.open(shot('tray-expanded')).convert('RGB')
        # The fixture icon is a solid white square, unlike any glyph or text.
        square = None
        for y in range(bottom, 600):
            for x in range(compact.width//2 - 200, compact.width//2 + 200):
                if all(min(expanded.getpixel((x+dx, y+dy))) > 230 for dx in range(0, 12, 3) for dy in range(0, 12, 3)):
                    square = (x+6, y+6); break
            if square:
                break
        assert square, 'Tray icon missing from the expanded Island'
        move(*square); click()
        assert 'Activate' in events(), 'Left click did not activate the tray item: '+events()
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        move(*square); click('right'); time.sleep(1.5)
        menu = Image.open(shot('tray-menu')).convert('RGB')
        # The Island stays expanded while the menu it opened is showing.
        island_bottom = max(y for y in range(0, 600) if max(menu.getpixel((menu.width//2 - 150, y))) < 12)
        assert island_bottom > bottom + 40, f'Island collapsed under its tray menu (bottom {island_bottom})'
        # The menu opens just below the icon; read its only entry inside the entry's light box.
        x0, y0, x1, y1 = square[0]-110, square[1]+30, square[0]+110, square[1]+60
        region = menu.crop((x0, y0, x1, y1)).convert('L')
        region.resize((region.width*3, region.height*3), Image.LANCZOS).save(out/'tray-menu-ocr.png')
        words = run(['tesseract', str(out/'tray-menu-ocr.png'), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'Fixture' in words, 'Tray menu did not open below its icon: '+words
        ex, ey = (x0+x1)//2, (y0+y1)//2
        move(ex, ey); click(); time.sleep(.5)
        assert 'MenuEvent 1 clicked' in events(), 'Menu entry did not reach the app: '+events()
        # An item without Activate (AppIndicator apps such as Steam) opens its menu on left click instead.
        fixture.terminate(); fixture.wait(timeout=5); time.sleep(1)
        before = events()
        menu_only = dict(env, ISLAND_TRAY_MENU_ONLY='1')
        fixture = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_tray.py')], env=menu_only,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(2)
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        move(*square); click(); time.sleep(1.5)
        menu = Image.open(shot('tray-menu-only')).convert('RGB')
        region = menu.crop((x0, y0, x1, y1)).convert('L')
        region.resize((region.width*3, region.height*3), Image.LANCZOS).save(out/'tray-menu-only-ocr.png')
        words = run(['tesseract', str(out/'tray-menu-only-ocr.png'), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'Fixture' in words, 'Left click on a menu-only tray item did not open its menu: '+words
        assert 'Activate' not in events()[len(before):].replace('SecondaryActivate', ''), events()
        fixture.terminate(); fixture.wait(timeout=5)
        assert shell.poll() is None
        print(f'PASS: tray item at {square} in the expanded Island activates, opens its menu with the Island held '
              f'open, the menu entry reaches the app, and a menu-only item opens its menu on left click', flush=True)
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
