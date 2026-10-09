"""All native Island cards, shared panel transitions and controls on private outputs."""
import csv
import io
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

from PIL import Image, ImageChops, ImageFilter


def prepare(base, cfg, env):
    repo = pathlib.Path(__file__).resolve().parents[1]
    plugin = base/'plugins/timer'
    plugin.mkdir(parents=True)
    (plugin/'plugin.toml').write_text('id="noctalia/timer"\nname="Island style test timer"\nversion="1.0.0"\nplugin_api=3\n[[service]]\nid="timer"\nentry="timer.luau"\n')
    (plugin/'timer.luau').write_text((repo/'tests/fixtures/island_progress_timer.luau').read_text())
    config = (cfg/'config.toml').read_text().split('[plugins]')[0]
    config = config.replace('[shell]\n', '[shell]\noffline_mode=true\ncorner_radius_scale=1\n')
    config = config.replace('[island]\nenabled=true\nhover_widgets=["workspaces","taskbar"]', '[island]\nenabled=false')
    config += '''
[shell.animation]
enabled=false
[bar]
order=["capsule","default"]
[bar.capsule]
presentation="island"
reserve_space=false
[bar.capsule.island]
hover_widgets=["workspaces"]
hover_widgets_center=["volume"]
hover_widgets_right=["notifications"]
track_preview_seconds=0
bluetooth_preview_seconds=5
[bar.capsule.monitor.TEST-2]
scale=1.25
'''
    config += '\n[plugins]\nauto_update="none"\nenabled=["noctalia/timer"]\n[[plugins.source]]\nname="test"\nkind="path"\nlocation='+json.dumps(str(plugin.parent))+'\nenabled=true\n'
    (cfg/'config.toml').write_text(config)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, bluetooth, battery):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
             *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
    pointer, keyboard = helpers
    publisher = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_downloads.py')], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    helpers.append(publisher)
    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'
    def properties(proc, **values):
        command(proc, json.dumps(values)); time.sleep(.3)
    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0'); time.sleep(.6)
    def click(x, y):
        move(x, y); command(pointer, 'press'); time.sleep(.08); command(pointer, 'release'); time.sleep(.3)
    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)]); return path
    def hover(): move(640, 40)
    def leave(): move(1100, 600)
    # Plugin countdowns are sampled by the Island's one-second clock tick.
    def timer(state='IDLE'): msg('plugin', 'noctalia/timer:timer', 'all', state, '75'); time.sleep(1.1)
    def download(visible):
        command(publisher, json.dumps({'uri': 'application://island-style-download.desktop',
                                      'properties': {'progress': .37, 'progress-visible': visible}})); time.sleep(.3)
    def text(name='ocr'):
        data = run(['tesseract', str(shot(name)), 'stdout', '--tessdata-dir',
                    os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                    '--psm', '11', '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=96'])
        return list(csv.DictReader(io.StringIO(data), delimiter='\t', quoting=csv.QUOTE_NONE))
    def tab(label):
        # A single title cycles live activities; inactive panels have no launcher here.
        expected = 'Now Playing' if label == 'Media' else label
        for _ in range(4):
            hover()
            heading = ' '.join(w.get('text') or '' for w in text('title-switch')
                               if int(w.get('top') or 0) < 65)
            if expected.lower() in heading.lower():
                return
            click(455, 42)
        raise AssertionError('Contextual title did not reach '+expected+': '+heading)
    def events(): return (out/'player-actions.log').read_text() if (out/'player-actions.log').exists() else ''
    env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_EVENTS'] = str(out/'player-actions.log')
    env['ISLAND_TEST_TITLE'] = 'A little closer to home'
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); leave()
        msg('color-scheme-set', 'community', 'macOS')
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=island-style-mic', 'master=hyprland-test.monitor'])
        for mode in ('dark', 'light'):
            msg('theme-mode-set', mode); time.sleep(.6)
            timer(); download(False); properties(bluetooth, Connected=False); properties(battery, IsPresent=False)
            leave(); shot(mode+'-rest'); hover(); shot(mode+'-calendar-widgets'); leave()

            properties(bluetooth, Connected=True, Percentage=75)
            began = time.monotonic(); shot(mode+'-bluetooth-connected')
            time.sleep(max(0, began+5.4-time.monotonic())); shot(mode+'-bluetooth-expired')
            hover(); shot(mode+'-bluetooth-hover')
            properties(battery, IsPresent=True, Type=2, State=1, Percentage=45.0, Serial='island-style-laptop')
            shot(mode+'-charging-batteries')
            leave(); properties(bluetooth, Percentage=5); shot(mode+'-low-battery')

            capture = subprocess.Popen(['parec', '--device=island-style-mic', '--client-name=Island privacy test',
                                        '--stream-name=Island capture'], env=env, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.DEVNULL)
            helpers.append(capture); time.sleep(.7)
            # The expanded Island shows capture as a clickable icon; the preview names it.
            words = ' '.join(w.get('text') or '' for w in text(mode+'-privacy-preview')).lower()
            assert 'icrophone' in words, 'Capture indicator missing: '+words
            time.sleep(1.5); hover(); shot(mode+'-privacy')
            capture.terminate(); capture.wait(timeout=5)

            leave(); player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], mode+'-player.log')
            time.sleep(.8); hover()
            # Capture ending briefly owns the Island with a "Microphone Stopped" OSD.
            # Wait for the actual media controls before exercising their hit targets.
            wait(lambda: any('home' in (word.get('text') or '').lower()
                             for word in text(mode+'-media-ready')), 'Media controls did not appear')
            leave(); shot(mode+'-media-compact'); hover()
            wait(lambda: any('home' in (word.get('text') or '').lower()
                             for word in text(mode+'-media-expanded')), 'Media did not expand again after leaving')
            before = events().count('PlayPause')
            click(640, 233)
            assert events().count('PlayPause') == before+1, 'Styled playback button did not activate'
            click(640, 233); click(700, 176)
            assert 'SetPosition' in events(), 'Styled seek bar did not seek'
            download(True); timer('PAUSED'); tab('Timers'); shot(mode+'-timers')
            tab('Downloads'); shot(mode+'-downloads'); tab('Media'); shot(mode+'-activities')
            msg('island-focus'); time.sleep(.3)
            for _ in range(3):
                command(keyboard, 15); time.sleep(.1)
            time.sleep(.2)
            shot(mode+'-keyboard-focus'); command(keyboard, 28); time.sleep(.3)
            assert any('style-download' in (word.get('text') or '') for word in text(mode+'-keyboard-title-enter')), 'Enter did not switch the focused title'
            command(keyboard, 57); time.sleep(.3)
            assert any('1:15' in (word.get('text') or '') for word in text(mode+'-keyboard-title-space')), 'Space did not preserve title focus and switch again'
            command(keyboard, 1)
            # Density is a presentation preference and keeps active activities intact.
            config = cfg/'config.toml'
            config.write_text(config.read_text().replace('track_preview_seconds=0',
                                                         'compact_layout=true\ntrack_preview_seconds=0'))
            msg('config-reload'); time.sleep(.6); leave(); hover(); shot(mode+'-compact-hover')
            msg('panel-open', 'control-center', 'home'); time.sleep(.5); shot(mode+'-compact-panel')
            msg('panel-close'); msg('capture-menu'); time.sleep(.3); shot(mode+'-compact-capture-menu')
            command(keyboard, 106); time.sleep(.3)
            assert any('Audio' in (word.get('text') or '') for word in text(mode+'-compact-record-options')), 'Keyboard recording selection did not show audio options'
            command(keyboard, 1)
            config.write_text(config.read_text().replace('compact_layout=true\n', ''))
            msg('config-reload'); time.sleep(.6)
            player.terminate(); player.wait(timeout=5); download(False); timer(); leave()
            properties(bluetooth, Connected=False); properties(battery, IsPresent=False)

            msg('volume-osd', '65'); time.sleep(.2); shot(mode+'-volume'); time.sleep(1.5)
            run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
                 '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
                 'Calendar', '0', '', 'Design review', 'Your shell and Island now share the same appearance.',
                 "['default', 'Open', 'later', 'Later']", "{'urgency': <byte 2>}", '0'])
            time.sleep(.5); msg('island-focus'); time.sleep(.3); shot(mode+'-notification-actions')
            command(keyboard, 1); msg('notification-clear-active'); hover(); shot(mode+'-unread-history')
            msg('notification-clear-history'); leave()
            msg('record-region'); time.sleep(.5)
            move(100, 250); command(pointer, 'press'); move(500, 500); command(pointer, 'release')
            wait(lambda: msg('record-status').startswith('REC'), 'Private recording did not start')
            leave(); time.sleep(1.1); shot(mode+'-recording'); hover(); shot(mode+'-recording-hover'); msg('record-stop')
            wait(lambda: msg('record-status') == 'idle', 'Private recording did not stop')
            msg('notification-clear-active'); msg('notification-clear-history')
            msg('capture-menu'); time.sleep(.3); shot(mode+'-capture-menu'); command(keyboard, 1)
            msg('panel-open', 'control-center', 'home'); time.sleep(.5); shot(mode+'-hosted-panel')
            msg('panel-close'); time.sleep(.5); shot(mode+'-panel-return')

        # A panel opened from the hover view hands back a compact capsule; only the compact view
        # may appear in it, not the wide hover view crossfading out (a second, clipped clock).
        config = cfg/'config.toml'
        config.write_text(config.read_text().replace('[shell.animation]\nenabled=false',
                                                     '[shell.animation]\nenabled=true\nspeed=0.5'))
        msg('config-reload'); time.sleep(.6); leave(); time.sleep(1); hover(); time.sleep(1)
        click(640, 30); time.sleep(1)
        frames = []
        def record():
            for i in range(150):
                frames.append(out/f'panel-return-{i:02d}.png')
                run(['grim', '-g', '440,0 400x90', str(frames[-1])])
        move(1100, 600); recorder = threading.Thread(target=record); recorder.start()
        command(pointer, 'press'); time.sleep(.08); command(pointer, 'release'); recorder.join(); time.sleep(.5)
        def bright(path):
            return Image.open(path).convert('L').point(lambda v: 255 if v > 170 else 0)
        settled = bright(frames[-1]).filter(ImageFilter.MaxFilter(7))
        emptied = False
        for path in frames:
            mask = bright(path)
            if not emptied:
                emptied = mask.getbbox() is None
                continue
            stray = ImageChops.subtract(mask, settled).histogram()[255]
            assert stray < 20, f'Content other than the compact view after the panel returned: {path}'
        assert emptied, 'The returning panel never showed an empty capsule'
        config.write_text(config.read_text().replace('[shell.animation]\nenabled=true\nspeed=0.5',
                                                     '[shell.animation]\nenabled=false'))
        msg('config-reload'); time.sleep(.6)

        # The shared corner preference applies to both the ring and borrowed panel surface.
        config.write_text(config.read_text().replace('corner_radius_scale=1', 'corner_radius_scale=0.5')
                          .replace('track_preview_seconds=0', 'outer_progress_ring=true\ntrack_preview_seconds=0'))
        msg('config-reload'); time.sleep(.6); timer('PAUSED'); leave(); shot('scaled-corners-ring')
        msg('panel-open', 'control-center', 'audio'); time.sleep(.5); shot('scaled-corners-panel'); msg('panel-close')
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(640, 760); shot('fractional-monitor-hover', 'TEST-2')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: light/dark Island cards, calendar, widgets, batteries, privacy, media controls and seeking, '
              'contextual title switching, notifications, OSD, keyboard, panel return (also from the hover view), corner scale '
              'and fractional monitor', flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
