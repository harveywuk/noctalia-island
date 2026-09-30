#!/usr/bin/env python3
"""Hyprland 0.56 Lua integration on two private, GPU-backed virtual outputs."""
import os, pathlib, subprocess, tempfile, time, json, re
REPO=pathlib.Path(__file__).resolve().parents[1]
out=pathlib.Path(tempfile.mkdtemp(prefix='hyprland-smoke-',dir=REPO/'build-rishot'))
print('Artifacts:',out,flush=True)
with tempfile.TemporaryDirectory(prefix='hp-') as tmp:
    base=pathlib.Path(tmp);runtime=base/'r';runtime.mkdir(mode=0o700)
    busconf=base/'bus.conf';busconf.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
    bus=subprocess.Popen(['dbus-daemon','--config-file='+str(busconf),'--nofork','--print-address=1'],stdout=subprocess.PIPE,text=True)
    processes=[bus];address=bus.stdout.readline().strip()
    env=dict(os.environ,HOME=str(base),XDG_RUNTIME_DIR=str(runtime),XDG_CONFIG_HOME=str(base/'config'),XDG_STATE_HOME=str(base/'state'),XDG_DATA_HOME=str(base/'data'),XDG_CACHE_HOME=str(base/'cache'),DBUS_SESSION_BUS_ADDRESS=address,DBUS_SYSTEM_BUS_ADDRESS=address,HYPRLAND_NO_SD_VARS='1',HYPRLAND_NO_SD_NOTIFY='1',HYPRLAND_NO_RT='1',HYPRLAND_NO_CRASHREPORTER='1',LIBSEAT_BACKEND='none',WLR_BACKENDS='headless',WLR_HEADLESS_OUTPUTS='1',WLR_LIBINPUT_NO_DEVICES='1',WLR_RENDERER='gles2')
    for key in ('DISPLAY','WAYLAND_DISPLAY','UMBRIEL_SOCKET','NIRI_SOCKET','SWAYSOCK','HYPRLAND_INSTANCE_SIGNATURE','XDG_SESSION_ID','XDG_SEAT','XDG_VTNR','LIBGL_ALWAYS_SOFTWARE'):env.pop(key,None)
    for key in ('XDG_CONFIG_HOME','XDG_STATE_HOME','XDG_DATA_HOME','XDG_CACHE_HOME'):
        pathlib.Path(env[key]).mkdir()
    env.update(XDG_CURRENT_DESKTOP='Hyprland',XDG_SESSION_TYPE='wayland',NOCTALIA_ASSETS_DIR=str(REPO/'assets'),NOCTALIA_CONFIG_HOME=env['XDG_CONFIG_HOME'],NOCTALIA_STATE_HOME=env['XDG_STATE_HOME'],NOCTALIA_DATA_HOME=env['XDG_DATA_HOME'],PIPEWIRE_RUNTIME_DIR=str(runtime),PULSE_SERVER='unix:'+str(runtime/'pulse/native'))
    def run(args): return subprocess.check_output(args,env=env,text=True,stderr=subprocess.STDOUT,timeout=20)
    def wait(check,reason):
        for _ in range(150):
            if check(): return
            time.sleep(.1)
        raise AssertionError(reason)
    def ctl(*args): return run(['hyprctl',*args])
    def dispatch(code):
        reply=ctl('dispatch',code)
        assert reply.strip()=='ok',reply
    # Prefer a Mesa device; callers can explicitly select a render node.
    devices=sorted(pathlib.Path('/dev/dri').glob('renderD*'),key=lambda p:((pathlib.Path('/sys/class/drm')/p.name/'device/vendor').read_text().strip()=='0x10de',p.name))
    assert devices, 'A GPU render node is required for the nested compositor'
    env.setdefault('WLR_RENDER_DRM_DEVICE',str(devices[0]))
    def start(args,name):
        with (out/name).open('w') as f:p=subprocess.Popen(args,env=env,stdout=f,stderr=f)
        processes.append(p);return p
    try:
        config=base/'labwc';config.mkdir();(config/'rc.xml').write_text('<labwc_config/>');(config/'autostart').write_text('')
        parent=start(['labwc','-C',str(config)],'parent.log')
        for _ in range(100):
            sockets=list(runtime.glob('wayland-*.lock'))
            if sockets or parent.poll() is not None:break
            time.sleep(.1)
        assert sockets, 'Private parent compositor failed; see parent.log'
        env['WAYLAND_DISPLAY']=sockets[0].name.removesuffix('.lock')
        config=base/'hyprland.lua';config.write_text('hl.monitor({output="",mode="1280x720@60",position="0x0",scale=1})\nhl.monitor({output="TEST-2",mode="1920x1080@60",position="0x720",scale=1.5,transform=2})\nhl.config({input={follow_mouse=1},misc={disable_hyprland_logo=true,disable_splash_rendering=true},debug={disable_logs=false,enable_stdout_logs=true}})\n')
        child=start(['Hyprland','-c',str(config)],'hyprland.log')
        for _ in range(150):
            ipc=list((runtime/'hypr').glob('*/.socket.sock'))
            if ipc or child.poll() is not None:break
            time.sleep(.1)
        assert ipc and child.poll() is None, 'Private Hyprland failed; see hyprland.log'
        env['HYPRLAND_INSTANCE_SIGNATURE']=ipc[0].parent.name
        time.sleep(2)
        for name in ('TEST-1','TEST-2'):
            print(subprocess.check_output(['hyprctl','output','create','headless',name],env=env,text=True),flush=True)
        time.sleep(1)
        result=subprocess.check_output(['hyprctl','-j','monitors'],env=env,text=True,stderr=subprocess.STDOUT)
        (out/'monitors.json').write_text(result)
        monitors={m['name']:m for m in json.loads(result)}
        assert set(monitors)=={'TEST-1','TEST-2'}
        assert (monitors['TEST-1']['x'],monitors['TEST-1']['y'])==(0,0)
        assert (monitors['TEST-2']['scale'],monitors['TEST-2']['transform'],monitors['TEST-2']['y'])==(1.5,2,720)
        env['WAYLAND_DISPLAY']=next(p.name.removesuffix('.lock') for p in runtime.glob('wayland-*.lock') if p not in sockets)
        cfg=base/'config/noctalia';cfg.mkdir()
        (cfg/'config.toml').write_text('[island]\nenabled=true\nhover_widgets=["workspaces","taskbar"]\n[bar.default]\nenabled=false\n[dock]\nenabled=false\n[shell]\nsetup_wizard_enabled=false\npolkit_agent=false\n[shell.screenshot]\ndirectory="'+str(out)+'"\n[osd.kinds]\nlock_keys=false\n[plugins]\nauto_update="none"\n[[plugins.source]]\nname="test"\nkind="path"\nlocation="/nonexistent"\nenabled=false\n')
        (base/'config/user-dirs.dirs').write_text('XDG_VIDEOS_DIR="'+str(out)+'"\n')
        wp=base/'config/wireplumber/wireplumber.conf.d';wp.mkdir(parents=True)
        (wp/'test.conf').write_text('wireplumber.profiles = { main = { hardware.audio = disabled hardware.bluetooth = disabled hardware.video-capture = disabled } }')
        start(['pipewire'],'pipewire.log');wait(lambda:(runtime/'pipewire-0').exists(),'PipeWire start')
        start(['wireplumber'],'wireplumber.log')
        start(['pipewire-pulse'],'pulse.log');wait(lambda:(runtime/'pulse/native').exists(),'Pulse start')
        run(['pactl','load-module','module-null-sink','sink_name=hyprland-test'])
        run(['pactl','set-default-sink','hyprland-test'])
        binary=str(REPO/'build-rishot/noctalia');shell=start([binary],'noctalia.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'Noctalia start')
        def msg(*args):
            reply=run([binary,'msg',*args]).strip()
            assert not reply.startswith('error'),reply
            return reply
        wait(lambda:msg('record-status')=='idle','Noctalia IPC')
        time.sleep(2)
        layers=json.loads(ctl('-j','layers'));(out/'layers.json').write_text(json.dumps(layers,indent=2))
        for name in ('TEST-1','TEST-2'):
            assert 'noctalia-island' in json.dumps(layers[name]),layers
            run(['grim','-o',name,str(out/(name+'-island.png'))])
        dispatch('hl.dsp.focus({monitor="TEST-1"})')
        dispatch('hl.dsp.focus({workspace="1"})')
        start(['kitty','--config','NONE','--class','hypr-test-one','--title','Hyprland test one','-e','sleep','180'],'kitty-one.log')
        wait(lambda:len(json.loads(ctl('-j','clients')))>=1,'first test window')
        dispatch(r'hl.dsp.focus({workspace="name:Review \"B\""})')
        start(['kitty','--config','NONE','--class','hypr-test-two','--title','Hyprland test two','-e','sleep','180'],'kitty-two.log')
        wait(lambda:len(json.loads(ctl('-j','clients')))>=2,'second test window')
        time.sleep(1)
        assert json.loads(ctl('-j','activeworkspace'))['name']=='Review "B"'
        print('Switch:',msg('workspace-switch','next'),flush=True)
        time.sleep(.5)
        assert json.loads(ctl('-j','activeworkspace'))['name']=='1'
        print('Switch:',msg('workspace-switch','prev'),flush=True)
        time.sleep(.5)
        assert json.loads(ctl('-j','activeworkspace'))['name']=='Review "B"'
        first=next(w for w in json.loads(ctl('-j','clients')) if w['class']=='hypr-test-one')['address']
        dispatch('hl.dsp.focus({window='+json.dumps('address:'+first)+'})')
        dispatch('hl.dsp.window.alter_zorder({mode="top",window='+json.dumps('address:'+first)+'})')
        assert json.loads(ctl('-j','activewindow'))['address']==first
        proto=REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
        run(['wayland-scanner','client-header',str(proto),str(base/'pointer-client.h')])
        run(['wayland-scanner','private-code',str(proto),str(base/'pointer-code.c')])
        (base/'pointer.c').write_text((REPO/'tests/fixtures/island_pointer.c').read_text().replace('x,y,1280,720','x,y,1280,1440'))
        run(['cc','-I'+str(base),str(base/'pointer.c'),str(base/'pointer-code.c'),'-lwayland-client','-o',str(base/'pointer')])
        pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);processes.append(pointer)
        def button(command):
            pointer.stdin.write(command+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok';time.sleep(.15)
        button('move 640 35');time.sleep(1)
        run(['grim','-o','TEST-1',str(out/'hover-island.png')])
        button('move 1100 600')
        print('Capture:',msg('screenshot-fullscreen'),flush=True)
        time.sleep(1)
        print('Record:',msg('record-region'),flush=True);time.sleep(.8)
        button('move 100 200');button('press')
        button('move 500 500');run(['grim','-o','TEST-1',str(out/'drag.png')]);button('release')
        run(['grim','-o','TEST-1',str(out/'recording.png')])
        wait(lambda:msg('record-status').startswith('REC'),'region recording starts')
        time.sleep(2);print('Stop:',msg('record-stop'),flush=True)
        wait(lambda:msg('record-status')=='idle','recording stops')
        print('Monitor record:',msg('record-monitor'),flush=True);time.sleep(.8)
        run(['grim','-o','TEST-1',str(out/'monitor-picker.png')]);button('move 727 45');button('press');button('release')
        wait(lambda:msg('record-status').startswith('REC'),'monitor recording starts')
        time.sleep(2);msg('record-stop');wait(lambda:msg('record-status')=='idle','monitor recording stops')
        videos=sorted(out.rglob('*.mp4'),key=lambda p:p.stat().st_mtime)
        assert len(videos)==2,videos
        for video,dimensions in zip(videos,[(400,300),(1920,1080)]):
            info=json.loads(run(['ffprobe','-v','error','-show_streams','-of','json',str(video)]))
            assert {s['codec_type'] for s in info['streams']}=={'video','audio'},info
            stream=next(s for s in info['streams'] if s['codec_type']=='video')
            assert (stream['width'],stream['height'])==dimensions,info
            assert float(stream['duration'])>1,info
        screenshots=list(out.glob('screenshot_*.png'))
        assert screenshots, 'Noctalia must save a screenshot'
        # Probe the installed screen-sharing backend on the same private bus.
        portal=start(['/usr/lib/xdg-desktop-portal-hyprland'],'portal.log')
        def portal_ready():
            try:
                reply=run(['gdbus','call','--session','--dest','org.freedesktop.impl.portal.desktop.hyprland','--object-path','/org/freedesktop/portal/desktop','--method','org.freedesktop.DBus.Properties.Get','org.freedesktop.impl.portal.ScreenCast','AvailableSourceTypes'])
                (out/'portal-source-types.txt').write_text(reply)
                types=re.search(r'uint32 (\d+)',reply)
                assert types and int(types[1]) & 3 == 3,reply
                return True
            except subprocess.CalledProcessError:
                assert portal.poll() is None,'Hyprland portal exited; see portal.log'
                return False
        wait(portal_ready,'Hyprland portal ScreenCast interface')
        time.sleep(2)
        assert shell.poll() is None,'Noctalia exited'
        print('PASS: scaled/rotated Island, quoted workspace IPC, Lua focus/raise, screenshot, region/monitor recording with audio, portal startup',flush=True)

    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=4)
                except subprocess.TimeoutExpired:p.kill();p.wait()
