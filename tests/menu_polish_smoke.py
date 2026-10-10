"""Native menu scaling, tooltip dismissal, scrolling and nested tray menus."""
import json
import pathlib
import subprocess
import sys
import time

from PIL import Image
import ocr
from island_tray_smoke import prepare


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')), *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
    pointer, keyboard = helpers
    original = (cfg/'config.toml').read_text()
    output = 'TEST-1'

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'
    def key(value): command(keyboard, value); time.sleep(.3)
    def move(x, y, delay=.3):
        dispatch(f'hl.dsp.cursor.move({{x={int(x)},y={int(y)}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0'); time.sleep(delay)
    def click(button=''):
        prefix = button+'-' if button else ''
        command(pointer, prefix+'press'); time.sleep(.08); command(pointer, prefix+'release'); time.sleep(.5)
    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)]); return path
    def configure(scale):
        (cfg/'config.toml').write_text(original+f'\n[accessibility]\nui_scale={scale}\n')
        msg('config-reload'); time.sleep(1)
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        msg('color-scheme-set', 'community', 'macOS')
        for scale, mode, output, pixel_scale in ((1, 'dark', 'TEST-1', 1), (1.5, 'dark', 'TEST-1', 1),
                                                  (1.5, 'light', 'TEST-1', 1), (1.1, 'dark', 'TEST-2', 1.5)):
            msg('settings-close'); configure(scale); msg('theme-mode-set', mode)
            dispatch(f'hl.dsp.focus({{monitor="{output}"}})'); move(1100, 600 if output == 'TEST-1' else 1320)
            msg('settings-open', 'system'); time.sleep(1)
            window = next(c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'dev.noctalia.Noctalia')
            (out/f'settings-window-{scale}-{mode}.json').write_text(json.dumps(window))
            x, y = window['at']; w, h = window['size']
            move(x+w-66*scale, y+30*scale, 1)
            tip = shot(f'tooltip-{scale}-{mode}')
            assert ocr.find(tip, 'More actions'), f'Missing tooltip at {scale}, {mode}'
            click(); menu = shot(f'menu-{scale}-{mode}')
            advanced = ocr.find(menu, 'Show Advanced')
            changed = ocr.find(menu, 'Show Only Changed')
            assert advanced and changed, f'Missing menu labels at {scale}, {mode}'
            assert abs(changed[1]-advanced[1]-28*scale*pixel_scale) <= 3, (scale, advanced, changed)
            assert not ocr.find(menu, 'More actions'), 'Tooltip remained behind its menu'
            key(108); shot(f'menu-selected-{scale}-{mode}')
            key(1); assert not ocr.find(shot('dismissed'), 'Show Advanced'), 'Escape failed to dismiss menu'
            print(f'PASS: Settings menu and tooltip at {scale}x in {mode}', flush=True)
        output = 'TEST-1'
        dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('settings-close'); configure(1.5); msg('theme-mode-set', 'dark')
        events = base/'tray-events'; events.write_text('')
        env.update(ISLAND_TEST_EVENTS=str(events), ISLAND_TRAY_LONG_MENU='1')
        start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'tray.log'); time.sleep(2)
        move(1100, 600); move(580, 32, 1.5)
        picture = Image.open(shot('tray-expanded')).convert('RGB')
        square = None
        for y in range(15, 500):
            for x in range(340, 940):
                if all(min(picture.getpixel((x+dx,y+dy))) > 230 for dx in range(0, 12, 3) for dy in range(0, 12, 3)):
                    square = (x+6, y+6); break
            if square: break
        assert square, 'Missing tray fixture'
        move(*square); click('right'); time.sleep(.7)
        menu = shot('tray-long-menu')
        playback = ocr.find(menu, 'Playback')
        assert playback, 'Long tray menu did not open'
        move(*playback); click(); time.sleep(.4)
        nested = shot('tray-submenu')
        child = ocr.find(nested, 'Nested action')
        assert child, 'Nested menu did not open beside its row'
        assert abs(child[1]-playback[1]) < 20, (child, playback)
        move(*child); click()
        assert 'MenuEvent 101 clicked' in events.read_text(), events.read_text()
        move(580, 32, 1); move(*square); click('right'); time.sleep(.5)
        key(102); key(106); key(108)
        assert ocr.find(shot('tray-keyboard-submenu'), 'Nested action'), 'Right did not open submenu'
        key(105); key(107); end = shot('tray-scrolled-end')
        last = ocr.find(end, 'Action 35')
        assert last, 'Keyboard End failed to reveal the final entry'
        assert 12 < last[1] < 700, ('Final entry clipped at output edge', last)
        key(28); assert 'MenuEvent 35 clicked' in events.read_text(), events.read_text()
        move(580, 32, 1); move(*square); click('right'); time.sleep(.5)
        key(107)
        for _ in range(5): key(103)
        parent = ocr.find(shot('tray-scrolled-parent'), 'Action 30')
        key(106); key(108)
        child = ocr.find(shot('tray-scrolled-submenu'), 'Scrolled child')
        assert parent and child and abs(child[1]-parent[1]) <= 4, (parent, child)
        key(28); assert 'MenuEvent 102 clicked' in events.read_text(), events.read_text()
        print('PASS: long tray menu, nested selection and keyboard scrolling at 1.5x', flush=True)
        assert shell.poll() is None
    finally:
        for helper in helpers:
            helper.terminate(); helper.wait(timeout=5)
        msg('settings-close')
