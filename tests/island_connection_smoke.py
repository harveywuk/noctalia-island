"""Connection cards and confirmed audio output on private BlueZ, UPower and PipeWire."""
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops
from island_battery_glow_smoke import prepare as prepare_battery


def prepare(base, cfg, env):
    prepare_battery(base, cfg, env)
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('media_gradient=true', 'media_gradient=true\nvolume_show_percentage=true'))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, bluetooth, battery):
    repo = pathlib.Path(__file__).resolve().parents[1]
    original = (cfg/'config.toml').read_text()
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

    def properties(proc, **values):
        command(proc, json.dumps(values))

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.5)

    def connect(percentage=None, **values):
        properties(bluetooth, Connected=False); time.sleep(.15)
        began = time.monotonic()
        properties(bluetooth, **values, Connected=True)
        if percentage is not None:
            properties(bluetooth, Percentage=percentage)
        return began

    def at(began, seconds):
        time.sleep(max(0, began+seconds-time.monotonic()))

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def text(name, output='TEST-1', region=(435, 0, 845, 85), detail=False):
        image = shot(name, output)
        if output == 'TEST-2':
            image = image.resize((1280, 720))
        crop = image.crop(region)
        crop = crop.resize((crop.width*3, crop.height*3))
        words = []
        # The artwork can confuse bright title OCR; preserve muted detail text in the colour pass.
        samples = [('text', crop), ('labels', crop.convert('L').point(lambda p: 255 if p > 110 else 0))]
        if detail:
            # Keep the muted status line separate from the bright title and pointer cursor.
            samples.append(('detail', image.crop((540, 44, 780, 66)).resize((960, 88))))
        for suffix, sample in samples:
            path = out/(name+'-'+suffix+'.png'); sample.save(path)
            # A block pass also recognises centred detail lines that sparse OCR can split.
            for mode in (('7',) if suffix == 'detail' else ('11', '6') if suffix == 'labels' else ('11',)):
                words.append(run(['tesseract', str(path), 'stdout', '--tessdata-dir',
                                  os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                                  '--psm', mode, '-c', 'user_defined_dpi=288']).lower())
        return '\n'.join(words)

    def card(name, status='connected', percentage=None, title='test headphones', output='TEST-1'):
        words = text(name, output, detail=True)
        assert title in words and (status is None or status in words), (name, words)
        if percentage is not None:
            # At 12 px, OCR sometimes reads the percent glyph as "46" over the artwork.
            assert str(percentage) in words.replace(' ', ''), (name, words)
        return words

    def absent(name):
        words = text(name)
        assert all(value not in words for value in ('connected', 'audio output', 'test headphones', 'test controller')), (name, words)

    def artwork(name):
        region = (590, 14, 690, 20)
        first = shot(name+'-before').crop(region); time.sleep(.4)
        second = shot(name+'-after').crop(region)
        assert sum(max(pixel) > 12 for pixel in second.getdata()) > 300, 'Artwork went black'
        assert ImageChops.difference(first, second).getbbox(), 'Artwork stopped animating'

    def configure(seconds=5, monitor='all', reduced=False):
        (cfg/'config.toml').write_text(original.replace('bluetooth_preview_seconds=5',
                                       'bluetooth_preview_seconds='+str(seconds))
                                     .replace('bluetooth_preview_monitor="all"',
                                              'bluetooth_preview_monitor='+json.dumps(monitor))
                                     +'\n[shell.animation]\nenabled='+str(not reduced).lower()+'\n')
        msg('config-reload'); time.sleep(.6)

    def click():
        command(pointer, 'press'); time.sleep(.08); command(pointer, 'release')

    def opened(name, context, output='TEST-1'):
        wait(lambda: json.loads(msg('status'))['activePanelId'] == 'control-center', name)
        time.sleep(.6); shot(name, output)
        # Toggling the same context closes it; a wrong destination would switch tabs instead.
        msg('panel-toggle', 'control-center', context); time.sleep(.7)
        assert not json.loads(msg('status'))['panelOpen'], (name, 'Wrong Control Centre page')
        move()
        absent(name+'-consumed')
        assert 'connected' not in text(name+'-consumed-other-output', 'TEST-2')

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Connection fixture'
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        time.sleep(2)

        began = connect(); at(began, .7)
        words = card('unknown-battery')
        assert '%' not in words, 'Unknown battery was shown as a charge level'
        artwork('connection-artwork')
        properties(bluetooth, Percentage=60); time.sleep(.4)
        card('late-battery', percentage=60)
        at(began, 4.1); card('before-expiry', percentage=60)
        at(began, 5.7); absent('expired-at-original-deadline')
        properties(bluetooth, Percentage=61); time.sleep(.4)
        absent('battery-update-does-not-replay')

        # Two outputs have the same human label. Only the Bluetooth identity may confirm routing.
        for name, address in (('connection-wrong', '11:22:33:44:55:66'),
                              ('connection-headphones', 'AA:BB:CC:DD:EE:FF')):
            run(['pw-cli', 'create-node', 'adapter', json.dumps({
                'factory.name': 'support.null-audio-sink', 'node.name': name,
                'node.description': 'Test headphones', 'media.class': 'Audio/Sink',
                'object.linger': True, 'audio.position': ['FL', 'FR'], 'api.bluez5.address': address,
            })])
        wait(lambda: any(sink['name'] == 'connection-headphones'
                         for sink in json.loads(run(['pactl', '--format=json', 'list', 'sinks']))),
             'Private Bluetooth output fixtures did not appear')
        run(['pactl', 'set-default-sink', 'connection-wrong']); time.sleep(2.1)
        began = connect(75); at(began, .6)
        words = card('same-name-different-device', percentage=75)
        assert 'audio output' not in words
        run(['pactl', 'set-default-sink', 'connection-headphones']); time.sleep(.7)
        (out/'audio-nodes.json').write_text(run(['pw-dump']))
        card('confirmed-audio-output', status='audio output', percentage=75)
        artwork('audio-confirmation-artwork')
        at(began, 5.7); absent('audio-confirmation-does-not-extend-card')
        # Confirm the default naming fallback when no BlueZ property is exported.
        run(['pactl', 'load-module', 'module-null-sink', 'sink_name=bluez_output.AA_BB_CC_DD_EE_FF.1'])
        run(['pactl', 'set-default-sink', 'bluez_output.AA_BB_CC_DD_EE_FF.1']); time.sleep(2.1)
        began = connect(80); at(began, .6)
        card('node-identity-fallback', status='audio output', percentage=80)
        properties(bluetooth, Connected=False); time.sleep(.5)
        absent('disconnect-dismisses-card')
        run(['pactl', 'set-default-sink', 'hyprland-test']); time.sleep(2.1)

        began = connect(40); at(began, .5)
        msg('volume-osd', '65'); time.sleep(.4)
        assert '65' in text('volume-wins')
        at(began, 3); card('connection-returns-with-time-left', percentage=40)
        at(began, 5.7); absent('osd-does-not-extend-card')

        began = connect(75); at(began, .5)
        run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
             '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
             'Mail', '0', '', 'Priority notice', 'Connection cards must expire behind this alert.',
             '[]', "{'urgency': <byte 2>}", '0'])
        time.sleep(.5)
        assert 'priority notice' in text('notification-wins', region=(420, 0, 860, 250))
        at(began, 5.7)
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.5)
        absent('notification-does-not-replay')

        msg('panel-open', 'control-center', 'bluetooth'); time.sleep(.5)
        began = connect(50); at(began, .6)
        assert json.loads(msg('status'))['activePanelId'] == 'control-center'
        at(began, 5.7); msg('panel-close'); time.sleep(.6)
        absent('panel-does-not-replay')
        msg('island-focus'); time.sleep(.6)
        began = connect(50); at(began, .5)
        assert 'connection fixture' in text('keyboard-keeps-media', region=(420, 0, 860, 300))
        at(began, 5.7); command(keyboard, 1); move()
        absent('keyboard-does-not-replay')

        # Wired peripherals get the same compact card; system charging keeps its existing card.
        properties(bluetooth, Connected=False)
        properties(battery, add='mouse_USB', properties={'Model': 'USB mouse', 'Type': 5, 'IsPresent': True,
                                                        'Percentage': 42., 'State': 2})
        time.sleep(.6); card('wired-device', title='usb mouse', percentage=42)
        properties(battery, remove='mouse_USB'); time.sleep(.4)
        absent('wired-removal')

        # A real focused client verifies that expiry and replacement release the keyboard grab.
        keys = out/'connection-keys'
        recorder = "import os,sys,tty; tty.setraw(0)\nf=open(sys.argv[1],'ab',buffering=0)\nwhile True: f.write(os.read(0,1))"
        start(['kitty', '--config', 'NONE', '-o', 'confirm_os_window_close=0', '--class', 'connection-client',
               '-e', sys.executable, '-c', recorder, str(keys)], 'connection-client.log')
        wait(lambda: keys.exists(), 'Keyboard fixture did not start')

        def focus_client():
            move()
            window = next(item for item in json.loads(ctl('-j', 'clients')) if item['class'] == 'connection-client')
            dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
            wait(lambda: json.loads(ctl('-j', 'activewindow')).get('class') == 'connection-client', 'Client focus')

        def released(name):
            before = keys.stat().st_size
            command(keyboard, 57)
            wait(lambda: keys.stat().st_size > before, name+' retained the keyboard grab')
            assert not json.loads(msg('status'))['panelOpen'], name+' activated another card'

        focus_client(); began = connect(75); at(began, .6)
        move(500, 40); card('headphones-hover', status=None)
        artwork('headphones-hover-artwork')
        command(pointer, 'press'); time.sleep(.15); shot('headphones-pressed')
        command(pointer, 'release'); opened('headphones-sound-controls', 'audio')

        for key, name in ((28, 'enter'), (57, 'space')):
            focus_client(); began = connect(75); at(began, .6)
            msg('island-focus'); time.sleep(.4)
            command(keyboard, 15); command(keyboard, 'shift-tab')
            properties(bluetooth, Percentage=74); time.sleep(.3)
            card('focused-'+name, status=None)
            artwork('focused-'+name+'-artwork')
            command(keyboard, key); opened('keyboard-'+name+'-sound-controls', 'audio')

        focus_client(); began = connect(30, Alias='Test controller', Icon='input-gaming'); at(began, .6)
        move(500, 40); card('controller-hover', status=None, title='test controller')
        click(); opened('controller-bluetooth-controls', 'bluetooth')

        # Pointer hover and explicit keyboard selection both keep the original deadline.
        focus_client(); began = connect(75, Alias='Test headphones', Icon='audio-headphones'); at(began, .6)
        move(500, 40); at(began, 4.1); card('hover-before-expiry', status=None)
        at(began, 5.7); absent('hover-expired'); move()
        focus_client(); began = connect(75); at(began, .6)
        msg('island-focus'); time.sleep(.4)
        at(began, 5.7); released('focused-expiry'); absent('focused-expired')

        focus_client(); began = connect(75); at(began, .6)
        msg('island-focus'); time.sleep(.4); command(keyboard, 1); time.sleep(.3)
        released('escape')
        focus_client(); began = connect(75); at(began, .6)
        msg('island-focus'); time.sleep(.4)
        properties(bluetooth, Connected=False); time.sleep(.5); released('focused-disconnect')

        focus_client(); began = connect(75); at(began, .6)
        msg('island-focus'); time.sleep(.4)
        began = connect(30, Alias='Test controller', Icon='input-gaming'); at(began, .6)
        released('focused-replacement')
        card('replacement-remains-visible', status=None, title='test controller')
        # A fresh explicit selection can activate the replacement.
        msg('island-focus'); time.sleep(.4); command(keyboard, 28)
        opened('replacement-bluetooth-controls', 'bluetooth')

        focus_client(); began = connect(75, Alias='Test headphones', Icon='audio-headphones'); at(began, .6)
        move(500, 40); command(pointer, 'press')
        began = connect(30, Alias='Test controller', Icon='input-gaming'); at(began, .6)
        command(pointer, 'release'); time.sleep(.4)
        assert not json.loads(msg('status'))['panelOpen'], 'An old press activated the replacement'
        card('pressed-replacement', status=None, title='test controller'); move()

        configure(monitor='TEST-2', reduced=True)
        began = connect(20, Alias='Test controller', Icon='input-gaming'); at(began, .5)
        card('fractional-controller', title='test controller', percentage=20, output='TEST-2')
        absent('specific-monitor-only')
        move(640, 760); click(); opened('fractional-bluetooth-controls', 'bluetooth', 'TEST-2')
        at(began, 5.7)
        assert 'connected' not in text('fractional-expiry', 'TEST-2')
        configure(seconds=0)
        began = connect(75); at(began, .5)
        absent('preview-disabled')

        # The card is still keyboard-actionable when there is no media activity to expand.
        player.terminate(); player.wait(timeout=5); time.sleep(.5)
        configure(); focus_client()
        began = connect(30); at(began, .6)
        msg('island-focus'); time.sleep(.4)
        card('idle-focused-controller', status=None, title='test controller')
        command(keyboard, 28); opened('idle-bluetooth-controls', 'bluetooth')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: device identity, unknown/late battery, five-second lifetime, confirmed audio output, '
              'same-name mismatch, node identity fallback, media artwork, disconnect, OSD/notification/panel/'
              'keyboard priority, wired devices, pointer/keyboard card actions, hover, consumed previews, '
              'expiry/disconnect/replacement focus release, cancelled gestures, monitor targeting, '
              'fractional scale and disabled previews', flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
