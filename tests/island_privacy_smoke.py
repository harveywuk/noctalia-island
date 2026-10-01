"""Microphone capture indicator on a tall standalone Island at fractional UI scale.

Mirrors a 64 px Island at ui_scale 1.1 (NOCTALIA_TEST_UI_SCALE overrides): the compact capsule,
hovering the capsule itself, and the expanded privacy card below playing media, which must
stay inside the Island's lower corners.
"""
import os
import pathlib
import subprocess
import time


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace(
        '[island]\nenabled=true\n',
        '[island]\nenabled=true\nheight=64\nclock_size=24\nreserve_space=true\n', 1)
    text = text.replace('[shell]\n', '[shell]\noffline_mode=true\n', 1)
    text += '\n[accessibility]\nui_scale='+os.environ.get('NOCTALIA_TEST_UI_SCALE', '1.1')+'\n'
    path.write_text(text)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    from PIL import Image

    # Without a pointer device the seat has no wl_pointer, so warps never reach the Island.
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.6)

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)]); return path

    def island_box(image):
        """Bounding box of the black Island capsule along the top edge."""
        xs = [x for x in range(image.width) if max(image.getpixel((x, 20))) < 12]
        ys = [y for y in range(0, 200) if max(image.getpixel((image.width//2, y))) < 12]
        return min(xs), max(xs), min(ys), max(ys)

    def white_columns(image, x0, x1, y0, y1):
        return [x for x in range(x0, x1) if any(min(image.getpixel((x, y))) > 200 for y in range(y0, y1))]

    capture = None
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=privacy-mic', 'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=privacy-mic', '--client-name=Privacy test',
                                    '--stream-name=Privacy capture'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(.7); shot('privacy-preview')
        time.sleep(5); compact = Image.open(shot('privacy-compact')).convert('RGB')
        left, right, top, bottom = island_box(compact)
        columns = white_columns(compact, (left+right)//2, right, top, bottom)
        assert columns, 'Compact microphone indicator missing'
        start_x = columns[-1]
        while start_x-1 in columns or start_x-2 in columns:
            start_x -= 1
        mic = [(x, y) for x in range(start_x, columns[-1]+1) for y in range(top, bottom)
               if min(compact.getpixel((x, y))) > 200]
        cx = sum(p[0] for p in mic)//len(mic); cy = sum(p[1] for p in mic)//len(mic)
        move(cx, cy); time.sleep(.5); shot('privacy-capsule-hover')
        move(1100, 600); time.sleep(1)
        # Media playing during a call puts the privacy card last in the expanded Island.
        import sys
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log'); time.sleep(1.5)
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        expanded = Image.open(shot('privacy-expanded')).convert('RGB')
        # The last card must clear the Island's large lower corners: keep the bottom margin at
        # least the 12 px side margin rather than letting the card meet the curve.
        column = [max(expanded.getpixel((expanded.width//2, y))) for y in range(0, 500)]
        island_bottom = max(y for y, v in enumerate(column) if v < 45)
        card_bottom = max(y for y, v in enumerate(column) if 25 <= v < 45)
        assert island_bottom - card_bottom >= 10, f'Last card clips the Island ({island_bottom - card_bottom}px margin)'
        # The card's audio button is the first white glyph in the left gutter below the calendar.
        eleft = min(x for x in range(expanded.width) if max(expanded.getpixel((x, 30))) < 12)
        rows = [y for y in range(140, 400) if any(min(expanded.getpixel((x, y))) > 200 for x in range(eleft+20, eleft+60))]
        assert rows, 'Expanded microphone button missing'
        end_y = rows[0]
        while end_y+1 in rows or end_y+2 in rows:
            end_y += 1
        move(eleft+38, (rows[0]+end_y)//2); time.sleep(1.2); shot('privacy-button-hover')
        assert shell.poll() is None
        print(f'PASS: privacy capsule at ({cx},{cy}); see privacy-*.png', flush=True)
    finally:
        if capture is not None:
            capture.terminate(); capture.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
