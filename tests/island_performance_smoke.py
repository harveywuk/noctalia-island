"""Artwork and widget work should stop when their Island is hidden or borrowed."""
import array
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import time
import wave

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    env['NOCTALIA_IDLE_PROFILE'] = '1'
    path = cfg/'config.toml'
    text = path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
    text = text.replace('[shell]\n', '[shell]\noffline_mode=true\n')
    text += '''
[bar.capsule]
presentation="island"
reserve_space=false
auto_hide=false
show_on_workspace_switch=false
[bar.capsule.island]
height=64
track_preview_seconds=0
reveal_on_track_change=false
hover_widgets=[]
hover_show_tray=true
media_gradient=true
[desktop_widgets]
enabled=true
[desktop_widgets.widget.perf_media]
type="media_player"
output="TEST-1"
cx=260.0
cy=440.0
placement_width=1280.0
placement_height=720.0
[desktop_widgets.widget.perf_media.settings]
card_size="medium"
'''
    path.write_text(text)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    original = (cfg/'config.toml').read_text()
    measure_only = os.environ.get('NOCTALIA_TEST_MEASURE_ONLY') == '1'
    results = []
    helpers = []
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

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.5)

    def media(method):
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.'+method])

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)])
        return Image.open(path).convert('RGB')

    def sample(label, quiet=False):
        # Ignore the first partial interval after changing state. The next report
        # contains ten uninterrupted seconds in this state, independently of FPS.
        time.sleep(1)
        log = out/'noctalia.log'
        pattern = r'idle profile ([\d.]+)s:.*'
        count = len(re.findall(pattern, log.read_text()))
        deadline = time.monotonic()+26
        while len(re.findall(pattern, log.read_text())) < count+2:
            assert time.monotonic() < deadline, 'Idle profiler did not report'
            assert shell.poll() is None
            time.sleep(.15)
        report = re.split(r'(?=idle profile [\d.]+s:)', log.read_text())[-1]
        cpu = re.search(r'cpu\(process/thread/bg\)=([\d.]+)/[\d.]+/[\d.]+ms ([\d.]+)/', report)
        surfaces = re.search(r'idle profile surfaces:.*?renders=(\d+) ([\d.]+)ms', report)
        timer = re.search(r'idle profile source \S*TimerPollSource: wake=(\d+)', report)
        assert cpu and surfaces and timer, report
        entry = {'phase': label, 'cpu_percent': float(cpu[2]), 'renders': int(surfaces[1]),
                 'render_ms': float(surfaces[2]), 'timer_wakes': int(timer[1])}
        results.append(entry)
        (out/'island-performance.json').write_text(json.dumps(results, indent=2))
        print(json.dumps(entry), flush=True)
        if quiet and not measure_only:
            assert entry['renders'] < 40, ('Hidden or stationary content kept rendering', entry)
            assert entry['timer_wakes'] < 150, ('Hidden artwork kept its animation timer alive', entry)
        return entry

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        sample('idle', quiet=True)
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Performance fixture'
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'tray.log')
        # Real sound exercises the waveform as well as the artwork. It stays on
        # the fixture's private PipeWire server and never reaches desktop audio.
        audio_path = base/'tone.wav'
        tone = array.array('h', (int(2500*math.sin(2*math.pi*440*i/48000)) for i in range(48000)))
        if sys.byteorder != 'little':
            tone.byteswap()
        with wave.open(str(audio_path), 'wb') as wav:
            wav.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            for _ in range(300):
                wav.writeframes(tone.tobytes())
        sound = start(['pw-play', str(audio_path)], 'sound.log')
        time.sleep(2)
        active = sample('playing-with-desktop-widget')
        assert active['renders'] > 100, 'Artwork fixture was not animating'
        before = shot('artwork-before'); time.sleep(.6); after = shot('artwork-after')
        assert ImageChops.difference(before.crop((515, 12, 590, 65)), after.crop((515, 12, 590, 65))).getbbox()

        # A rebuilt capsule must start its shader without requiring a view change.
        msg('config-reload'); time.sleep(1)
        before = shot('reloaded-flow-before'); time.sleep(.6); after = shot('reloaded-flow-after')
        if not measure_only:
            assert ImageChops.difference(before.crop((515, 12, 590, 65)),
                                         after.crop((515, 12, 590, 65))).getbbox(), 'Artwork missing after reload'

        hidden = original.replace('auto_hide=false', 'auto_hide=true')
        (cfg/'config.toml').write_text(hidden); msg('config-reload'); msg('desktop-widgets-hide')
        move(1100, 600); time.sleep(1)
        sample('auto-hidden-playing', quiet=True)
        hidden_image = shot('hidden')
        move(640, 1); time.sleep(1)
        shown = shot('revealed')
        assert ImageChops.difference(hidden_image.crop((480, 8, 800, 180)), shown.crop((480, 8, 800, 180))).getbbox()
        before = shot('revealed-flow-before'); time.sleep(.6); after = shot('revealed-flow-after')
        assert ImageChops.difference(before.crop((500, 16, 590, 30)), after.crop((500, 16, 590, 30))).getbbox(), (
            'Artwork did not resume on reveal')

        msg('panel-open', 'control-center', 'audio'); move(1100, 600)
        assert json.loads(msg('status'))['activePanelId'] == 'control-center'
        sample('control-centre-audio', quiet=True)
        msg('panel-close'); time.sleep(.8)
        move(1100, 600); move(640, 1); time.sleep(1)
        before = shot('returned-flow-before'); time.sleep(.6); after = shot('returned-flow-after')
        assert ImageChops.difference(before.crop((500, 16, 590, 30)), after.crop((500, 16, 590, 30))).getbbox(), (
            'Artwork did not resume after Control Centre')

        # Keyboard focus survives player updates, and Escape releases the grab.
        msg('island-focus'); time.sleep(.6)
        command(keyboard, 15)
        command(keyboard, 28); time.sleep(.5)
        assert (out/'player-events.txt').read_text().count('PlayPause') == 1, 'Keyboard media controls lost focus'
        # Pause changes the button's icon and rebuilds the card. Enter must still
        # reach that same playback button after the service update.
        command(keyboard, 28); time.sleep(.5)
        assert (out/'player-events.txt').read_text().count('PlayPause') == 2, 'Playback update lost keyboard focus'
        command(keyboard, 1); time.sleep(.5)
        assert not json.loads(msg('status'))['panelOpen']
        media('Pause'); sound.terminate(); sound.wait(timeout=5)
        move(1100, 600)
        sample('paused-hidden', quiet=True)

        (cfg/'config.toml').write_text(original+'\n[shell.animation]\nenabled=false\n')
        msg('config-reload'); msg('desktop-widgets-show'); media('Play'); move(1100, 600)
        sample('reduced-motion-with-desktop-widget', quiet=True)
        assert shell.poll() is None
        print('PASS: visible artwork animates, hidden and borrowed Islands settle, reveal and panel return '
              'resume the artwork, keyboard controls work, and paused/reduced-motion widgets rest', flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
