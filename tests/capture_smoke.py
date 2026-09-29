#!/usr/bin/env python3
"""Exercise native capture controls on a private headless Umbriel display."""
import os, subprocess, sys, tempfile, time, pathlib, json
from PIL import Image
REPO = pathlib.Path(__file__).resolve().parents[1]
if '--worker' not in sys.argv:
    # Keep service activation from finding the real session's keyring daemon.
    with tempfile.TemporaryDirectory(prefix='capture-test-bus-') as bus:
        config = pathlib.Path(bus)/'bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        raise SystemExit(subprocess.call(['dbus-run-session','--config-file',str(config),'--',sys.executable,__file__,'--worker',*sys.argv[1:]]))
out = REPO / 'build-rishot/capture-smoke'
out.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='noctalia-capture-smoke-') as tmp:
    base = pathlib.Path(tmp); runtime = base/'runtime'; runtime.mkdir(mode=0o700)
    cfg = base/'config/noctalia'; cfg.mkdir(parents=True)
    (cfg/'config.toml').write_text('[island]\nenabled=true\n[bar.default]\nenabled=false\n[dock]\nenabled=false\n[shell]\nsetup_wizard_enabled=false\npolkit_agent=false\n[shell.screenshot]\ndirectory="'+str(out)+'"\n')
    (base/'config/user-dirs.dirs').write_text('XDG_VIDEOS_DIR="'+str(out)+'"\n')
    config = base/'umbriel.toml'; config.write_text('[output."HEADLESS-1"]\nmode="1280x720"\n')
    env=dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base/'config'), XDG_STATE_HOME=str(base/'state'), XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'), NOCTALIA_CONFIG_HOME=str(base/'config'), NOCTALIA_STATE_HOME=str(base/'state'), NOCTALIA_DATA_HOME=str(base/'data'), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_LIBINPUT_NO_DEVICES='1', LIBGL_ALWAYS_SOFTWARE='1', XDG_VIDEOS_DIR=str(out), PULSE_SERVER='unix:/run/user/1000/pulse/native')
    env.pop('WAYLAND_DISPLAY',None); env.pop('DISPLAY',None)
    env.pop('UMBRIEL_SOCKET',None)
    processes=[]
    def run(args): return subprocess.check_output(args,env=env,text=True,stderr=subprocess.STDOUT,timeout=15)
    def start(args,name):
        with (out/name).open('w') as f: p=subprocess.Popen(args,env=env,stdout=f,stderr=f)
        processes.append(p); return p
    def wait(check,reason):
        for _ in range(150):
            if check(): return
            time.sleep(.1)
        raise AssertionError(reason)
    try:
        compositor=start(['/usr/local/bin/umbriel','-c',str(config)],'umbriel.log')
        wait(lambda:list(runtime.glob('wayland-*.lock')),'headless compositor start')
        env['WAYLAND_DISPLAY']=next(runtime.glob('wayland-*.lock')).name.removesuffix('.lock')
        for kind,proto,libs in [('pointer',REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml',[]),('keyboard',REPO/'protocols/virtual-keyboard-unstable-v1.xml',['-lxkbcommon'])]:
            run(['wayland-scanner','client-header',str(proto),str(base/f'{kind}-client.h')])
            run(['wayland-scanner','private-code',str(proto),str(base/f'{kind}-code.c')])
            run(['cc','-I'+str(base),str(REPO/f'tests/fixtures/island_{kind}.c'),str(base/f'{kind}-code.c'),'-lwayland-client',*libs,'-o',str(base/kind)])
        pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True); processes.append(pointer)
        keyboard=subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True); processes.append(keyboard)
        def command(proc,text):
            proc.stdin.write(text+'\n');proc.stdin.flush();assert proc.stdout.readline().strip()=='ok';time.sleep(.1)
        def move(x,y): command(pointer,f'move {x} {y}')
        def click(): command(pointer,'press');command(pointer,'release')
        def key(code): command(keyboard,str(code))
        binary=str(REPO/'build-rishot/noctalia'); shell=start([binary],'noctalia.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'shell start')
        def msg(name): return run([binary,'msg',name])
        def ready():
            assert shell.poll() is None, f'Shell exited: {shell.returncode}'
            try: return msg('record-status').strip()=='idle'
            except subprocess.CalledProcessError as error:
                print(error.output.strip(),flush=True)
                raise
        wait(ready, 'capture IPC startup')
        assert msg('record-region').strip()=='ok';time.sleep(.4);key(1)
        assert msg('record-status').strip()=='idle', 'escape must cancel recording selection'
        assert msg('screenshot-annotate').strip()=='ok';time.sleep(1)
        run(['grim',str(out/'recording-toolbar.png')])
        layers=run(['/usr/local/bin/umbriel','layers'])
        (out/'capture-layers.txt').write_text(layers)
        assert 'overlay\tnoctalia-island' in layers and 'noctalia-annotate' not in layers
        move(575,40);click();time.sleep(.5);run(['grim',str(out/'colour-picker.png')])
        # Enter a custom colour; keyboard events must stay in the picker.
        move(530,522);click();key(107)
        for _ in range(8): key(14)
        for code in [11,11,30,30,33,33]: key(code)
        key(28);time.sleep(.2)
        run(['grim',str(out/'custom-colour.png')])
        key(1)
        move(100,200);command(pointer,'press');move(300,200);command(pointer,'release')
        run(['grim',str(out/'custom-colour-stroke.png')])
        pixel=Image.open(out/'custom-colour-stroke.png').convert('RGB').getpixel((200,200))
        assert pixel[0] < 10 and 160 < pixel[1] < 180 and pixel[2] > 245, pixel
        move(535,40);click();time.sleep(.5);run(['grim',str(out/'drawing-tools.png')])
        key(1)
        if '--preview' in sys.argv: raise SystemExit(0)
        move(447,40);click();time.sleep(.4)
        move(100,200);command(pointer,'press');move(500,500);command(pointer,'release')
        wait(lambda:msg('record-status').startswith('REC'),'region recording start')
        time.sleep(2)
        run(['grim',str(out/'recording-indicator.png')])
        # Click the red island timer to stop, then verify idle and video/audio streams.
        move(640,38);command(pointer,'press')
        before_tick=msg('record-status').strip()
        wait(lambda:msg('record-status').strip()!=before_tick,'recording timer tick while pressed')
        time.sleep(1.1) # Include the Island's one-second refresh before release.
        assert msg('record-status').startswith('REC'), 'press alone must not stop recording'
        command(pointer,'release')
        wait(lambda:msg('record-status').strip()=='idle','click-to-stop')
        assert msg('screenshot-annotate').strip()=='ok';time.sleep(1)
        move(487,40);click();time.sleep(1);run(['grim',str(out/'monitor-picker.png')]);move(690,45);click();time.sleep(.5);run(['grim',str(out/'monitor-picked.png')])
        wait(lambda:msg('record-status').startswith('REC'),'monitor recording start')
        time.sleep(2);assert msg('record-stop').strip()=='ok'
        wait(lambda:msg('record-status').strip()=='idle','IPC stop')
        assert msg('screenshot-annotate').strip()=='ok';time.sleep(1)
        move(400,350);key(23);move(400,350);command(pointer,'press');move(550,550);command(pointer,'release')
        key(44);move(650,300);command(pointer,'press');move(850,550);command(pointer,'release')
        run(['grim',str(out/'annotation-tools.png')]);key(1);time.sleep(.6)
        layers=run(['/usr/local/bin/umbriel','layers'])
        assert 'top\tnoctalia-island' in layers and 'noctalia-annotate' not in layers
        run(['grim',str(out/'island-restored.png')])
        videos=sorted((out/'Recordings').glob('*.mp4'),key=lambda p:p.stat().st_mtime)[-2:]
        assert len(videos)==2
        for video in videos:
            info=json.loads(run(['ffprobe','-v','error','-show_streams','-of','json',str(video)]))
            assert {s['codec_type'] for s in info['streams']}=={'video','audio'}
        assert shell.poll() is None
        print('PASS: selection cancel, toolbar region recording, island stop, toolbar monitor recording, IPC stop, annotation tools')
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=6)
                except subprocess.TimeoutExpired:p.kill();p.wait()
