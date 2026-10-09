"""Capture menu, delayed fresh frames and audio selection on private GPU outputs."""
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops

REPO = pathlib.Path(__file__).resolve().parents[1]
TESSDATA = os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
                    .replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    .replace('[osd.kinds]\n', '[osd.kinds]\nkeyboard_layout=false\n')
                    .replace('[shell.screenshot]\n', '[shell.screenshot]\nannotate=false\ncopy_to_clipboard=false\nfreeze_screen=true\n')+'''
[bar.live]
presentation="island"
reserve_space=false
[bar.live.island]
height=64
clock_size=24
hover_widgets=[]
media_gradient=true
track_preview_seconds=0
''')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    for kind, proto, libs in (
        ('pointer', REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', REPO/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(proto), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(proto), str(base/(kind+'-code.c'))])
        source = base/(kind+'.c')
        source.write_text((REPO/f'tests/fixtures/island_{kind}.c').read_text().replace('x,y,1280,720', 'x,y,1280,1440'))
        run(['cc', '-I'+str(base), str(source), str(base/(kind+'-code.c')), *libs, '-o', str(base/kind)])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def send(proc, command):
        proc.stdin.write(command+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        send(pointer, f'move {round(x)} {round(y)}'); time.sleep(.12)

    def click(x, y):
        move(x, y); send(pointer, 'press'); send(pointer, 'release'); time.sleep(.45)

    def key(code):
        send(keyboard, str(code)); time.sleep(.4)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def text(name, output='TEST-1'):
        im = shot(name, output); scale = im.width/1280
        crop = im.crop((round(430*scale), 0, round(850*scale), round(350*scale)))
        path = out/(name+'-text.png'); crop.resize((1260, 1050)).save(path)
        return run(['tesseract', str(path), 'stdout', '--tessdata-dir', TESSDATA, '--psm', '11'])

    def clear():
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.5)

    def menu(output='TEST-1'):
        clear(); dispatch('hl.dsp.focus({monitor='+json.dumps(output)+'})')
        move(1100, 600 if output=='TEST-1' else 1320)
        assert msg('capture-menu') == 'ok'; time.sleep(.8)

    # Menu coordinates are logical, including the Island's top margin.
    def option(x, y, output='TEST-1'):
        click(450+x, 8+y+(720 if output=='TEST-2' else 0))

    def select_region():
        move(100, 250); send(pointer, 'press'); move(500, 500); send(pointer, 'release'); time.sleep(.35)

    def saved():
        return set(out.glob('screenshot*.png'))

    def recording():
        return msg('record-status').startswith('REC')

    def finish_recording(expected_audio):
        time.sleep(1.3); before=set((out/'Recordings').glob('*.mp4'))
        msg('record-stop'); wait(lambda:msg('record-status')=='idle', 'Recording failed to finish')
        assert before, 'No video was created'
        video=max(before, key=lambda path:path.stat().st_mtime)
        streams=json.loads(run(['ffprobe','-v','error','-show_entries','stream=codec_type','-of','json',str(video)]))['streams']
        assert {s['codec_type'] for s in streams} == ({'video','audio'} if expected_audio else {'video'}), streams
        clear()

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark'); time.sleep(1)
        menu(); content=text('capture-menu')
        assert all(word in content for word in ('Capture','Record','Region','Window','Monitor','Delay')), content
        assert not any(word in content for word in ('Audio','Desktop','Mic')), content
        # Keyboard focus must retain the menu; Escape must return to the normal Island.
        msg('island-focus'); key(15); key(28)
        assert 'Audio' in text('capture-menu-keyboard-record')
        key(1)
        assert 'Select Region' not in text('capture-menu-escape')
        menu(); option(285,70)
        content=text('capture-menu-record'); assert 'Audio' in content and 'Desktop' in content and 'Mic' in content, content
        option(100,70)
        content=text('capture-menu-back-to-screenshot')
        assert not any(word in content for word in ('Audio','Desktop','Mic')), content
        option(285,70)
        assert 'Audio' in text('capture-menu-record-again')
        key(1)

        # The media artwork remains animated behind the controls.
        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')], 'capture-menu-player.log')
        time.sleep(1.2); move(1100,600); time.sleep(.5); move(640,40); time.sleep(.9); shot('capture-menu-hover-without-button')
        menu()
        a=shot('capture-menu-art-before'); time.sleep(.6); b=shot('capture-menu-art-after'); strip=(580,10,690,16)
        assert sum(max(pixel)>12 for pixel in b.crop(strip).getdata())>300, 'Menu hid playing artwork'
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Menu froze playing artwork'
        run(['notify-send','Ordinary message','Keep capture controls available']); time.sleep(.6)
        assert 'Capture' in text('capture-menu-normal-notification')
        run(['notify-send','-u','critical','Urgent alert','Capture controls should return afterward']); time.sleep(.6)
        assert 'Urgent' in text('capture-menu-urgent')
        clear(); assert 'Capture' in text('capture-menu-restored')
        key(1); player.terminate(); player.wait(timeout=5); time.sleep(.7)

        # A private terminal changes colour after selection. The saved region must contain
        # the new frame, even though the normal screenshot preference freezes selection.
        signal=base/'capture-colour'; signal.write_text('waiting')
        script=base/'capture-colour.py'
        script.write_text('import pathlib,time\np=pathlib.Path('+repr(str(signal))+')\nwhile p.read_text()!="go": time.sleep(.05)\nprint("\\033]11;#20a040\\007",flush=True)\ntime.sleep(300)\n')
        terminal=start(['kitty','--config','NONE','--class','capture-colour','--override','background=#a02020',
                        '-e',sys.executable,str(script)], 'capture-colour.log')
        wait(lambda:any(w['class']=='capture-colour' for w in json.loads(ctl('-j','clients'))), 'Colour fixture did not map')
        menu(); option(100,70); option(130,116); option(190,158); option(240,205); select_region()
        assert 'Taking screenshot' in text('capture-countdown'), 'Missing screenshot countdown'
        before=saved(); signal.write_text('go')
        wait(lambda:bool(saved()-before), 'Delayed screenshot was not saved')
        result=Image.open(next(iter(saved()-before))).convert('RGB')
        assert result.size==(400,250), result.size
        red,green,blue=result.getpixel((200,150)); assert green>100 and green>red*2 and green>blue, (red,green,blue)
        clear(); terminal.terminate(); terminal.wait(timeout=5)

        # A held Cancel remains the same button while the countdown ticks.
        menu(); option(330,158); option(240,205); select_region(); before=saved()
        move(640,94); send(pointer,'press'); time.sleep(1.3); send(pointer,'release'); time.sleep(.7)
        assert 'Taking screenshot' not in text('capture-countdown-cancel')
        time.sleep(9); assert saved()==before, 'Canceled screenshot was still taken'
        menu(); option(240,205); select_region(); key(1)
        assert 'Taking screenshot' not in text('capture-countdown-escape')

        # Silent recording on a region, desktop audio on a scaled/rotated monitor.
        menu(); option(285,70); option(130,158); option(135,200); option(240,247); select_region()
        wait(recording, 'Silent region recording did not start'); finish_recording(False)
        menu('TEST-2'); option(285,70,'TEST-2'); option(305,116,'TEST-2'); option(130,158,'TEST-2')
        content=text('capture-menu-scaled','TEST-2'); assert 'Monitor' in content and 'Audio' in content,content
        option(220,200,'TEST-2'); option(240,247,'TEST-2'); shot('capture-monitor-picker','TEST-2'); click(600,1100)
        shot('capture-monitor-started','TEST-2')
        wait(recording, 'Scaled monitor recording did not start'); finish_recording(True)

        # Microphone mode must use the explicit input, not a monitor source.
        run(['pactl','load-module','module-remap-source','source_name=capture-mic','master=hyprland-test.monitor'])
        run(['pactl','set-default-source','capture-mic'])
        menu(); option(305,200); option(240,247); select_region()
        wait(recording, 'Microphone recording did not start'); finish_recording(True)

        # The other delay works for a screenshot of a scaled, rotated monitor, including
        # with animation disabled. Neither the selector nor countdown may remain in it.
        config=cfg/'config.toml'; original=config.read_text()
        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload'); time.sleep(.7)
        menu('TEST-2'); option(100,70,'TEST-2'); option(305,116,'TEST-2'); option(265,158,'TEST-2')
        before=saved(); option(240,205,'TEST-2'); click(600,1100)
        wait(lambda:bool(saved()-before), 'Scaled monitor screenshot was not saved')
        picture=next(iter(saved()-before)); result=Image.open(picture)
        assert result.size==(1920,1080), result.size
        crop=out/'capture-monitor-result.png'; result.crop((650,0,1270,440)).save(crop)
        contents=run(['tesseract',str(crop),'stdout','--tessdata-dir',TESSDATA,'--psm','11'])
        assert 'Taking' not in contents and 'Cancel' not in contents and 'Select' not in contents, contents
        config.write_text(original); msg('config-reload'); time.sleep(.7)

        # Existing stop shortcuts also cancel recording before it begins.
        menu(); option(285,70); option(330,158); option(240,247); select_region()
        msg('record-stop'); time.sleep(.6)
        assert 'Starting recording' not in text('capture-countdown-stop')

        # Reload and output removal must cancel a pending recording.
        menu(); option(285,70); option(330,158); option(240,247); select_region()
        assert 'Starting recording' in text('capture-record-countdown')
        msg('config-reload'); time.sleep(.7)
        assert 'Starting recording' not in text('capture-countdown-reloaded')
        menu('TEST-2'); option(285,70,'TEST-2'); option(305,116,'TEST-2')
        option(330,158,'TEST-2'); option(240,247,'TEST-2'); click(600,1100)
        assert 'Starting recording' in text('capture-before-output-removal','TEST-2')
        ctl('output','remove','TEST-2'); time.sleep(10.5)
        assert msg('record-status')=='idle', 'Output removal started a stale recording'
        # Restore the virtual pointer's original desktop extent before the lock check.
        ctl('output','create','headless','TEST-2'); time.sleep(1)
        # Lock only the private nested session, after all interactive checks finish.
        menu(); option(285,70); option(330,158); option(240,247); select_region()
        assert 'Starting recording' in text('capture-before-lock')
        msg('session','lock'); time.sleep(10.5)
        assert msg('record-status')=='idle', 'Locking started a pending recording'
        shot('capture-menu-locked')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: capture menu, keyboard, moving artwork, fresh delayed screenshot, cancellation, silent/desktop/microphone video, scaled monitor, reload, output removal and lock cancellation',flush=True)
    finally:
        if shell.poll() is None:
            msg('record-stop')
        pointer.terminate(); pointer.wait(timeout=5)
        keyboard.terminate(); keyboard.wait(timeout=5)
