"""Contextual expanded sections and live hover controls on a private GPU desktop."""
import csv
import io
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
track_preview_seconds=0
split_activities=false''').replace('[shell]\n', '[shell]\noffline_mode=true\n')
                   +'\n[shell.animation]\nenabled=false\n[control_center]\nhidden_tabs=["monitor","system"]\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
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
    config = cfg/'config.toml'
    original = config.read_text()

    def send(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        send(pointer, 'relative 1 0'); send(pointer, 'relative -1 0'); time.sleep(.35)

    def click(x, y):
        move(x, y); send(pointer, 'press'); time.sleep(.08); send(pointer, 'release'); time.sleep(.35)

    def key(value):
        send(keyboard, value); time.sleep(.3)

    def words(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        im = Image.open(path)
        factor = im.width/1280
        enlarged = out/(name+'-ocr.png'); im.resize((im.width*2, im.height*2)).save(enlarged)
        result = run(['tesseract', str(enlarged), 'stdout', '--tessdata-dir', tessdata,
                      '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        rows = csv.DictReader(io.StringIO(result[result.index('level\t'):]), delimiter='\t', quoting=csv.QUOTE_NONE)
        return [(r['text'].strip(), (int(r['left'])+int(r['width'])/2)/(2*factor),
                 (int(r['top'])+int(r['height'])/2)/(2*factor)) for r in rows if (r.get('text') or '').strip()]

    def text(name, output='TEST-1'):
        return ' '.join(word for word, x, y in words(name, output))

    def panel(section='home'):
        msg('panel-open', 'control-center', section); time.sleep(.5)

    def close():
        msg('panel-close'); move(1100, 600); time.sleep(.4)

    def status():
        return json.loads(msg('caffeine-status'))

    def hover():
        move(1100, 600); time.sleep(.4); move(640, 40); time.sleep(.5)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        msg('color-scheme-set', 'community', 'macOS')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'A little closer to home'
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.log')
        player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        time.sleep(.6)
        for compact in (False, True):
            name = 'compact' if compact else 'comfortable'
            config.write_text(original.replace('track_preview_seconds=0',
                                               'compact_layout='+str(compact).lower()+'\ntrack_preview_seconds=0')
                              .replace('[shell.animation]\nenabled=false',
                                       '[shell.animation]\nenabled='+str(compact).lower()))
            msg('config-reload'); time.sleep(.6)
            for mode in ('dark', 'light'):
                msg('theme-mode-set', mode); panel()
                title = text(name+'-'+mode+'-home')
                assert 'Control' in title and 'Center' in title, title
                click(440, 48)
                overview_words = words(name+'-'+mode+'-section-overview')
                menu = ' '.join(w for w, x, y in overview_words)
                assert all(s in menu for s in ('Playing', 'Audio', 'Privacy')), menu
                assert 'home' in menu, 'Live media was lost while choosing a section'
                assert 'Monitor' not in menu and 'System' not in menu, 'Hidden section leaked into the chooser'
                _, media_x, media_y = next(word for word in overview_words if 'Playing' in word[0])
                click(media_x, media_y)
                move(1100, 600); time.sleep(.5)
                title = text(name+'-'+mode+'-media')
                assert 'Now' in title and 'Playing' in title and 'home' in title, title
                assert title.count('Playing') == 1, 'Repeated Now Playing heading: '+title
                if not compact and mode == 'dark':
                    click(876, 48)
                    assert 'Island Test Player' in text('header-player-menu'), 'Header player selector did not open'
                    key(1)
                key('chord 2 15')
                assert 'Output' in text(name+'-'+mode+'-audio'), 'Keyboard section navigation failed'
                click(440, 48); key(1)
                assert json.loads(msg('status'))['panelOpen'], 'Escape dismissed the panel instead of returning from its overview'
                assert 'Output' in text(name+'-'+mode+'-overview-return'), 'Escape did not restore the previous view'
                close()
            # The title itself is keyboard reachable, with no persistent navigation strip.
            panel(); key(15); key(28)
            assert 'Playing' in text(name+'-keyboard-overview'), 'Keyboard title did not open section choices'
            key(106); key(108); key(103); key(28)
            assert 'home' in text(name+'-keyboard-selected-media'), 'Arrow/Enter did not select Now Playing'
            key(57)
            assert 'Privacy' in text(name+'-keyboard-reopen-overview'), 'Section selection did not restore title focus'
            key(1); close()

        # Metadata and playback changes keep updating while the overview owns the body.
        panel('audio'); click(440, 48)
        def player_method(method):
            run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest',
                 '--object-path', '/org/mpris/MediaPlayer2', '--method',
                 'org.mpris.MediaPlayer2.Player.'+method])
            time.sleep(.5)
        player_method('Next')
        assert 'Another orbit' in text('overview-track-change'), 'Overview metadata stopped updating'
        player_method('Pause')
        assert 'Another orbit' in text('overview-paused'), 'Paused activity disappeared'
        key(1); panel('monitor'); click(440, 48)
        text('overview-forced-hidden')
        # Read the coloured selection independently; sparse page OCR drops it.
        selected = out/'overview-selected-label.png'
        Image.open(out/'overview-forced-hidden.png').crop((588, 247, 700, 277)).resize((560, 150)).save(selected)
        selected_text = run(['tesseract', str(selected), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'Monitor' in selected_text, 'A directly opened hidden section lost its current entry'
        click(909, 48)
        assert json.loads(msg('status'))['panelOpen'], 'Overview Close dismissed the whole panel'
        close(); panel(); click(440, 48)
        player.terminate(); player.wait(timeout=5); time.sleep(.8)
        assert 'Another' not in text('overview-player-removed'), 'Departed player left stale media in the overview'
        close()
        env['ISLAND_TEST_TITLE'] = 'Longform recordings from the outer edge of the solar system, live at the observatory'
        env['ISLAND_TEST_ARTIST'] = 'The Midnight Observatory Ensemble with the Northern Lights Chamber Orchestra'
        player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'long-metadata-player.log')
        time.sleep(.8)
        for compact in (False, True):
            name = 'compact' if compact else 'comfortable'
            config.write_text(original.replace('track_preview_seconds=0',
                                               'compact_layout='+str(compact).lower()+'\ntrack_preview_seconds=0'))
            msg('config-reload'); time.sleep(.6)
            panel('media')
            assert 'Longform' in text(name+'-long-media'), 'Long media title lost its readable start'
            click(440, 48)
            assert 'Longform' in text(name+'-long-overview'), 'Long title displaced the overview content'
            key(1)
            assert 'Now' in text(name+'-long-return'), 'Long metadata blocked keyboard return'
            close(); hover(); text(name+'-long-hover'); close()
        player.terminate(); player.wait(timeout=5); time.sleep(.8)
        for compact in (False, True):
            name = 'compact' if compact else 'comfortable'
            config.write_text(original.replace('track_preview_seconds=0',
                                               'compact_layout='+str(compact).lower()+'\ntrack_preview_seconds=0'))
            msg('config-reload'); time.sleep(.6); msg('caffeine-for', '15'); hover()
            assert 'Keep Awake' in text(name+'-awake') and status()['enabled'], 'Timed activity did not open'
            before = status()['remaining_seconds']
            click(744 if compact else 791, 94 if compact else 101)
            assert 897 <= status()['remaining_seconds']-before <= 900, 'Add time did not extend the live deadline'
            click(790 if compact else 843, 94 if compact else 101)
            assert not status()['enabled'], 'End did not stop Keep Awake'

        # Real private capture verifies the hover card still controls the routed microphone.
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=refinement-mic', 'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=refinement-mic', '--client-name=VoiceRoom'], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        helpers.append(capture); time.sleep(2); hover()
        assert 'VoiceRoom' in text('microphone-hover'), 'Capturing app is missing'
        click(786, 161)
        def muted():
            return next(s['mute'] for s in json.loads(run(['pactl', '--format=json', 'list', 'sources']))
                        if s['name'] == 'refinement-mic')
        wait(muted, 'Hover microphone control did not mute the live device')
        text('microphone-muted'); click(786, 161); wait(lambda: not muted(), 'Unmute failed')
        click(560, 104)
        assert 'Output' in text('microphone-full-audio'), 'Capture summary did not open Audio'
        close(); capture.terminate(); capture.wait(timeout=5)
        # A fake /proc device handle exercises camera presentation without using host hardware.
        proc = pathlib.Path(env['NOCTALIA_PRIVACY_PROC_ROOT'])/'4242'
        (proc/'fd').mkdir(parents=True)
        (proc/'comm').write_text('CameraCall\n'); (proc/'cmdline').write_bytes(b'CameraCall\0')
        (proc/'exe').symlink_to('/usr/bin/CameraCall'); (proc/'fd/3').symlink_to('/dev/video0')
        time.sleep(4.2); hover()
        camera = text('camera-hover')
        assert 'CameraCall' in camera and 'Active' in camera, camera
        click(747, 95)
        assert 'window' in text('camera-no-window'), 'Background app feedback was lost'
        (proc/'fd/3').unlink(); time.sleep(3)
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(1100, 1320); panel('privacy')
        assert 'Privacy' in text('fractional-privacy', 'TEST-2')
        click(440, 768)
        assert 'Playing' in text('fractional-overview', 'TEST-2'), 'Overview failed on a fractional output'
        msg('panel-close')
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        config.write_text(original.replace('[island]\nenabled=true', '[island]\nenabled=false'))
        msg('config-reload'); time.sleep(.6); panel()
        assert 'Home' in text('standalone-control-center'), 'Standalone Control Centre lost its normal header'
        msg('panel-close')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: contextual expanded headers, in-place overview, hidden sections, keyboard, both densities and themes, '
              'normal and reduced motion, live media updates, Keep Awake controls, live microphone controls, '
              'fractional overview and standalone panel', flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
