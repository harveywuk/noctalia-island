"""Webcam privacy indicator for apps that open /dev/video* directly, outside PipeWire.

A fake /proc (NOCTALIA_PRIVACY_PROC_ROOT) lists a "camtest" process holding /dev/video0, and a
kitty window with that class stands in for the app. The compact Island must show the camera
icon, name the app on hover, and raise its window when clicked; so must the expanded Island.
With a microphone capture too, the compact Island keeps one indicator slot that alternates.
"""
import json
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
    path.write_text(text)
    proc = base/'fakeproc'/'4242'
    (proc/'fd').mkdir(parents=True)
    (proc/'comm').write_text('camtest\n')
    (proc/'cmdline').write_bytes(b'camtest\0')
    (proc/'exe').symlink_to('/usr/bin/camtest')
    (proc/'fd'/'3').symlink_to('/dev/video0')
    env['NOCTALIA_PRIVACY_PROC_ROOT'] = str(base/'fakeproc')


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

    def command(value):
        pointer.stdin.write(value+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'

    def move(x, y):
        # A warp alone is not a motion event; nudge so hover and focus-follows-mouse react.
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.3)
        command('relative 1 0'); command('relative -1 0'); time.sleep(.4)

    def click():
        command('press'); command('release'); time.sleep(.8)

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)]); return path

    def tooltip_words(name, x, y):
        # The tooltip sits just below the pointer; enlarge that strip so OCR reads small text.
        image = Image.open(shot(name)).convert('RGB')
        strip = image.crop((max(0, x-180), y+8, min(image.width, x+180), y+70))
        path = out/(name+'-tooltip.png')
        strip.resize((strip.width*3, strip.height*3), Image.LANCZOS).save(path)
        return run(['tesseract', str(path), 'stdout', '--tessdata-dir', tessdata, '--psm', '6'])

    def active_class():
        return json.loads(ctl('-j', 'activewindow')).get('class', '')

    def camera_icon(image, top, bottom, left, right):
        """Centre of the rightmost white glyph cluster between left and right."""
        columns = [x for x in range(left, right) if any(min(image.getpixel((x, y))) > 200 for y in range(top, bottom))]
        assert columns, 'Camera indicator missing'
        start_x = columns[-1]
        while start_x-1 in columns or start_x-2 in columns:
            start_x -= 1
        pixels = [(x, y) for x in range(start_x, columns[-1]+1) for y in range(top, bottom)
                  if min(image.getpixel((x, y))) > 200]
        return sum(p[0] for p in pixels)//len(pixels), sum(p[1] for p in pixels)//len(pixels)

    capture = None
    kitty = ['kitty', '--config', 'NONE', '-o', 'confirm_os_window_close=0']
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        start(kitty+['--class', 'camtest', '--title', 'camtest', '/bin/sh'], 'camtest.log')
        wait(lambda: 'camtest' in ctl('clients'), 'camtest window')
        start(kitty+['--class', 'other', '--title', 'other', '/bin/sh'], 'other.log')
        wait(lambda: 'class: other' in ctl('clients'), 'other window')
        time.sleep(4)  # Two scan periods, plus the capture preview.

        move(1000, 400); assert active_class() == 'other', active_class()
        compact = Image.open(shot('camera-compact')).convert('RGB')
        xs = [x for x in range(compact.width) if max(compact.getpixel((x, 20))) < 12]
        ys = [y for y in range(0, 200) if max(compact.getpixel((compact.width//2, y))) < 12]
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        cx, cy = camera_icon(compact, top, bottom, (left+right)//2, right)
        move(cx, cy); time.sleep(1)
        text = tooltip_words('camera-hover', cx, cy)
        assert 'Camera' in text and 'camtest' in text, 'Camera tooltip missing: '+text
        click()
        assert active_class() == 'camtest', 'Camera icon did not raise its app: '+active_class()

        # The expanded Island shows the same icon centred below its content.
        move(1000, 400); assert active_class() == 'other', active_class()
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        expanded = Image.open(shot('camera-expanded')).convert('RGB')
        # The icon row sits on the Island's black background below its first card: the first
        # bright glyph on the centre line whose row is otherwise black.
        mid = expanded.width//2
        rows = [y for y in range(top+60, 400)
                if max(expanded.getpixel((mid-40, y))) < 12
                and any(min(expanded.getpixel((x, y))) > 200 for x in range(mid-12, mid+12))]
        assert rows, 'Expanded camera indicator missing'
        end_y = rows[0]
        while end_y+1 in rows or end_y+2 in rows:
            end_y += 1
        ex, ey = mid, (rows[0]+end_y)//2
        move(ex, ey); time.sleep(1)
        text = tooltip_words('camera-expanded-hover', ex, ey)
        assert 'Camera' in text and 'camtest' in text, 'Expanded camera tooltip missing: '+text
        click()
        assert active_class() == 'camtest', 'Expanded camera icon did not raise its app: '+active_class()

        # Microphone and camera share the compact slot: one glyph at a time, alternating.
        move(1000, 400); time.sleep(1.5)
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=privacy-mic', 'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=privacy-mic', '--client-name=Privacy test'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(7)  # Past the capture preview.
        patterns = []
        for index in range(12):
            frame = Image.open(shot(f'camera-rotation-{index}')).convert('RGB')
            fl, fr, ft, fb = (lambda xs, ys: (min(xs), max(xs), min(ys), max(ys)))(
                [x for x in range(frame.width) if max(frame.getpixel((x, 20))) < 12],
                [y for y in range(0, 200) if max(frame.getpixel((frame.width//2, y))) < 12])
            assert fr-fl <= right-left+2, f'Indicators widened the Island ({fr-fl} > {right-left})'
            ix, iy = camera_icon(frame, ft, fb, (fl+fr)//2, fr)
            patterns.append(tuple(min(frame.getpixel((x, y))) > 200
                                  for x in range(ix-8, ix+8) for y in range(iy-8, iy+8)))
            time.sleep(1)
        assert len(set(patterns)) >= 2, 'Compact privacy indicator did not alternate'
        assert shell.poll() is None
        print('PASS: webcam held outside PipeWire shows, names and raises its app (compact and expanded); '
              'one compact slot alternates with the microphone', flush=True)
    finally:
        if capture is not None:
            capture.terminate(); capture.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
