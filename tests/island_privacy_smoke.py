"""Microphone capture indicator on a tall standalone Island at fractional UI scale.

Mirrors a 64 px Island at ui_scale 1.1 (NOCTALIA_TEST_UI_SCALE overrides): the compact capsule,
hovering the capsule itself, and the expanded Island's clickable microphone icon below playing
media, whose tooltip names the capturing app.
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
    pointer = None

    # Without a pointer device the seat has no wl_pointer, so warps never reach the Island.
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.3)
        if pointer is not None:
            # A warp alone is not a motion event; nudge so the hovered control sees the pointer.
            for step in ('relative 1 0', 'relative -1 0'):
                pointer.stdin.write(step+'\n'); pointer.stdin.flush()
                assert pointer.stdout.readline().strip() == 'ok'
        time.sleep(.4)

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
        # The expanded Island shows capture indicators as a centred row of icons below its
        # content; the microphone is the lowest white glyph on the centre line.
        column = [max(expanded.getpixel((expanded.width//2, y))) for y in range(0, 500)]
        island_bottom = max(y for y, v in enumerate(column) if v < 45)
        rows = [y for y in range(island_bottom-60, island_bottom)
                if any(min(expanded.getpixel((x, y))) > 200 for x in range(expanded.width//2-8, expanded.width//2+8))]
        assert rows, 'Expanded microphone icon missing'
        assert island_bottom - rows[-1] >= 10, f'Microphone icon clips the Island ({island_bottom - rows[-1]}px)'
        move(expanded.width//2, (rows[0]+rows[-1])//2); time.sleep(1.2)
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        words = run(['tesseract', str(shot('privacy-icon-hover')), 'stdout', '--tessdata-dir', tessdata, '--psm', '11'])
        assert 'Microphone' in words and 'Privacy' in words, 'Microphone tooltip missing: '+words
        for step in ('press', 'release'):
            pointer.stdin.write(step+'\n'); pointer.stdin.flush()
            assert pointer.stdout.readline().strip() == 'ok'
        time.sleep(1.2)
        words = run(['tesseract', str(shot('privacy-icon-click')), 'stdout', '--tessdata-dir', tessdata, '--psm', '11'])
        assert 'Output' in words, 'Microphone icon did not open the audio panel: '+words
        assert shell.poll() is None
        print(f'PASS: privacy capsule at ({cx},{cy}); see privacy-*.png', flush=True)
    finally:
        if capture is not None:
            capture.terminate(); capture.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
