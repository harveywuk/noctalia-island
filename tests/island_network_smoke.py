"""Network cards driven by private NetworkManager, PipeWire and compositor fixtures."""
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
media_gradient=true
track_preview_seconds=0
network_preview_seconds=5
network_preview_monitor="all"
volume_show_percentage=true
split_activities=false''').replace('[shell]\n', '[shell]\noffline_mode=true\n'))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, network):
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

    def change(**values):
        began = time.monotonic()
        command(network, json.dumps(values))
        return began

    def at(began, seconds):
        time.sleep(max(0, began+seconds-time.monotonic()))

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.4)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        image = Image.open(path).convert('RGB')
        return image.resize((1280,720)) if output == 'TEST-2' else image

    def text(name, output='TEST-1', region=(430,0,850,85)):
        image = shot(name, output)
        crop = image.crop(region); crop = crop.resize((crop.width*3,crop.height*3))
        detail = image.crop((540,44,790,66)).resize((1500,132))
        samples = [('text', crop, '11'), ('labels', crop.convert('L').point(lambda p:255 if p>110 else 0), '6'),
                   ('detail', detail, '7'),
                   ('detail-labels', detail.convert('L').point(lambda p:255 if p>90 else 0), '7')]
        words = []
        for suffix, sample, mode in samples:
            path = out/(name+'-'+suffix+'.png'); sample.save(path)
            words.append(run(['tesseract',str(path),'stdout','--tessdata-dir',
                              os.environ.get('NOCTALIA_TEST_TESSDATA',str(repo/'build-rishot/test-data/tessdata')),
                              '--psm',mode,'-c','user_defined_dpi=288']).lower())
        return ''.join(' '.join(words).split())

    def card(name, title, detail='', output='TEST-1'):
        words = text(name, output)
        assert ''.join(title.lower().split()) in words, (name, words)
        assert ''.join(detail.lower().split()) in words, (name, words)

    def absent(name, output='TEST-1'):
        words = text(name, output)
        assert all(value not in words for value in ('connectedto','ethernetconnected','connectionlost','connectionrestored')), (name, words)

    def artwork(name):
        region = (590,14,690,20)
        first = shot(name+'-before').crop(region); time.sleep(.4)
        second = shot(name+'-after').crop(region)
        assert sum(max(pixel)>12 for pixel in second.getdata())>300, 'Network card hid the artwork'
        assert ImageChops.difference(first,second).getbbox(), 'Network card froze the artwork'

    def opened(name, output='TEST-1'):
        wait(lambda:json.loads(msg('status'))['activePanelId']=='control-center', 'Network controls did not open')
        time.sleep(.6); shot(name, output)
        msg('panel-toggle','control-center','network'); time.sleep(.7)
        assert not json.loads(msg('status'))['panelOpen'], 'Card opened the wrong Control Centre tab'
        move(); absent(name+'-consumed'); absent(name+'-consumed-other-output','TEST-2')

    def configure(seconds=5, monitor='all', reduced=False):
        (cfg/'config.toml').write_text(original.replace('network_preview_seconds=5','network_preview_seconds='+str(seconds))
                                      .replace('network_preview_monitor="all"','network_preview_monitor='+json.dumps(monitor))
                                      +'\n[shell.animation]\nenabled='+str(not reduced).lower()+'\n')
        msg('config-reload'); time.sleep(.6)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Network fixture'
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'player.log')
        time.sleep(2); absent('quiet-startup')
        change(strength=30,frequency=2412); time.sleep(1.2); absent('signal-and-band-stay-quiet')
        change(connected=False); time.sleep(1.2); absent('short-dropout')
        change(connected=True); time.sleep(1.4); absent('short-reconnect')

        began = change(ssid='Studio'); at(began,.35); absent('settling')
        at(began,1.2); card('wifi-connected','Connected to','Studio'); artwork('wifi-artwork')
        change(strength=85); at(began,5.1); card('before-original-expiry','Connected to','Studio')
        at(began,6.4); absent('expired-without-extension')

        began = change(connected=False); at(began,2); absent('loss-debounce')
        at(began,3.2); card('connection-lost','Connection lost','Studio'); artwork('lost-artwork')
        began = change(connected=True); at(began,.35); absent('stale-loss-removed')
        at(began,1.2); card('connection-restored','Connection restored','Studio')
        move(500,40); shot('restore-hover')
        command(pointer,'press'); time.sleep(.15); shot('restore-pressed'); command(pointer,'release')
        opened('pointer-network-controls')

        # A real client proves that expired/replaced cards release keyboard input.
        keys = out/'client-keys'
        recorder = "import os,sys,tty; tty.setraw(0)\nf=open(sys.argv[1],'ab',buffering=0)\nwhile True: f.write(os.read(0,1))"
        start(['kitty','--config','NONE','-o','confirm_os_window_close=0','--class','network-client',
               '-e',sys.executable,'-c',recorder,str(keys)],'client.log')
        wait(lambda:keys.exists(),'Keyboard fixture')

        def focus_client():
            move()
            window = next(item for item in json.loads(ctl('-j','clients')) if item['class']=='network-client')
            dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
            wait(lambda:json.loads(ctl('-j','activewindow')).get('class')=='network-client','Client focus')

        def released(name):
            before = keys.stat().st_size; command(keyboard,57)
            wait(lambda:keys.stat().st_size>before,name+' trapped keyboard input')
            assert not json.loads(msg('status'))['panelOpen'],name+' opened a replacement'

        focus_client(); began = change(kind='wired'); at(began,1.2)
        card('ethernet-connected','Ethernet connected')
        msg('island-focus'); time.sleep(.4); command(keyboard,15); command(keyboard,'shift-tab')
        change(strength=60); time.sleep(.3); shot('ethernet-keyboard-focus'); artwork('focused-artwork')
        command(keyboard,28); opened('enter-network-controls')
        focus_client(); began = change(kind='wifi',ssid='Desk'); at(began,1.2)
        msg('island-focus'); time.sleep(.4); command(keyboard,57); opened('space-network-controls')

        focus_client(); began = change(ssid='Office'); at(began,1.2)
        msg('island-focus'); time.sleep(.4); command(keyboard,1); time.sleep(.3); released('escape')
        focus_client(); began = change(ssid='Study'); at(began,1.2)
        msg('island-focus'); time.sleep(.4); at(began,6.5); released('expiry'); absent('focused-expired')
        focus_client(); began = change(ssid='Lounge'); at(began,1.2)
        msg('island-focus'); time.sleep(.4)
        began = change(kind='wired'); at(began,1.2); released('replacement')
        card('replacement-card','Ethernet connected')
        msg('island-focus'); time.sleep(.4); command(keyboard,28); opened('replacement-controls')

        began = change(kind='wifi',ssid='Workshop'); at(began,1.2)
        msg('volume-osd','65'); time.sleep(.4)
        assert '65' in text('volume-priority',region=(735,30,780,52)); artwork('volume-background')
        at(began,3.6); card('card-after-volume','Connected to','Workshop')
        at(began,6.4); absent('volume-did-not-extend')

        began = change(ssid='Meeting'); at(began,1.2)
        run(['gdbus','call','--session','--dest','org.freedesktop.Notifications','--object-path',
             '/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.Notify',
             'Mail','0','','Priority notice','Network cards expire behind this alert.','[]',"{'urgency': <byte 2>}",'0'])
        time.sleep(.5)
        assert 'prioritynotice' in text('notification-priority',region=(420,0,860,250))
        at(began,6.4); msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.5)
        absent('notification-does-not-replay')

        msg('panel-open','control-center','network'); time.sleep(.4)
        began = change(ssid='Kitchen'); at(began,1.2)
        assert json.loads(msg('status'))['activePanelId']=='control-center'
        at(began,6.4); msg('panel-close'); time.sleep(.7); move(); absent('panel-does-not-replay')
        msg('island-focus'); time.sleep(.4)
        began = change(ssid='Hall'); at(began,1.2)
        assert 'networkfixture' in text('keyboard-keeps-media',region=(420,0,860,300))
        at(began,6.4); command(keyboard,1); move(); absent('keyboard-does-not-replay')

        configure(monitor='TEST-2',reduced=True); msg('theme-mode-set','light')
        began = change(ssid='Garden'); at(began,1.2)
        absent('specific-monitor'); card('fractional-network','Connected to','Garden','TEST-2')
        move(500,760); command(pointer,'press'); command(pointer,'release'); opened('fractional-controls','TEST-2')
        configure(seconds=0); began = change(kind='wired'); at(began,1.3); absent('disabled-preview')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: quiet startup/metadata, connection/loss debounce, sustained loss and restoration, Wi-Fi/Ethernet '
              'handover, original expiry, live artwork, pointer and keyboard actions, focus release, priorities, '
              'monitor targeting, fractional scale, reduced motion and disabled previews',flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
