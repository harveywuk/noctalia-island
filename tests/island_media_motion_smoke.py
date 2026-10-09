"""Follow native artwork through expansion, reversal and panel handoff on private outputs."""
import json
import pathlib
import subprocess
import sys
import time

from PIL import Image


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
track_preview_seconds=0
split_activities=false
media_gradient=true
outer_progress_ring=false''').replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    +'\n[shell.animation]\nenabled=true\nspeed=0.25\n')
    # A saturated test swatch distinguishes the cover from its dim artwork shader.
    art = base/'motion-art.png'
    Image.new('RGB', (160, 160), (255, 35, 190)).save(art)
    env['ISLAND_TEST_ART'] = art.as_uri()
    env['ISLAND_TEST_TITLE'] = 'Continuous artwork'
    env['ISLAND_TEST_EVENTS'] = str(base/'player-events.log')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    protocol = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(protocol), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(protocol), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    config = cfg/'config.toml'
    original = config.read_text()

    def move(inside, output='TEST-1'):
        y = (40 if inside else 600) + (720 if output == 'TEST-2' else 0)
        dispatch(f'hl.dsp.cursor.move({{x={640 if inside else 1100},y={y}}})')
        for value in ('relative 1 0', 'relative -1 0'):
            pointer.stdin.write(value+'\n'); pointer.stdin.flush()
            assert pointer.stdout.readline().strip() == 'ok'

    def bounds(name, output='TEST-1', waveform=False):
        path = out/(name+'.png')
        run(['grim', '-o', output, str(path)])
        im = Image.open(path).convert('RGB')
        if output == 'TEST-2':
            im = im.resize((1280, 720))
        # Exclude the waveform and any cover displayed inside a hosted panel.
        area = im.crop((300, 8, 600, 240))
        mask = Image.new('1', area.size)
        mask.putdata([r > 240 and 180 < b < 205 and 20 < g < 50 for r, g, b in area.getdata()])
        rect = mask.getbbox()
        assert rect is not None, f'Artwork disappeared: {name}'
        x, y, right, bottom = rect
        assert 30 <= right-x <= 70 and 30 <= bottom-y <= 70, (name, rect)
        assert .75 < (right-x)/(bottom-y) < 1.25, f'Artwork was clipped: {name}: {rect}'
        # One solid cover, including its rounded corners, rather than two overlapping copies.
        pixels = sum(bool(pixel) for pixel in mask.getdata())
        assert pixels > (right-x)*(bottom-y)*.75, f'Duplicate or faded cover: {name}'
        if waveform:
            center_y = (y+bottom)/2+8
            wave = im.crop((725, int(center_y-17), 895, int(center_y+18)))
            assert sum(r > 220 and g > 100 and b > 150 for r, g, b in wave.getdata()) >= 20, \
                f'Playing waveform disappeared: {name}'
        return (x+300, y+8, right-x, bottom-y)

    def capture(name, inside, output='TEST-1', reverse=False):
        frames = [bounds(name+'-before', output, waveform=True)]
        move(inside, output)
        began = time.monotonic()
        for i, offset in enumerate((.08, .18, .3, .45, .65, .9, 1.2, 1.6, 2.2, 3.0)):
            time.sleep(max(0, began+offset-time.monotonic()))
            if reverse and i == 3:
                move(not inside, output)
            frames.append(bounds(f'{name}-{i:02d}', output, waveform=True))
        (out/(name+'.json')).write_text(json.dumps(frames))
        assert len({rect[0] for rect in frames}) >= 4, f'Artwork did not move gradually: {name}'
        if reverse:
            assert abs(frames[-1][0]-frames[0][0]) <= 2, (name, frames)
        else:
            assert abs(frames[-1][0]-frames[0][0]) >= 25, (name, frames)
        return frames

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(False)
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'motion-player.log')
        time.sleep(3)
        for compact in (False, True):
            name = 'compact' if compact else 'comfortable'
            config.write_text(original.replace('track_preview_seconds=0',
                                               'compact_layout='+str(compact).lower()+'\ntrack_preview_seconds=0'))
            msg('config-reload'); move(False); time.sleep(3)
            capture(name+'-expand', True)
            capture(name+'-collapse', False)
            capture(name+'-reverse', True, reverse=True)
            # Exercise the independently retained waveform across player state changes.
            for method in ('Pause', 'Play'):
                run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest',
                     '--object-path', '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.'+method])
                time.sleep(.4); bounds(name+'-'+method)
            msg('panel-open', 'control-center', 'home'); time.sleep(3)
            msg('panel-close'); time.sleep(3); bounds(name+'-panel-return')

        # A notification interrupts the expanded card while the media shader continues underneath.
        move(True); time.sleep(.4)
        run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
             '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
             'Motion test', '0', '', 'Activity interruption', 'Keep the media backdrop.', '[]', '{}', '0'])
        time.sleep(1); move(False); msg('notification-clear-active'); msg('notification-clear-history')
        time.sleep(3); bounds('notification-return')

        # Reduced motion must immediately show the final layout without a stranded shared element.
        config.write_text(original.replace('enabled=true\nspeed=0.25', 'enabled=false'))
        msg('config-reload'); move(False); time.sleep(1)
        for inside in (True, False):
            move(inside); time.sleep(.4); bounds('reduced-'+str(inside))

        config.write_text(original); msg('config-reload'); time.sleep(3)
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(False, 'TEST-2'); time.sleep(3)
        capture('fractional-expand', True, 'TEST-2')
        capture('fractional-collapse', False, 'TEST-2')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: continuous media artwork in both densities, reversals, playback changes, panel return, '
              'notification interruption, reduced motion and fractional output', flush=True)
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
