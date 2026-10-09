#!/usr/bin/env python3
"""Check island settings and resized media controls on a private labwc display."""
import os, subprocess, sys, tempfile, time, pathlib, json, tomllib, ast
from PIL import Image, ImageChops
REPO = pathlib.Path(__file__).resolve().parents[1]
if '--worker' not in sys.argv:
    # Keep service activation from finding the real session's keyring daemon.
    with tempfile.TemporaryDirectory(prefix='island-settings-bus-') as bus:
        config = pathlib.Path(bus)/'bus.conf'
        config.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        raise SystemExit(subprocess.call(['dbus-run-session','--config-file',str(config),'--',sys.executable,__file__,'--worker',*sys.argv[1:]]))
out = REPO / 'build-rishot/island-settings-smoke'
out.mkdir(exist_ok=True)
(out/'player-actions.log').unlink(missing_ok=True)
with tempfile.TemporaryDirectory(prefix='island-settings-smoke-') as tmp:
    base = pathlib.Path(tmp); runtime = base/'runtime'; runtime.mkdir(mode=0o700)
    cfg = base/'config/noctalia'; cfg.mkdir(parents=True)
    (cfg/'config.toml').write_text('[island]\nenabled=true\nmedia_gradient=false\n[bar.default]\nenabled=false\n[dock]\nenabled=false\n[shell]\nsetup_wizard_enabled=false\npolkit_agent=false\n[shell.screenshot]\ndirectory="'+str(out)+'"\n')
    if any(mode in sys.argv for mode in ('--hover-widgets-only','--hover-layout-only','--hover-editor-only')):
        plugin_root=base/'hover-plugins'; plugin=plugin_root/'hover';plugin.mkdir(parents=True)
        (plugin/'plugin.toml').write_text('id="test/hover"\nname="Hover Widget Test"\nversion="1.0.0"\nplugin_api=3\n[[widget]]\nid="widget"\nentry="widget.luau"\n')
        (plugin/'widget.luau').write_text('local n=0\nfunction update() noctalia.setUpdateInterval(200); barWidget.setText("Plugin "..n); barWidget.setGlyph("puzzle"); noctalia.state.set("ticks",(noctalia.state.get("ticks") or 0)+1); noctalia.writeFile(noctalia.pluginDir().."/state.json",noctalia.json.encode({ticks=noctalia.state.get("ticks"),clicks=n})) end\nfunction onClick() n=n+1; noctalia.state.set("clicks",n); update() end\n')
        hover_config=(cfg/'config.toml').read_text().replace('[island]','[island]\nhover_widgets=["test/hover:widget","test_button","volume","clock"]')
        hover_config+='\n[plugins]\nauto_update="none"\nenabled=["test/hover"]\n[[plugins.source]]\nname="hover-test"\nkind="path"\nlocation='+json.dumps(str(plugin_root))+'\nenabled=true\n'
        hover_config+='\n[widget.test_button]\ntype="custom_button"\nlabel="Action"\nglyph="star"\n[widget.test_button.actions]\nleft="exec touch '+str(base/'widget-clicked')+'"\n'
        # Lock-key LEDs are global even on a private display; keep real typing from interrupting these checks.
        hover_config+='\n[osd.kinds]\nlock_keys=false\n'
        if '--hover-layout-only' in sys.argv or '--hover-editor-only' in sys.argv:
            hover_config=hover_config.replace('hover_widgets=["test/hover:widget","test_button","volume","clock"]',
                'hover_widgets=["test_button"]\nhover_widgets_center=["test/hover:widget"]\nhover_widgets_right=["right_button"]')
            hover_config+='\n[widget.right_button]\ntype="custom_button"\nlabel="R"\n[widget.right_button.actions]\nleft="exec touch '+str(base/'right-clicked')+'"\n'
        if '--hover-layout-only' in sys.argv:
            # The Cupertino Island keeps hover widgets to the calendar view; the theme look shows
            # them beside every expanded section, which is what these placement checks cover.
            hover_config=hover_config.replace('[island]','[island]\nappearance="theme"',1)
        (cfg/'config.toml').write_text(hover_config)
    if '--progress-outline-only' in sys.argv:
        plugin_root=base/'progress-plugins';plugin=plugin_root/'timer';plugin.mkdir(parents=True)
        (plugin/'plugin.toml').write_text('id="noctalia/timer"\nname="Outline test timer"\nversion="1.0.0"\nplugin_api=3\n[[service]]\nid="timer"\nentry="timer.luau"\n')
        (plugin/'timer.luau').write_text((REPO/'tests/fixtures/island_progress_timer.luau').read_text())
        with (cfg/'config.toml').open('a') as f:
            f.write('\n[osd.kinds]\nlock_keys=false\n[plugins]\nauto_update="none"\nenabled=["noctalia/timer"]\n[[plugins.source]]\nname="outline-test"\nkind="path"\nlocation='+json.dumps(str(plugin_root))+'\nenabled=true\n')
    if '--polish-only' in sys.argv:
        config_path=cfg/'config.toml'
        config_path.write_text(config_path.read_text().replace('[island]','[island]\nscale=1.4'))
    if any(mode in sys.argv for mode in ('--timer-only','--polish-only','--activity-switcher-only')):
        import shutil
        timer_source=pathlib.Path(os.environ.get('ISLAND_TIMER_SOURCE','/tmp/noctalia-official-timer-review'))/'timer'
        pomo_source=pathlib.Path(os.environ.get('ISLAND_POMODORO_SOURCE','/tmp/noctalia-community-timer-review'))/'pomodoro'
        plugin_root=base/'timer-plugins';plugin_root.mkdir()
        for source, name in [(timer_source,'timer'),(pomo_source,'pomodoro')]:
            assert (source/'plugin.toml').exists(), 'Set ISLAND_TIMER_SOURCE and ISLAND_POMODORO_SOURCE to reviewed upstream checkouts'
            target=plugin_root/name;shutil.copytree(source,target)
            manifest=target/'plugin.toml'
            manifest.write_text(manifest.read_text()+'\n[[service]]\nid="island-test"\nentry="island-test.luau"\n')
            (target/'island-test.luau').write_text((REPO/'tests/fixtures/island_timer_probe.luau').read_text().replace('__POMODORO__','true' if name=='pomodoro' else 'false'))
        with (cfg/'config.toml').open('a') as f:
            f.write('\n[plugins]\nauto_update="none"\nenabled=["noctalia/timer","thepunkoff/pomodoro"]\n[[plugins.source]]\nname="timer-test"\nkind="path"\nlocation='+json.dumps(str(plugin_root))+'\nenabled=true\n')
            f.write('\n[osd.kinds]\nlock_keys=false\n')
    if '[osd.kinds]' not in (cfg/'config.toml').read_text():
        # Lock-key LEDs are global even on a private display; keep real typing out of the checks.
        with (cfg/'config.toml').open('a') as f:
            f.write('\n[osd.kinds]\nlock_keys=false\n')
    (base/'config/user-dirs.dirs').write_text('XDG_VIDEOS_DIR="'+str(out)+'"\n')
    config = base/'labwc'; config.mkdir()
    (config/'rc.xml').write_text('<labwc_config/>')
    (config/'autostart').write_text('')
    env=dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base/'config'), XDG_STATE_HOME=str(base/'state'), XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'), NOCTALIA_CONFIG_HOME=str(base/'config'), NOCTALIA_STATE_HOME=str(base/'state'), NOCTALIA_DATA_HOME=str(base/'data'), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_LIBINPUT_NO_DEVICES='1', LIBGL_ALWAYS_SOFTWARE='1', XDG_VIDEOS_DIR=str(out))
    env['NOCTALIA_ASSETS_DIR']=str(REPO/'assets')
    # Keep the host's webcam users (the /proc scan) out of the private session.
    (base/'emptyproc').mkdir()
    env['NOCTALIA_PRIVACY_PROC_ROOT']=str(base/'emptyproc')
    # Hold the artwork gradient still so pixel comparisons over media cards stay stable.
    env['NOCTALIA_FREEZE_ARTWORK_FLOW']='1'
    env['HOME']=str(base)
    env['DBUS_SYSTEM_BUS_ADDRESS']=env['DBUS_SESSION_BUS_ADDRESS']
    env.pop('WAYLAND_DISPLAY',None); env.pop('DISPLAY',None)
    for key in ('HYPRLAND_INSTANCE_SIGNATURE','SWAYSOCK','TRIAD_SOCKET','MANGO_INSTANCE_SIGNATURE'):
        env.pop(key,None)
    env.update(XDG_CURRENT_DESKTOP='labwc',XDG_SESSION_TYPE='wayland',HOME=str(base),GSETTINGS_BACKEND='keyfile')
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
        if '--privacy-only' in sys.argv or '--visualizer-only' in sys.argv:
            env['PIPEWIRE_RUNTIME_DIR']=str(runtime)
            audio=start(['pipewire'],'privacy-pipewire.log')
            wait(lambda:(runtime/'pipewire-0').exists(),'private PipeWire startup')
        if '--visualizer-only' in sys.argv:
            wp=base/'config/wireplumber/wireplumber.conf.d';wp.mkdir(parents=True)
            (wp/'test.conf').write_text('wireplumber.profiles = { main = { hardware.audio = disabled hardware.bluetooth = disabled hardware.video-capture = disabled } }')
            start(['wireplumber'],'visualizer-wireplumber.log')
            env['PULSE_SERVER']='unix:'+str(runtime/'pulse/native')
            start(['pipewire-pulse'],'visualizer-pulse.log')
            wait(lambda:(runtime/'pulse/native').exists(),'private PulseAudio startup')
            run(['pactl','load-module','module-null-sink','sink_name=island-test'])
            run(['pactl','set-default-sink','island-test'])
        compositor=start(['labwc','-C',str(config)],'labwc.log')
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
        if any(mode in sys.argv for mode in ('--battery-only','--privacy-only','--polish-only','--hover-layout-only','--progress-outline-only')):
            env['DBUS_SYSTEM_BUS_ADDRESS'] = env['DBUS_SESSION_BUS_ADDRESS']
            battery = subprocess.Popen([sys.executable, str(REPO/'tests/fixtures/island_battery.py')], env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            processes.append(battery)
            assert battery.stdout.readline().strip() == 'ok'
        if '--bluetooth-preview-only' in sys.argv or '--island-timing-only' in sys.argv:
            bluetooth = subprocess.Popen([sys.executable, str(REPO/'tests/fixtures/island_bluetooth.py')], env=env,
                                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            processes.append(bluetooth)
            assert bluetooth.stdout.readline().strip() == 'ok'
        binary=os.environ.get('NOCTALIA_TEST_BINARY',str(REPO/'build-rishot/noctalia')); shell=start([binary],'noctalia.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'shell start')
        def msg(*words): return run([binary,'msg',*words])
        def find_box(word,name,min_x=0,min_y=0):
            """(left, top, width, height) of the first OCR word containing `word` on a fresh screenshot."""
            import csv,io
            run(['grim',str(out/name)])
            data=run(['tesseract',str(out/name),'stdout','--tessdata-dir',os.environ.get('NOCTALIA_TEST_TESSDATA',str(REPO/'build-rishot/test-data/tessdata')),
                      '--psm','11','-c','tessedit_create_tsv=1'])
            for row in csv.DictReader(io.StringIO(data),delimiter='\t',quoting=csv.QUOTE_NONE):
                if word in (row.get('text') or '') and int(row['left'])>=min_x and int(row['top'])>=min_y:
                    return int(row['left']),int(row['top']),int(row['width']),int(row['height'])
            raise AssertionError(f'{word!r} not found in {name}')
        def find_text(word,name,min_x=0):
            """Centre of the first OCR word containing `word` (right of min_x) on a fresh screenshot."""
            import csv,io
            run(['grim',str(out/name)])
            data=run(['tesseract',str(out/name),'stdout','--tessdata-dir',os.environ.get('NOCTALIA_TEST_TESSDATA',str(REPO/'build-rishot/test-data/tessdata')),
                      '--psm','11','-c','tessedit_create_tsv=1'])
            for row in csv.DictReader(io.StringIO(data),delimiter='\t',quoting=csv.QUOTE_NONE):
                if word in (row.get('text') or '') and int(row['left'])>=min_x:
                    return int(row['left'])+int(row['width'])//2,int(row['top'])+int(row['height'])//2
            raise AssertionError(f'{word!r} not found in {name}')
        def click_switch(label,name,min_x=420,xs=(1020,1110),below=50):
            """Click the switch in the settings row titled `label`: the round knob right of the row."""
            _,label_y=find_text(label,name,min_x=min_x)
            image=Image.open(out/name).convert('RGB')
            knob=[(x,y) for y in range(label_y-10,label_y+below) for x in range(*xs)
                  if min(image.getpixel((x,y)))>225]
            assert knob,f'No switch beside {label!r} in {name}'
            move(sum(p[0] for p in knob)//len(knob),sum(p[1] for p in knob)//len(knob));click()
        def ready():
            assert shell.poll() is None, f'Shell exited: {shell.returncode}'
            try: return msg('theme-mode-get').strip() in ('dark', 'light')
            except subprocess.CalledProcessError: return False
        wait(ready, 'shell IPC startup')
        time.sleep(3)
        if '--activity-switcher-only' in sys.argv:
            def state(name):
                try:return json.loads((plugin_root/name/'test-state.json').read_text()) or {}
                except (FileNotFoundError,json.JSONDecodeError):return {}
            def timer(event,*payload):msg('plugin','noctalia/timer:island-test','all',event,*payload)
            def shot(name):
                path=out/('activity-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB')
            def events():
                path=out/'player-actions.log'
                return path.read_text() if path.exists() else ''
            wait(lambda:state('timer').get('state')=='IDLE','Timer plugin ready')
            msg('theme-mode-set','dark');move(1100,600)
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'activity-player.log')
            time.sleep(1);move(640,40);time.sleep(.7)
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,
                stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);processes.append(publisher)
            def download(value,visible=True):
                command(publisher,json.dumps({'uri':'application://switcher-download.desktop',
                    'properties':{'progress':value,'progress-visible':visible}}))
            download(.25);timer('start','600');time.sleep(1)
            shot('media-new-activities')
            move(640,207);click();time.sleep(.6)
            assert events().count('PlayPause')==1,'New activities stole the selected media view'
            time.sleep(3.3);shot('paused-media')
            move(590,159);command(pointer,'press');move(720,159);command(pointer,'release');time.sleep(.3)
            assert 'SetPosition' in events(),'Seeking failed below the tab row'
            move(750,31);click();time.sleep(.6);shot('timers')
            move(530,127);click()
            wait(lambda:state('timer').get('state')=='PAUSED','Timer tab pause control')
            move(640,31);click();time.sleep(.6);shot('downloads')
            download(.65);time.sleep(.4);shot('download-progress')
            move(750,31);click();time.sleep(.6);before=shot('timer-before-alert')
            notification=int(run(['notify-send','-p','-a','Activity test','-u','critical','-t','0','Urgent alert','Activities stay behind this notification.']).strip())
            time.sleep(.7);alert=shot('urgent')
            assert ImageChops.difference(before.crop((470,50,810,150)),alert.crop((470,50,810,150))).getbbox(),'Urgent notification did not interrupt activity'
            run(['gdbus','call','--session','--dest','org.freedesktop.Notifications','--object-path','/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.CloseNotification',str(notification)])
            time.sleep(.7);shot('timer-after-alert');move(530,127);click()
            wait(lambda:state('timer').get('state')=='RUNNING','Selected timer did not return after alert dismissal')
            move(640,31);click();time.sleep(.5);download(1,False);time.sleep(.7);shot('download-ended')
            move(640,207);click();time.sleep(.4)
            assert events().count('PlayPause')==2,'Finished download did not fall back to media'
            move(720,31);click();time.sleep(.5);timer('RESET');time.sleep(.7)
            move(640,207);click();time.sleep(.4)
            assert events().count('PlayPause')==3,'Cancelled timer did not fall back to media'
            move(1100,600);time.sleep(.7);shot('compact')
            move(640,40);time.sleep(.7);move(640,163);click();time.sleep(.4)
            assert events().count('PlayPause')==4,'Leaving did not reset the switcher to the single-activity layout'
            # Keyboard focus survives another activity arriving and reaches the tabs.
            move(1100,600);time.sleep(.5);msg('island-focus');time.sleep(.5)
            download(.3);timer('start','600');time.sleep(.7)
            key(15);key(28);time.sleep(.4)
            assert events().count('PlayPause')==5,'Activity arrival stole media keyboard focus'
            key(15);key(15);key(15);key(15);key(28);time.sleep(.5)
            shot('keyboard-timers')
            # Timers tab retains focus; next Tab cycles to its pause control.
            key(15);key(28)
            wait(lambda:state('timer').get('state')=='PAUSED','Keyboard switching did not reach timer controls')
            key(1);move(1100,600);time.sleep(.5)
            base_config=(cfg/'config.toml').read_text()
            (cfg/'config.toml').write_text(base_config.replace('[island]','[island]\nhover_show_media=false\nhover_show_timers=false'))
            msg('config-reload');time.sleep(.6);move(640,40);time.sleep(.6);shot('hidden-sections')
            player.terminate();player.wait(timeout=6)
            assert shell.poll() is None
            print('PASS: activity selection, paused media/seek, timer controls, download progress, urgent priority, completed-activity fallback, collapse, keyboard switching and hidden sections')
            raise SystemExit(0)
        if '--island-timing-only' in sys.argv:
            move(1100,600);msg('theme-mode-set','dark')
            original=(cfg/'config.toml').read_text()
            managed='\n[bar]\norder=["timing","default"]\n[bar.timing]\npresentation="island"\nenabled=true\n[bar.timing.island]\nmedia_gradient=false\ntrack_preview_seconds=8\npaused_media_seconds=7\nbluetooth_preview_seconds=10\n[bar.timing.monitor.HEADLESS-1.island]\ntrack_preview_seconds=2\npaused_media_seconds=1\nbluetooth_preview_seconds=2\nreveal_on_track_change=false\n'
            def configure(text):
                (cfg/'config.toml').write_text(original+text);msg('config-reload');time.sleep(.7)
            def shot(name):
                path=out/('timing-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB')
            def width(image):
                left=right=640
                # Row 18 sits above the artwork but below the pill's rounded ends' steepest curve.
                while left>200 and max(image.getpixel((left-1,18)))<45:left-=1
                while right<1080 and max(image.getpixel((right+1,18)))<45:right+=1
                return right-left+1
            def media(method):
                return run(['gdbus','call','--session','--dest','org.mpris.MediaPlayer2.islandtest',
                    '--object-path','/org/mpris/MediaPlayer2','--method','org.mpris.MediaPlayer2.Player.'+method])
            def bt(**properties):command(bluetooth,json.dumps(properties))
            configure(managed)
            bt(Connected=True,Percentage=75);time.sleep(.5)
            assert width(shot('bluetooth-preview'))>210
            time.sleep(2)
            assert width(shot('bluetooth-expired'))<200,'Monitor Bluetooth duration was ignored'
            bt(Connected=False)
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            env['ISLAND_TEST_TICK']='1'
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'timing-player.log')
            time.sleep(.8)
            assert width(shot('track-preview'))>380
            time.sleep(2)
            assert 250<width(shot('track-expired'))<310,'Monitor track duration was ignored'
            media('Pause');time.sleep(.4)
            assert width(shot('pause-grace'))>240
            time.sleep(1)
            assert width(shot('pause-expired'))<200,'Monitor pause duration was ignored'
            zero=managed.replace('track_preview_seconds=2','track_preview_seconds=0').replace('paused_media_seconds=1','paused_media_seconds=0').replace('bluetooth_preview_seconds=2','bluetooth_preview_seconds=0')
            configure(zero);media('Play');media('Next');time.sleep(.6)
            assert 250<width(shot('zero-track'))<310,'Zero must disable track previews'
            media('Pause');time.sleep(.6)
            assert width(shot('zero-pause'))<200,'Zero must hide paused media immediately'
            bt(Connected=True,Percentage=75);time.sleep(.6)
            assert width(shot('zero-bluetooth'))<200,'Zero must disable Bluetooth previews'
            bt(Percentage=5);time.sleep(.6)
            assert width(shot('zero-low-battery'))>210,'Zero preview duration must preserve low-battery warnings'
            bt(Connected=False)
            hidden=managed.replace('enabled=true','enabled=true\nauto_hide=true')
            configure(hidden);before=shot('auto-hidden').crop((300,5,980,95))
            media('Play');media('Previous');time.sleep(.8)
            assert ImageChops.difference(before,shot('no-reveal').crop((300,5,980,95))).getbbox() is None,'Reveal disabled on monitor was ignored'
            configure(hidden.replace('reveal_on_track_change=false','reveal_on_track_change=true'))
            media('Next');time.sleep(.7)
            assert width(shot('reveal-enabled'))>380
            time.sleep(2)
            assert ImageChops.difference(before,shot('reveal-expired').crop((300,5,980,95))).getbbox() is None,'Reveal did not end at monitor duration'
            player.terminate();player.wait(timeout=6)
            configure(managed)
            msg('settings-open','bar');time.sleep(.8);shot('settings')
            (out/'timing-inspect-env.json').write_text(json.dumps(env))
            if os.environ.get('NOCTALIA_TEST_TIMING_INSPECT'):
                print('INSPECT: timing settings ready',flush=True)
                until=time.monotonic()+int(os.environ['NOCTALIA_TEST_TIMING_INSPECT'])
                while time.monotonic()<until:time.sleep(.25)
            print('PASS: monitor preview/pause/Bluetooth durations, zero values, low battery and per-monitor reveal preference')
            raise SystemExit(0)
        if '--media-polish-only' in sys.argv:
            move(1100,600);msg('theme-mode-set','dark')
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            env['ISLAND_TEST_TICK']='1'
            def shot(name):
                path=out/('media-polish-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB')
            def width(image):
                # Measure the uninterrupted dark capsule above its text and artwork.
                left=right=640
                # Row 18 sits above the artwork but below the pill's rounded ends' steepest curve.
                while left>200 and max(image.getpixel((left-1,18)))<45:left-=1
                while right<1080 and max(image.getpixel((right+1,18)))<45:right+=1
                return right-left+1
            def media(method):
                return run(['gdbus','call','--session','--dest','org.mpris.MediaPlayer2.islandtest',
                    '--object-path','/org/mpris/MediaPlayer2','--method','org.mpris.MediaPlayer2.Player.'+method])
            def until(deadline):time.sleep(max(0,deadline-time.monotonic()))
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'media-polish-player.log')
            started=time.monotonic();time.sleep(1.2)
            assert width(shot('first-track'))>380,'First track must show the wider title preview'
            until(started+3);msg('config-reload');time.sleep(.6)
            assert width(shot('preview-reload'))>380,'Reload interrupted the preview'
            until(started+6)
            assert 250<width(shot('compact'))<310,'Position updates/reload restarted the five-second preview'
            media('Next');changed=time.monotonic();time.sleep(.7)
            assert width(shot('next-track'))>380,'Track change did not show a title preview'
            msg('panel-open','control-center');time.sleep(.5);msg('panel-close');time.sleep(.6)
            assert width(shot('preview-panel-return'))>380,'Closing a panel lost the preview width'
            until(changed+5.8)
            assert 250<width(shot('next-compact'))<310,'Track preview did not collapse'
            media('Pause');paused=time.monotonic();time.sleep(.6)
            assert 250<width(shot('pause-grace'))<310,'Pause should retain a compact indicator briefly'
            msg('config-reload');until(paused+3.8)
            assert width(shot('paused-hidden'))<200,'Paused indicator did not hide after three seconds'
            move(640,35);time.sleep(.8);shot('paused-hover')
            move(640,163);click();time.sleep(.6)
            assert 'PlayPause' in (out/'player-actions.log').read_text(),'Paused controls unavailable on hover'
            click();time.sleep(3.5);shot('pause-held-hover')
            assert (out/'player-actions.log').read_text().count('PlayPause')==2,'Pause button moved during playback update'
            move(590,115);command(pointer,'press');move(720,115);command(pointer,'release');time.sleep(.4)
            assert 'SetPosition' in (out/'player-actions.log').read_text(),'Paused hover seeking did not reach MPRIS'
            move(1100,600);time.sleep(.6)
            assert width(shot('pause-left'))<200,'Leaving paused controls did not restore the clock'
            media('Play');time.sleep(.5)
            assert 250<width(shot('resumed'))<310,'Resume should restore compact playback without repeating the title'
            media('Stop');time.sleep(.6)
            assert width(shot('stopped'))<200,'Stopped player left a stale indicator'
            path=cfg/'config.toml'
            path.write_text(path.read_text()+'\n[bar.media]\npresentation="island"\nenabled=true\nauto_hide=true\n[bar.media.island]\nmedia_gradient=false\n')
            msg('config-reload');time.sleep(.8)
            hidden=shot('auto-hidden').crop((300,5,980,95))
            media('Play');media('Previous');time.sleep(.7)
            assert width(shot('auto-reveal'))>380,'Track preview did not reveal an auto-hidden Island'
            time.sleep(5.2)
            assert ImageChops.difference(hidden,shot('auto-hidden-again').crop((300,5,980,95))).getbbox() is None,'Island did not hide after the track preview'
            run(['notify-send','-a','Media priority','-u','critical','-t','0','Urgent notification','Keep this above the track preview.'])
            time.sleep(.8);urgent=shot('urgent')
            media('Next');time.sleep(.7)
            # Compare the alert text inside the card; the critical-alert halo intentionally pulses.
            assert ImageChops.difference(urgent.crop((452,46,825,95)),shot('urgent-track-change').crop((452,46,825,95))).getbbox() is None,'Track change replaced an urgent notification'
            msg('notification-clear-active');msg('notification-clear-history');time.sleep(.6)
            assert width(shot('preview-after-urgent'))>380,'Remaining track preview did not return after notification dismissal'
            path.write_text(path.read_text().replace('auto_hide=true','auto_hide=false'))
            msg('config-reload');time.sleep(.5)
            media('Play');time.sleep(.3);player.terminate();player.wait(timeout=6);time.sleep(.6)
            assert width(shot('player-closed'))<200,'Closing the player left a stale indicator'
            print('PASS: five-second track previews, reload/position stability, panel return, pause grace, paused hover controls/seek, resume, stop, auto-hide, urgent priority and player removal')
            raise SystemExit(0)
        if '--notification-polish-only' in sys.argv:
            def send(title,body='Notification details',urgency='normal',timeout=0,app='Preview test',replace=None):
                args=['notify-send','-p','-a',app,'-u',urgency,'-t',str(timeout)]
                if replace is not None:args+=['-r',str(replace)]
                return int(run(args+[title,body]).strip())
            def close(identifier):
                run(['gdbus','call','--session','--dest','org.freedesktop.Notifications',
                     '--object-path','/org/freedesktop/Notifications','--method',
                     'org.freedesktop.Notifications.CloseNotification',str(identifier)])
                time.sleep(.6)
            def shot(name):
                path=out/('notification-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB')
            def history():
                path=base/'state/noctalia/notification_history.json'
                return json.loads(path.read_text())['entries'] if path.exists() else []
            def entry(identifier):
                return next((e for e in history() if e['notification']['id']==identifier),None)
            def tall(image):
                # Wallpaper at this location is much lighter than the Island card.
                return max(image.getpixel((500,95)))<35
            move(1100,600);msg('notification-clear-active');msg('notification-clear-history')
            normal=send('Ordinary preview');start_time=time.monotonic();time.sleep(.8)
            assert tall(shot('preview')),'New notification did not expand'
            time.sleep(1);send('Updated preview',replace=normal)
            time.sleep(max(0,start_time+5.8-time.monotonic()))
            assert not tall(shot('collapsed')),'Ordinary preview did not collapse after five seconds'
            assert entry(normal) and not entry(normal)['active'] and not entry(normal)['seen'],'Preview expiry must retain unread history'
            move(640,40);time.sleep(.8);shot('unread-hover')
            msg('panel-open','control-center','notifications');time.sleep(.8)
            assert entry(normal)['seen'],'Opening history should mark notifications seen'
            msg('panel-close');move(1100,600);time.sleep(.7)
            urgent=send('Urgent stays visible',urgency='critical',timeout=1000);time.sleep(.8)
            move(640,60);time.sleep(.3);move(1100,600)
            msg('config-reload');time.sleep(1.5)
            assert tall(shot('urgent')) and entry(urgent)['active'],'Urgent alert expired after pointer leave or reload'
            send('Urgent stays visible',urgency='critical',timeout=1000,replace=urgent);time.sleep(1.5)
            assert tall(shot('urgent-identical-update')) and entry(urgent)['active'],'Identical urgent replacement lost its persistent display'
            first=shot('urgent-first')
            send('Ordinary while urgent',timeout=2000);time.sleep(.7)
            assert ImageChops.difference(first.crop((420,35,860,160)),shot('urgent-uninterrupted').crop((420,35,860,160))).getbbox() is None,'Ordinary alert replaced an urgent alert'
            second=send('Second urgent',urgency='critical',timeout=1000);time.sleep(1.5)
            assert entry(second)['active'],'Queued urgent alert expired'
            close(urgent);shot('urgent-next');time.sleep(1.2)
            assert tall(shot('urgent-next-later')) and entry(second)['active'],'Next urgent did not remain visible'
            close(second)
            hovered=send('Reading a preview',timeout=2000);time.sleep(.7);move(640,70);time.sleep(5.3)
            assert tall(shot('hover-held')) and entry(hovered)['active'],'Hovered preview disappeared while reading'
            move(1100,600);time.sleep(.8)
            assert not tall(shot('hover-left')) and not entry(hovered)['active'],'Expired hover preview did not collapse on leave'
            settings=cfg/'config.toml';original=settings.read_text()
            settings.write_text(original+'''\n[notification.filter."Silent app"]
match="silent-app"
play_sound=false
[notification.filter."History app"]
match="history-app"
show_toast=false
play_sound=false
[notification.filter."Hidden app"]
match="hidden-app"
show_toast=false
save_history=false
play_sound=false
''');msg('config-reload');time.sleep(.5)
            hidden=send('Hidden notification',app='hidden-app');time.sleep(.5)
            assert not tall(shot('hidden-app')) and entry(hidden) is None,'Hidden application leaked into preview or history'
            quiet=send('Saved for later',app='history-app');time.sleep(.6)
            assert not tall(shot('history-app')) and entry(quiet),'History-only application displayed a preview or lost its history'
            silent=send('Silent preview',app='silent-app');time.sleep(.7)
            assert tall(shot('silent-app')) and entry(silent),'Silent application lost its visual preview'
            close(silent)
            msg('notification-dnd-set','on');suppressed=send('Respect DND',urgency='critical',timeout=1000);time.sleep(1.5)
            assert not tall(shot('urgent-dnd')),'Urgent alert bypassed Do Not Disturb'
            msg('notification-dnd-set','off')
            msg('settings-open','notifications');time.sleep(.7);shot('settings')
            # Notification settings are grouped into sub-pages; the filter list is on Filtering.
            move(*find_text('Filtering','notification-settings-groups.png',min_x=420));click();time.sleep(.5);shot('filtering')
            _,hidden_y=find_text('hidden-app','notification-filters.png',min_x=420)
            move(1042,hidden_y);click();time.sleep(.5);shot('delivery-editor')  # The row's settings gear.
            move(630,370);click();time.sleep(.3);shot('delivery-options')
            move(330,450);click();time.sleep(.6)
            saved=tomllib.loads((base/'state/noctalia/settings.toml').read_text())
            delivery=next(f for f in saved['notification']['filter'].values() if f['match']=='hidden-app')
            assert delivery['show_toast'] and delivery['save_history'] and not delivery['play_sound'],'Silent dropdown choice did not persist its delivery policy'
            shot('delivery-saved')
            if os.environ.get('NOCTALIA_TEST_NOTIFICATION_INSPECT'):
                (out/'notification-inspect-env.json').write_text(json.dumps(env))
                print('INSPECT: notification filter settings ready',flush=True)
                time.sleep(int(os.environ['NOCTALIA_TEST_NOTIFICATION_INSPECT']))
            assert shell.poll() is None
            print('PASS: five-second previews, replacement timing, unread history, urgent retention/queue, hover pause, DND and per-app delivery')
            raise SystemExit(0)
        if '--bluetooth-preview-only' in sys.argv:
            def publish(**values): command(bluetooth,json.dumps(values))
            def shot(name):
                path=out/('bluetooth-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB')
            def differs(a,b,bounds=(400,8,880,64)):
                a=a.crop(bounds);b=b.crop(bounds)
                if bounds==(400,8,880,64):
                    a.paste((0,0,0),(180,0,300,56));b.paste((0,0,0),(180,0,300,56))
                return ImageChops.difference(a,b).getbbox() is not None
            move(1100,600);absent=shot('disconnected')
            publish(Connected=True,Percentage=75);connected_at=time.monotonic();time.sleep(.7)
            assert differs(absent,shot('connected')),'Connection must show the battery ring'
            time.sleep(1);publish(Percentage=42);msg('config-reload')
            time.sleep(max(0,connected_at+4.1-time.monotonic()))
            assert differs(absent,shot('four-seconds')),'Battery ring disappeared before five seconds'
            time.sleep(max(0,connected_at+5.8-time.monotonic()))
            assert not differs(absent,shot('expired')),'Battery updates or reload restarted the connection countdown'
            move(640,40);time.sleep(.8);hover=shot('hover')
            publish(Connected=False);time.sleep(.8)
            assert differs(hover,shot('disconnected-hover'),(440,135,840,235)),'Expired battery must remain in hover details'
            move(1100,600);publish(Connected=True,Percentage=42);time.sleep(.7)
            assert differs(absent,shot('reconnected')),'Reconnect must restart the preview'
            time.sleep(5);publish(Percentage=5);time.sleep(.7)
            assert differs(absent,shot('low')),'Low battery must stay visible after preview expiry'
            time.sleep(5.2)
            assert differs(absent,shot('low-later')),'Low battery ring must not time out'
            publish(Percentage=75);time.sleep(.8)
            assert not differs(absent,shot('recovered')),'Healthy battery must return to hover-only after recovery'
            assert shell.poll() is None
            print('PASS: Bluetooth five-second connection preview, battery updates/reload, hover-only details, reconnect, persistent low battery and recovery')
            raise SystemExit(0)
        if '--progress-outline-only' in sys.argv:
            def shot(name):
                run(['grim',str(out/('outline-'+name+'.png'))])
                return Image.open(out/('outline-'+name+'.png')).convert('RGB')
            original=(cfg/'config.toml').read_text()
            def configure(enabled,scale=1):
                move(1100,600)
                (cfg/'config.toml').write_text(original.replace('[island]',f'[island]\nouter_progress_ring={str(enabled).lower()}\nscale={scale}'))
                msg('config-reload');time.sleep(1.2)
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(publisher)
            def publish(value,app='test.desktop',visible=True):
                command(publisher,json.dumps({'uri':'application://'+app,'properties':{'progress':value,'progress-visible':visible}}));time.sleep(.7)
            move(1100,600);publish(.25);small=shot('default')
            configure(True);quarter=shot('quarter')
            assert ImageChops.difference(small.crop((580,8,700,12)),quarter.crop((580,8,700,12))).getbbox(), 'Outer track must appear'
            publish(.75);threequarters=shot('threequarters')
            assert quarter.getpixel((640,70)) != threequarters.getpixel((640,70)), 'Progress must travel around the bottom edge'
            assert ImageChops.difference(quarter.crop((505,23,540,57)),threequarters.crop((505,23,540,57))).getbbox() is None, 'Compact icon must no longer contain a progress ring'
            publish(0);zero=shot('zero');publish(1);full=shot('full')
            assert zero.getpixel((640,70)) != full.getpixel((640,70)), 'Zero and full progress must render correctly'
            publish(.25);publish(.75,'second.desktop');half=shot('multiple')
            assert half.getpixel((640,9)) == quarter.getpixel((640,9)), 'Multiple downloads keep the same outline track'
            publish(0,'second.desktop',False);publish(.75)
            move(640,40);time.sleep(.8);expanded=shot('expanded')
            publish(.25);expanded_quarter=shot('expanded-quarter')
            assert ImageChops.difference(expanded.crop((460,60,464,135)),expanded_quarter.crop((460,60,464,135))).getbbox(), 'Expanded outline follows progress on the rounded card'
            move(1100,600);time.sleep(.8)
            msg('theme-mode-set','light');time.sleep(.7);shot('light')
            configure(True,1.4);shot('scaled')
            msg('theme-mode-set','dark');configure(True)
            # Timer state wins over download and battery state and updates in place.
            msg('plugin','noctalia/timer:timer','all','PAUSED','75');time.sleep(1.3);timer=shot('timer')
            publish(.1);timer_unchanged=shot('timer-priority')
            assert ImageChops.difference(timer.crop((550,68,730,72)),timer_unchanged.crop((550,68,730,72))).getbbox() is None, 'Download progress must not replace the timer outline'
            msg('plugin','noctalia/timer:timer','all','PAUSED','25');time.sleep(1.3);timer_quarter=shot('timer-quarter')
            assert timer.getpixel((640,70)) != timer_quarter.getpixel((640,70)), 'Timer ticks must update the outline without rebuilding controls'
            msg('plugin','noctalia/timer:timer','all','IDLE','0');time.sleep(1.3)
            command(battery,json.dumps({'IsPresent':True,'Type':5,'PowerSupply':False,'State':0,'Percentage':75.}));time.sleep(.8)
            shot('download-battery');publish(0,visible=False);time.sleep(.8);battery_full=shot('battery')
            command(battery,json.dumps({'Percentage':25.}));time.sleep(.8);battery_quarter=shot('battery-quarter')
            assert battery_full.getpixel((640,70)) != battery_quarter.getpixel((640,70)), 'Battery percentage must drive the outline when other activities finish'
            run(['notify-send','-a','Outline test','-t','0','Priority','Notifications hide activity progress.']);time.sleep(.8);shot('notification')
            msg('notification-clear-active');msg('notification-clear-history');time.sleep(.8)
            steam=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_steam.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(steam)
            command(steam,'Running Update,Downloading,Staging,');time.sleep(2.5)
            spinning=shot('indeterminate');time.sleep(.3);spun=shot('indeterminate-moved')
            for frame in (spinning,spun): frame.paste((0,0,0),(570,14,710,66))
            assert ImageChops.difference(spinning.crop((420,8,860,72)),spun.crop((420,8,860,72))).getbbox(), 'Unknown download progress must travel around the outline'
            steam.terminate();steam.wait(timeout=6);time.sleep(2.5)
            configure(False);shot('disabled')
            # Exercise the actual toggle and persistence in Settings.
            # The toggle is on the island settings' Layout sub-page; click its row's switch.
            msg('settings-open','island');time.sleep(.8)
            move(*find_text('Layout','progress-settings-groups.png',min_x=420));click();time.sleep(.7);shot('settings')
            click_switch('Outer','progress-settings-layout.png');time.sleep(.8);shot('settings-enabled')
            data=tomllib.loads((base/'state/noctalia/settings.toml').read_text())
            assert data['island']['outer_progress_ring'] is True, data
            assert shell.poll() is None
            print('PASS: outer ring progress, expanded geometry, theme/scale, timer/download/battery priority, default mode and Settings persistence')
            raise SystemExit(0)
        if '--hover-editor-only' in sys.argv:
            def shot(name): run(['grim',str(out/('hover-editor-'+name+'.png'))])
            def saved():
                p=base/'state/noctalia/settings.toml'
                values=tomllib.loads(hover_config)['island'].copy()
                if p.exists(): values.update(tomllib.loads(p.read_text()).get('island',{}))
                return values
            def drag(x1,y1,x2,y2):
                move(x1,y1);command(pointer,'press');move(x1+12,y1+8);move(x2,y2)
                time.sleep(.2);shot('dragging');command(pointer,'release');time.sleep(.8)
            msg('settings-open','island');time.sleep(1)
            # Island settings are grouped into sub-pages; the editor is on Widgets, below the fold.
            try: widgets=find_text('Widgets','hover-editor-groups.png',min_x=420)
            except AssertionError:
                move(760,500);command(pointer,'scroll 10');time.sleep(.6)
                widgets=find_text('Widgets','hover-editor-groups.png',min_x=420)
            move(*widgets);click();time.sleep(.7);shot('settings')
            # Everything is located from the visible text: the three group headers, widget names
            # and buttons. Within a group card the drag handle sits 16 px left of a widget's name,
            # its gear and remove buttons 156 and 184 px right of the group header, and the arrow
            # row 27 px below the name (up, down, left, right at +15, +41, +67, +93).
            def column(name): return find_box(name,'hover-editor-columns.png',min_x=420)
            def item(name,min_x=420):
                left,top,width,height=find_box(name,'hover-editor-items.png',min_x=min_x,min_y=400)
                return left-16,top+height//2
            left_col,centre_col,right_col=column('Left'),column('Centre'),column('Right')
            header_y=centre_col[1]+centre_col[3]//2
            def press(word):
                move(*find_text(word,'hover-editor-buttons.png',min_x=420));click()
            original=saved()
            # Drop outside the editor cancels without changing settings.
            drag(*item('test_button'),1140,380)
            assert saved()==original, 'Outside drop must not change the layout'
            # Move a custom widget into the centre lane after the plugin.
            _,plugin_y=item('Hover')
            drag(*item('test_button'),centre_col[0]+60,plugin_y+30);shot('moved')
            assert saved()['hover_widgets']==[], saved()
            assert saved()['hover_widgets_center']==['test/hover:widget','test_button'], saved()
            # Reorder within a group, then drop into the now-empty left group.
            _,plugin_y=item('Hover')
            drag(*item('test_button'),centre_col[0]+60,plugin_y-8);shot('reordered')
            assert saved()['hover_widgets_center']==['test_button','test/hover:widget'], saved()
            drag(*item('test_button'),left_col[0]+60,header_y+40);shot('empty-drop')
            assert saved()['hover_widgets']==['test_button'], saved()
            assert saved()['hover_widgets_center']==['test/hover:widget'], saved()
            # Widget settings shortcuts must open an actual inspector for both kinds.
            _,button_y=item('test_button')
            move(left_col[0]+156,button_y);click();time.sleep(.8);shot('custom-settings');key(1);time.sleep(.5)
            _,plugin_y=item('Hover')
            move(centre_col[0]+156,plugin_y);click();time.sleep(.8);shot('plugin-settings')
            # The inspector puts each switch below its description.
            click_switch('Scroll','hover-editor-inspector.png',min_x=260,xs=(940,1000),below=70);time.sleep(.6)
            widget_data=tomllib.loads((base/'state/noctalia/settings.toml').read_text()).get('widget',{})
            assert widget_data['test/hover:widget']['enable_scroll'] is False, widget_data
            key(1);time.sleep(.7);shot('after-inspector')
            def layout_state():
                data=saved()
                return {**{key:data.get(key,[]) for key in ('hover_widgets','hover_widgets_center','hover_widgets_right')},
                        **{key:data.get(key,True) for key in ('hover_show_clock','hover_show_calendar','hover_show_media','hover_show_downloads','hover_show_timers','hover_show_batteries','hover_show_unread','hover_show_tray')}}
            before=layout_state()
            # Presets replace the hover choices together; undo restores custom and plugin references.
            for word,name,groups in [('Minimal','minimal',[[],['clock'],[]]),('Media','media',[['volume'],['media'],['audio_visualizer']]),('System','system',[['sysmon'],['network'],['battery']])]:
                press(word);time.sleep(.8);shot(name)
                current=saved()
                assert [current.get(key,[]) for key in ('hover_widgets','hover_widgets_center','hover_widgets_right')]==groups, (name,current)
                assert current.get('enabled') is True and current.get('hover_show_calendar') is False
                press('Undo');time.sleep(.8);shot(name+'-undo')
                assert layout_state()==before, (name,layout_state(),before)
                assert tomllib.loads((base/'state/noctalia/settings.toml').read_text())['widget']['test/hover:widget']['enable_scroll'] is False
            # The shared picker appends to the selected group, including its existing items.
            centre_col=column('Centre');header_y=centre_col[1]+centre_col[3]//2
            move(centre_col[0]+188,header_y);click();time.sleep(.7);shot('picker')
            # The picker opens with its search field focused.
            for code in (46,38,24,46,37): key(code)  # clock
            time.sleep(.5);shot('picker-filtered');key(28);time.sleep(.8);shot('added-clock')
            assert saved()['hover_widgets_center']==['test/hover:widget','clock'], saved()
            # Arrow buttons provide the same moves as dragging.
            _,plugin_y=item('Hover')
            move(centre_col[0]+93,plugin_y+27);click();time.sleep(.8);shot('arrow-moved')
            assert saved()['hover_widgets_center']==['clock'], saved()
            assert saved()['hover_widgets_right']==['right_button','test/hover:widget'], saved()
            right_col=column('Right')
            _,plugin_y=item('Hover',min_x=right_col[0]-30)
            move(right_col[0]+184,plugin_y);click();time.sleep(.8)
            assert saved()['hover_widgets_right']==['right_button'], saved()
            # Plugin adds use the normal named-instance workflow and keep existing entries.
            move(right_col[0]+188,right_col[1]+right_col[3]//2);click();time.sleep(.7)
            for code in (35,24,47,18,19): key(code)  # hover
            time.sleep(.5);shot('plugin-picker-filtered');key(28);time.sleep(.8);shot('added-plugin')
            data=tomllib.loads((base/'state/noctalia/settings.toml').read_text())
            added=saved()['hover_widgets_right']
            assert added[0]=='right_button' and len(added)==2, data
            assert data['widget'][added[1]]['type']=='test/hover:widget', data
            print('PASS: hover editor drag/reorder/cancel, inspectors, presets/undo, arrows, removal and built-in/plugin picker')
            raise SystemExit(0)
        if '--hover-layout-only' in sys.argv:
            def shot(name): run(['grim',str(out/('hover-layout-'+name+'.png'))])
            def plugin_state():
                try: return json.loads((plugin/'state.json').read_text())
                except (FileNotFoundError,json.JSONDecodeError): return {}
            def configure(options, name, source=hover_config):
                move(1100,600)
                (cfg/'config.toml').write_text(source.replace('[island]','[island]\n'+options))
                time.sleep(1.2);move(640,40);time.sleep(.8);shot(name)
            def plugin_click(y):
                # The expected row is a hint; sections above it change height, so click where OCR finds the widget.
                before=plugin_state().get('clicks',0)
                try: x,y=find_text('Plugin','hover-layout-plugin.png',min_x=400)
                except AssertionError: x=640
                move(x,y);click()
                wait(lambda:plugin_state().get('clicks',0)==before+1,'Centre plugin click')
            hidden='hover_show_clock=false\nhover_show_calendar=false\n'
            if '--settings-only' not in sys.argv:
                configure('', 'groups')
                plugin_click(165)
                move(520,165);click();wait(lambda:(base/'widget-clicked').exists(),'Left group click')
                move(778,165);click();wait(lambda:(base/'right-clicked').exists(),'Right group click')
                configure('hover_show_calendar=false', 'clock-only');plugin_click(101)
                configure('hover_show_clock=false', 'calendar-only');plugin_click(101)
                configure(hidden, 'widgets-only');plugin_click(37)
                configure(hidden, 'centre-only', hover_config.replace('hover_widgets=["test_button"]','hover_widgets=[]').replace('hover_widgets_right=["right_button"]','hover_widgets_right=[]'))
                plugin_click(37)
                (base/'right-clicked').unlink()
                configure(hidden, 'right-only', hover_config.replace('hover_widgets=["test_button"]','hover_widgets=[]').replace('hover_widgets_center=["test/hover:widget"]','hover_widgets_center=[]'))
                move(778,37);click();wait(lambda:(base/'right-clicked').exists(),'Right-only group click')
                # Disabled built-in sections must leave the widget row accessible.
                run(['notify-send','-a','Hover test','-t','0','Unread item','Hover sections'])
                msg('notification-clear-active')
                configure(hidden+'hover_show_unread=false', 'unread-hidden');plugin_click(37)
                configure(hidden, 'unread-visible');plugin_click(69)
                msg('notification-clear-history')
                command(battery,json.dumps({'IsPresent':True,'Type':5,'PowerSupply':False,'Percentage':42.}))
                configure(hidden+'hover_show_batteries=false', 'battery-hidden');plugin_click(37)
                configure(hidden, 'battery-visible');plugin_click(97)
                command(battery,json.dumps({'IsPresent':False}))
                env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
                player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'hover-layout-player.log')
                time.sleep(1.5)
                configure(hidden+'hover_show_media=false', 'media-hidden');plugin_click(37)
                configure(hidden, 'media-visible');plugin_click(219)
                player.terminate();player.wait(timeout=6)
                publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
                processes.append(publisher)
                command(publisher,json.dumps({'uri':'application://test.desktop','properties':{'progress-visible':True,'progress':.42}}))
                configure(hidden+'hover_show_downloads=false', 'downloads-hidden');plugin_click(37)
                configure(hidden, 'downloads-visible');plugin_click(186)
                publisher.terminate();publisher.wait(timeout=6)
            configure(hidden, 'restored')
            msg('settings-open','island');time.sleep(1);shot('settings')
            # The section switches are on the Hover Sections sub-page, below the fold.
            try: sections=find_text('Sections','hover-layout-groups.png',min_x=420)
            except AssertionError:
                move(760,500);command(pointer,'scroll 10');time.sleep(.6)
                sections=find_text('Sections','hover-layout-groups.png',min_x=420)
            move(*sections);click();time.sleep(.7);shot('section-settings')
            click_switch('Clock','hover-layout-sections.png');time.sleep(.7);shot('section-settings-edited')
            settings_path=base/'state/noctalia/settings.toml'
            saved=tomllib.loads(settings_path.read_text()).get('island',{}) if settings_path.exists() else {}
            assert saved.get('hover_show_clock') is True, ('Section toggle must save the clock setting',saved)
            move(827,273);click();time.sleep(.7);shot('widget-settings')
            assert shell.poll() is None
            print('PASS: hover section and placement Settings controls' if '--settings-only' in sys.argv else 'PASS: hover group placement and clicks, optional sections, widgets-only view and Settings controls')
            raise SystemExit(0)
        if '--hover-widgets-only' in sys.argv:
            def shot(name): run(['grim',str(out/('hover-widgets-'+name+'.png'))])
            def plugin_state():
                try: return json.loads((plugin/'state.json').read_text())
                except (FileNotFoundError,json.JSONDecodeError): return {}
            move(1100,600);shot('compact')
            move(640,40);time.sleep(2);shot('expanded')
            # The first row starts below the idle calendar (64 + 72 pixels).
            move(545,165);click();time.sleep(.4);shot('clicked')
            assert plugin_state().get('clicks') == 1, 'Plugin click handler must run'
            time.sleep(1.3)
            assert plugin_state().get('clicks') == 1, 'Clock refreshes must retain the plugin runtime'
            move(615,165);click();wait(lambda:(base/'widget-clicked').exists(),'custom command widget click')
            move(660,165);click();time.sleep(1);shot('audio-panel')
            msg('panel-close');move(1100,600);time.sleep(1)  # Close the Island-hosted panel explicitly.
            before=plugin_state().get('ticks',0);time.sleep(.7)
            assert plugin_state().get('ticks',0)==before, 'Hidden widgets must stop their runtime'
            move(640,40);time.sleep(1);shot('reopened')
            assert plugin_state().get('ticks',0)>before, 'Hover must restart plugin widgets'
            # Reorder and remove through the same persisted config used by settings.
            changed=hover_config.replace('["test/hover:widget","test_button","volume","clock"]','["clock","test_button","test/hover:widget"]')
            (cfg/'config.toml').write_text(changed);time.sleep(1.5)
            move(1100,600);move(640,40);time.sleep(1);shot('reordered')
            (cfg/'config.toml').write_text(changed.replace('["clock","test_button","test/hover:widget"]','[]'));time.sleep(1.5)
            move(1100,600);move(640,40);time.sleep(1);shot('empty')
            # Disabling a configured plugin must remove its runtime without deleting the list item.
            (cfg/'config.toml').write_text(hover_config.replace('enabled=["test/hover"]','enabled=[]'));time.sleep(1.5)
            move(1100,600);move(640,40);time.sleep(1)
            before=plugin_state().get('ticks',0);time.sleep(.7)
            assert plugin_state().get('ticks',0)==before, 'Disabled plugin must not run'
            shot('plugin-disabled')
            names=['item'+str(i) for i in range(60)]
            overflow=hover_config.replace('["test/hover:widget","test_button","volume","clock"]',json.dumps(names))
            overflow+=''.join('\n[widget.'+name+']\ntype="custom_button"\nlabel="Item '+str(i)+'"\n' for i,name in enumerate(names))
            (cfg/'config.toml').write_text(overflow);time.sleep(1.5)
            move(1100,600);move(640,40);time.sleep(1);shot('overflow-top')
            move(640,560);command(pointer,'scroll 8');time.sleep(.7);shot('overflow-scrolled')
            a=Image.open(out/'hover-widgets-overflow-top.png').convert('RGB')
            b=Image.open(out/'hover-widgets-overflow-scrolled.png').convert('RGB')
            assert ImageChops.difference(a.crop((465,145,815,690)),b.crop((465,145,815,690))).getbbox(), 'Overflow widgets must scroll'
            (cfg/'config.toml').write_text(hover_config);time.sleep(1.5)
            msg('settings-open','island');time.sleep(1);shot('settings')
            move(827,273);click();time.sleep(.6);shot('widget-settings')
            msg('settings-close')
            msg('settings-open-widget','island','test_button');time.sleep(1);shot('widget-inspector');msg('settings-close')
            assert shell.poll() is None
            print('PASS: hover modules, plugin/custom clicks, retained state, cleanup, reload, disable and overflow')
            raise SystemExit(0)
        if '--visualizer-only' in sys.argv:
            import math, struct, wave
            tone=base/'tone.wav'
            with wave.open(str(tone),'wb') as wav:
                wav.setparams((1,2,48000,0,'NONE','not compressed'))
                wav.writeframes(b''.join(struct.pack('<h',int(12000*math.sin(2*math.pi*440*i/48000))) for i in range(48000*30)))
            env['ISLAND_TEST_ART']='file://'+str(REPO/'assets/noctalia-wallpaper.png')
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'visualizer-player.log')
            move(1100,600);time.sleep(5)
            def shot(name):
                path=out/('visualizer-'+name+'.png');run(['grim',str(path)])
                return Image.open(path).convert('RGB').crop((700,16,790,65))
            silent=shot('silent')
            sound=start(['paplay','--device=island-test',str(tone)],'visualizer-audio.log')
            time.sleep(2)
            active=shot('active')
            assert ImageChops.difference(silent,active).getbbox(), 'Visualizer must react to audio'
            def spectrum_count():
                return sum(node.get('info',{}).get('props',{}).get('application.name')=='Noctalia Spectrum'
                           for node in json.loads(run(['pw-dump'])))
            assert spectrum_count()==1, 'Compact media should own one spectrum stream'
            # Rebuilding or lending the surface must release and restore the audio listener.
            move(640,40);time.sleep(.8);shot('expanded')
            assert spectrum_count()==0, 'Expanded media must release the compact visualizer'
            move(1100,600);time.sleep(1);shot('returned')
            assert spectrum_count()==1
            msg('panel-open','control-center');time.sleep(1)
            assert spectrum_count()==0, 'A hosted panel must release the visualizer'
            msg('panel-close');time.sleep(1)  # Close the Island-hosted panel explicitly.
            assert spectrum_count()==1, 'Returning from a panel must restore the visualizer'
            sound.terminate();sound.wait(timeout=5);time.sleep(3)
            quiet=shot('quiet')
            assert ImageChops.difference(active,quiet).getbbox(), 'Visualizer must settle when audio stops'
            player.terminate();player.wait(timeout=5);time.sleep(1)
            shot('stopped')
            assert spectrum_count()==0, 'Removing the player must release the audio stream'
            assert shell.poll() is None
            print('PASS: live audio visualizer, silence, media expansion and player removal')
            raise SystemExit(0)
        if '--polish-only' in sys.argv:
            def shot(name): run(['grim',str(out/('polish-'+name+'.png'))])
            def state(name):
                try: return json.loads((plugin_root/name/'test-state.json').read_text()) or {}
                except (FileNotFoundError,json.JSONDecodeError): return {}
            def dispatch(plugin,event,*payload): msg('plugin',plugin+':island-test','all',event,*payload)
            wait(lambda:state('timer').get('state')=='IDLE' and 'isRunning' in state('pomodoro'),'Timer engines ready')
            command(battery,json.dumps({'IsPresent':True,'Type':5,'PowerSupply':False,'Percentage':42.}))
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,
                                       stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(publisher)
            def download(index,progress):
                command(publisher,json.dumps({'uri':f'application://polish-long-download-name-{index}-with-extra-text.desktop',
                    'properties':{'progress':progress,'progress-visible':True}}))
            for index in range(4): download(index,.25)
            dispatch('noctalia/timer','start','600');dispatch('thepunkoff/pomodoro','toggle')
            time.sleep(1.2);move(1100,600);shot('compact');move(640,40);time.sleep(1);shot('busy-top')
            # Separating timers from downloads keeps the scaled card within the output.
            before=Image.open(out/'polish-busy-top.png').convert('RGB')
            assert before.getpixel((640,715)) != before.getpixel((640,650)), 'Island must leave clearance at the bottom'
            # Progress updates must preserve the tab row and selected download card.
            download(0,.65);time.sleep(.3);shot('progress')
            after=Image.open(out/'polish-progress.png').convert('RGB')
            assert ImageChops.difference(before.crop((410,15,870,61)),after.crop((410,15,870,61))).getbbox() is None, 'Progress preserves the activity tabs'
            # Reverse Tab reaches Close; reopening allows keyboard switching to Timers.
            msg('island-focus');time.sleep(.7);shot('keyboard-first')
            key('shift-tab');time.sleep(.5);shot('keyboard-last');key(28);time.sleep(.7);shot('closed')
            closed=Image.open(out/'polish-closed.png').convert('RGB')
            assert ImageChops.difference(after.crop((450,200,830,680)),closed.crop((450,200,830,680))).getbbox(), 'Keyboard Close collapses the crowded view'
            msg('island-focus');time.sleep(.6);key(15);key(28);time.sleep(.5);key(15);key(28)
            wait(lambda:state('timer').get('state')=='PAUSED','Keyboard Timers tab reaches Pause after reopening')
            key(1);assert shell.poll() is None
            print('PASS: scaled activity tabs, bounded cards, stable progress updates, keyboard reveal/close/switch/pause')
            raise SystemExit(0)
        if '--timer-only' in sys.argv:
            def shot(name): run(['grim',str(out/('timer-'+name+'.png'))])
            def state(name):
                try: return json.loads((plugin_root/name/'test-state.json').read_text()) or {}
                except (FileNotFoundError,json.JSONDecodeError): return {}
            def dispatch(plugin,event,*payload): msg('plugin',plugin+':island-test','all',event,*payload)
            wait(lambda: state('timer').get('state')=='IDLE' and 'isRunning' in state('pomodoro'),'Both plugin services ready')
            move(1100,600);shot('idle');move(640,40);time.sleep(.8);shot('entry-points')
            timer_config=(cfg/'config.toml').read_text()
            (cfg/'config.toml').write_text(timer_config.replace('[island]','[island]\nhover_show_timers=false'))
            time.sleep(1.2);move(1100,600);move(640,40);time.sleep(.8);shot('shortcuts-hidden')
            idle_footer=Image.open(out/'timer-idle.png').convert('RGB').crop((490,160,790,220))
            hidden_footer=Image.open(out/'timer-shortcuts-hidden.png').convert('RGB').crop((490,160,790,220))
            assert ImageChops.difference(idle_footer,hidden_footer).getbbox() is None, 'Hidden timer shortcuts must leave no footer'
            (cfg/'config.toml').write_text(timer_config);time.sleep(1.2);move(1100,600);move(640,40);time.sleep(.8)
            move(550,160);click();time.sleep(.8);shot('panel')
            move(640,164);click()  # Give the duration input explicit pointer focus.
            key(3);key(11);key(11);key(28)  # Enter 200 (2:00) in the actual plugin panel.
            shot('panel-started')
            wait(lambda:state('timer').get('state')=='RUNNING','Timer starts from its panel');key(1);move(1100,600);time.sleep(1.1)
            shot('running');move(640,40);time.sleep(.8);shot('hover')
            (cfg/'config.toml').write_text(timer_config.replace('[island]','[island]\nhover_show_timers=false'))
            time.sleep(1.2);move(1100,600);move(640,40);time.sleep(.8);shot('running-hidden')
            hidden_footer=Image.open(out/'timer-running-hidden.png').convert('RGB').crop((490,160,790,220))
            assert ImageChops.difference(idle_footer,hidden_footer).getbbox() is None, 'Hidden running timer must leave no footer'
            assert state('timer').get('state')=='RUNNING', 'Hiding timer controls must not stop the timer'
            (cfg/'config.toml').write_text(timer_config);time.sleep(1.2);move(1100,600);move(640,40);time.sleep(.8)
            move(536,207);click();wait(lambda:state('timer').get('state')=='PAUSED','Island pause reaches plugin')
            before=state('timer')['remaining'];time.sleep(1.3);assert state('timer')['remaining']==before
            shot('paused');click();wait(lambda:state('timer').get('state')=='RUNNING','Island resume reaches plugin')
            time.sleep(1.1);assert state('timer')['remaining']<before
            dispatch('thepunkoff/pomodoro','toggle');wait(lambda:state('pomodoro').get('isRunning') is True,'Concurrent Pomodoro start')
            time.sleep(1.1);shot('both-hover')
            move(536,207);click();wait(lambda:state('timer').get('state')=='PAUSED','Pause plain timer with both active')
            time.sleep(1.1);click();wait(lambda:state('timer').get('state')=='RUNNING','Timer controls stay in place after compact priority changes')
            assert state('pomodoro')['isRunning'] is True
            dispatch('thepunkoff/pomodoro','resetAll');time.sleep(1.1)
            move(640,207);click();wait(lambda:state('timer').get('state')=='IDLE','Island cancel reaches plugin')
            move(1100,600);time.sleep(1.1)
            dispatch('thepunkoff/pomodoro','toggle');wait(lambda:state('pomodoro').get('isRunning') is True,'Pomodoro start')
            time.sleep(1.1);shot('pomodoro');move(640,40);time.sleep(.8);shot('pomodoro-hover')
            move(536,207);click();wait(lambda:state('pomodoro').get('isRunning') is False,'Pomodoro pause from island')
            dispatch('thepunkoff/pomodoro','skip');wait(lambda:state('pomodoro')['sessionPtr']['stage']==2,'Pomodoro break transition')
            time.sleep(1.1);shot('break')
            move(640,207);click();wait(lambda:state('pomodoro').get('isDirty') is False,'Pomodoro cancel resets cycle')
            move(1100,600);dispatch('noctalia/timer','start','2')
            wait(lambda:state('timer').get('state')=='NOTIFY','Timer completes through upstream service')
            time.sleep(.7);shot('complete')
            # Plugin notifications are internal and intentionally do not enter history.
            before_toast=Image.open(out/'timer-idle.png').convert('RGB').crop((440,80,840,105))
            after_toast=Image.open(out/'timer-complete.png').convert('RGB').crop((440,80,840,105))
            assert ImageChops.difference(before_toast,after_toast).getbbox(), 'Completion toast must appear above the countdown'
            msg('notification-clear-active');msg('notification-clear-history')
            dispatch('noctalia/timer','RESET');time.sleep(1.1)
            dispatch('noctalia/timer','start','120');time.sleep(1.1)
            msg('plugins','disable','noctalia/timer');time.sleep(1.1);shot('disabled')
            idle=Image.open(out/'timer-idle.png').convert('RGB').crop((400,8,880,72))
            disabled=Image.open(out/'timer-disabled.png').convert('RGB').crop((400,8,880,72))
            for image in [idle,disabled]: image.paste((0,0,0),(180,0,300,64))
            assert ImageChops.difference(idle,disabled).getbbox() is None,'Disabled plugin must not leave a stale countdown'
            assert shell.poll() is None
            print('PASS: actual Timer/Pomodoro plugins, island pause/resume/cancel, break transition, completion notification and disabled-plugin cleanup')
            raise SystemExit(0)
        if '--privacy-only' in sys.argv:
            def shot(name): run(['grim',str(out/('privacy-'+name+'.png'))])
            def crop(name,bounds=(400,8,880,80)):
                result=Image.open(out/('privacy-'+name+'.png')).convert('RGB').crop(bounds)
                if bounds == (400,8,880,80): result.paste((0,0,0),(180,0,300,72))
                return result
            def differs(a,b,bounds=(400,8,880,80)):
                return ImageChops.difference(crop(a,bounds),crop(b,bounds)).getbbox()
            def capture(kind):
                source='Audio/Source' if kind=='mic' else 'Video/Source'
                consumer='Stream/Input/Audio' if kind=='mic' else 'Stream/Input/Video'
                p=start(['pw-loopback','-n','privacy-'+kind,'-c','1','-m','MONO',
                         '--capture-props',f'node.name=input-{kind} media.class={consumer} application.name="Privacy Test {kind}"',
                         '--playback-props',f'node.name=output-{kind} media.class={source} media.name="'+('Screen capture' if kind=='screen' else 'Test source')+'"'],
                        'privacy-'+kind+'.log')
                time.sleep(.8)
                assert p.poll() is None
                nodes=json.loads(run(['pw-dump']))
                for node in nodes:
                    props=node.get('info',{}).get('props',{})
                    if props.get('node.name') not in ('input-'+kind,'output-'+kind): continue
                    direction='Input' if props['node.name']=='input-'+kind else 'Output'
                    run(['pw-cli','set-param',str(node['id']),'PortConfig',
                         '{ direction = '+direction+' mode = dsp format = { mediaType = audio mediaSubtype = raw format = F32P rate = 48000 channels = 1 position = [ MONO ] } }'])
                time.sleep(.4)
                outputs=run(['pw-link','-o']).splitlines();inputs=run(['pw-link','-i']).splitlines()
                source_port=next(v.strip() for v in outputs if 'output-'+kind+':' in v)
                input_port=next(v.strip() for v in inputs if 'input-'+kind+':' in v)
                run(['pw-link',source_port,input_port]);time.sleep(2.5)
                return p
            move(1100,600);shot('idle')
            mic=capture('mic');shot('mic')
            assert differs('idle','mic'), 'Microphone capture must appear'
            move(640,40);time.sleep(.8);shot('mic-hover')
            move(1100,600);camera=capture('camera');screen=capture('screen');shot('all')
            move(640,40);time.sleep(.8);shot('all-hover')
            assert differs('mic-hover','all-hover',(400,135,880,330)), 'Each capture kind needs an expanded app row'
            move(1100,600)
            command(battery,json.dumps({'IsPresent':True,'Type':5,'PowerSupply':False,'Percentage':42.}))
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,
                                       stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(publisher)
            command(publisher,json.dumps({'uri':'application://test-browser.desktop',
                                          'properties':{'progress':.65,'progress-visible':True}}))
            time.sleep(.8);shot('combined');move(640,40);time.sleep(.8);shot('combined-hover')
            move(1100,600);publisher.terminate();publisher.wait(timeout=6)
            command(battery,json.dumps({'IsPresent':False}));time.sleep(.8)
            run(['notify-send','-a','Privacy test','-t','0','Test notification','Capture indicators remain visible.'])
            time.sleep(.8);shot('notification')
            msg('notification-clear-active');msg('notification-clear-history');time.sleep(.8)
            settings=cfg/'config.toml';original=settings.read_text()
            settings.write_text(original+'\n[shell.privacy]\nmic_filter_regex="Privacy Test mic"\n');msg('config-reload');time.sleep(1)
            # The compact Island cycles one indicator, so check the expanded row, which lists every kind.
            move(640,40);time.sleep(.8);shot('filtered-hover');move(1100,600);time.sleep(.8)
            assert differs('all-hover','filtered-hover',(400,135,880,180)), 'Existing privacy filters must apply'
            settings.write_text(original);msg('config-reload');time.sleep(.8)
            camera.terminate();camera.wait(timeout=6);screen.terminate();screen.wait(timeout=6);time.sleep(2.5)
            # Compact mic icon opens Noctalia's existing audio tab.
            move(724,40);click();time.sleep(.8);shot('audio-controls')
            assert differs('mic-hover','audio-controls',(400,90,880,330)), 'Microphone click must open the audio panel'
            msg('panel-close');move(1100,600)  # Close the Island-hosted panel explicitly.
            mic.terminate();mic.wait(timeout=6);time.sleep(2.5);shot('stopped')
            assert not differs('idle','stopped'), 'Stopping captures must remove all indicators'
            assert shell.poll() is None
            print('PASS: isolated PipeWire mic/camera/screen detection, hover details, filtering and capture cleanup')
            raise SystemExit(0)
        if '--battery-only' in sys.argv:
            def publish(**values): command(battery,json.dumps(values));time.sleep(.7)
            def shot(name): run(['grim',str(out/('battery-'+name+'.png'))])
            def crop(name, bounds=(400,8,880,64)):
                image=Image.open(out/('battery-'+name+'.png')).convert('RGB').crop(bounds)
                if bounds == (400,8,880,64): image.paste((0,0,0),(180,0,300,56))
                return image
            def differs(a,b,bounds=(400,8,880,64)):
                return ImageChops.difference(crop(a,bounds),crop(b,bounds)).getbbox()
            move(1100,600);shot('absent')
            publish(IsPresent=True);shot('normal')
            assert not differs('absent','normal'), 'A healthy discharging system pack stays quiet'
            move(640,40);time.sleep(.8);shot('normal-hover')
            publish(State=1);shot('charging-hover')
            assert differs('normal-hover','charging-hover',(440,135,840,215)), 'Hover must update battery status and time estimate'
            move(1100,600);time.sleep(.8);shot('charging')
            assert differs('absent','charging'), 'Charging ring must be visible'
            time.sleep(.5);shot('pulse')
            assert differs('charging','pulse',(714,20,758,62)), 'Charging ring must pulse'
            settings=cfg/'config.toml';original=settings.read_text()
            settings.write_text(original+'\n[shell.animation]\nenabled=false\n');msg('config-reload');time.sleep(.8)
            shot('reduced');time.sleep(.5);shot('reduced-later')
            assert not differs('reduced','reduced-later',(714,20,758,62)), 'Disabled animations must stop the charging pulse'
            settings.write_text(original);msg('config-reload');time.sleep(.7)
            publish(State=2,Percentage=5.)
            # Dismiss the existing low-battery notification so we can inspect the ring underneath.
            msg('notification-clear-active');msg('notification-clear-history');time.sleep(.8);shot('low')
            publish(Percentage=9.);msg('notification-clear-active');msg('notification-clear-history');time.sleep(.6);shot('low-updated')
            assert differs('low','low-updated',(714,20,758,62)), 'Ring fill must track battery percentage'
            publish(State=4,Percentage=100.);shot('full')
            assert not differs('absent','full'), 'A fully charged system pack should leave the compact island'
            publish(Type=5,PowerSupply=False,State=0,Percentage=42.);shot('mouse')
            assert differs('absent','mouse'), 'Peripheral charge must be visible on desktop'
            move(640,40);time.sleep(.8);shot('mouse-hover')
            msg('theme-mode-set','light');time.sleep(.8);shot('mouse-light')
            msg('theme-mode-set','dark');move(1100,600);time.sleep(.8)
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'battery-player.log')
            time.sleep(2);assert player.poll() is None
            shot('media');move(640,40);time.sleep(.8);shot('media-hover')
            assert differs('media-hover','mouse-hover',(440,75,840,260)), 'Media must remain visible above the battery row'
            move(1100,600)
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,
                                       stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(publisher)
            command(publisher,json.dumps({'uri':'application://test-browser.desktop',
                                          'properties':{'progress':.65,'progress-visible':True}}))
            time.sleep(.8);shot('download');move(640,40);time.sleep(.8);shot('download-hover')
            move(1100,600);publisher.terminate();publisher.wait(timeout=6)
            player.terminate();player.wait(timeout=6)
            run(['notify-send','-a','Battery test','-t','0','Battery priority','Notifications still take priority.'])
            time.sleep(.7);shot('notification');msg('notification-clear-active');time.sleep(.7);shot('unread')
            msg('notification-clear-history');time.sleep(.7)
            move(1100,600);publish(IsPresent=False);shot('removed')
            assert not differs('absent','removed'), 'Disconnecting the battery must restore the original island'
            assert shell.poll() is None
            print('PASS: battery absence, quiet healthy pack, charge ring/pulse, hover details, low charge, full charge and peripheral disconnect')
            raise SystemExit(0)
        if '--downloads-only' in sys.argv:
            import struct
            move(1100,600)
            run(['grim',str(out/'download-empty.png')])
            publisher=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_downloads.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(publisher)
            def publish(properties, app='test-browser.desktop'):
                command(publisher,json.dumps({'uri':'application://'+app,'properties':properties}));time.sleep(.6)
            def shot(name): run(['grim',str(out/name)])
            def capsule(name):
                image=Image.open(out/name).convert('RGB').crop((400,8,880,220))
                image.paste((0,0,0),(180,0,300,55))  # Ignore minute changes in the centred clock.
                return image
            publish({'count':7,'count-visible':True})
            shot('download-badge-only.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-badge-only.png')).getbbox() is None, 'Badge-only messages must not become downloads'
            publish({'progress':.25,'progress-visible':True})
            shot('download-compact.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-compact.png')).getbbox(), 'Transfer must appear'
            publish({'progress':.65})
            shot('download-updated.png')
            assert ImageChops.difference(capsule('download-compact.png'),capsule('download-updated.png')).getbbox(), 'Partial progress update must retain visibility'
            def ring_crop(name): return Image.open(out/name).convert('RGB').crop((502,20,542,60))
            assert ImageChops.difference(ring_crop('download-compact.png'),ring_crop('download-updated.png')).getbbox(), 'Circular fill must reflect reported progress'
            publish({'progress':'invalid'})
            shot('download-invalid.png')
            assert ImageChops.difference(capsule('download-updated.png'),capsule('download-invalid.png')).getbbox() is None, 'Malformed progress must not corrupt state'
            move(640,40);time.sleep(.7);shot('download-expanded.png')
            publish({'progress':.4,'progress-visible':True},'second-app.desktop')
            shot('download-multiple.png')
            assert ImageChops.difference(capsule('download-expanded.png'),capsule('download-multiple.png')).getbbox(), 'Multiple apps need separate rows'
            move(1100,600);time.sleep(.7)
            msg('island-focus');time.sleep(.7);key(1);time.sleep(.7)
            run(['notify-send','-a','Download test','-t','0','Notification priority','Downloads resume after dismissal.']);time.sleep(.7)
            shot('download-notification.png');msg('notification-clear-active');time.sleep(.7)
            shot('download-restored.png')
            publish({'progress-visible':False})
            publisher.terminate();publisher.wait(timeout=6);time.sleep(.7)
            msg('notification-clear-history');time.sleep(.7);shot('download-disconnected.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-disconnected.png')).getbbox() is None, 'Disconnected publishers must leave no stale progress'
            # Exercise the actual Zen native-messaging framing, including split reads.
            host=subprocess.Popen([sys.executable,str(REPO/'scripts/zen-download-progress.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            processes.append(host)
            def native(count,progress):
                payload=json.dumps(f'{count}:{progress}').encode();packet=struct.pack('=I',len(payload))+payload
                host.stdin.write(packet[:2]);host.stdin.flush();time.sleep(.1)
                host.stdin.write(packet[2:]);host.stdin.flush();time.sleep(.7)
            native(1,0);shot('download-zen-start.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-zen-start.png')).getbbox(), 'Zen transfer at zero must be visible'
            native(1,.72)
            shell.terminate();shell.wait(timeout=6)
            shell=start([binary],'noctalia-download-restart.log');wait(ready,'Restarted shell IPC');time.sleep(1)
            shot('download-zen-restarted.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-zen-restarted.png')).getbbox(), 'Zen bridge must resend progress after shell restart'
            move(640,40);time.sleep(.7);shot('download-zen-expanded.png')
            move(1100,600);native(0,0);time.sleep(.7);shot('download-zen-finished.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-zen-finished.png')).getbbox() is None, 'Finished transfers must disappear'
            host.stdin.close();host.wait(timeout=6)
            assert host.returncode==0,host.stderr.read().decode()
            steam=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_steam.py')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(steam)
            command(steam,'Running Update,Downloading,Staging,');time.sleep(2.5)
            shot('download-steam-compact.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-steam-compact.png')).getbbox(), 'Steam download activity must appear'
            time.sleep(.3);shot('download-steam-spinning.png')
            assert ImageChops.difference(ring_crop('download-steam-compact.png'),ring_crop('download-steam-spinning.png')).getbbox(), 'Steam activity ring must rotate'
            move(640,40);time.sleep(.7);shot('download-steam-expanded.png')
            command(steam,'Running Update,Staging,');time.sleep(2.5);shot('download-steam-installing.png')
            assert ImageChops.difference(capsule('download-steam-expanded.png'),capsule('download-steam-installing.png')).getbbox(), 'Steam must show phase changes'
            command(steam,'Running Update,Stopping,');move(1100,600);time.sleep(2.5);shot('download-steam-paused.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-steam-paused.png')).getbbox() is None, 'Paused Steam activity must clear'
            command(steam,'Running Update,Downloading,');time.sleep(2.5)
            steam.terminate();steam.wait(timeout=6);time.sleep(2.5);shot('download-steam-exited.png')
            assert ImageChops.difference(capsule('download-empty.png'),capsule('download-steam-exited.png')).getbbox() is None, 'Exiting Steam must clear activity'
            assert shell.poll() is None
            print('PASS: desktop progress, Zen bridge/restart recovery, Steam phases, pause and process-exit cleanup')
            raise SystemExit(0)
        if '--keyboard-only' not in sys.argv:
            msg('settings-open','island')
            time.sleep(1)
            # Island settings are grouped into sub-pages; open General and flip "Reserve desktop space"
            # at its row's switch. Matches are limited to the content pane, right of the sidebar.
            move(*find_text('General','settings.png',min_x=420));click();time.sleep(.6)
            click_switch('Reserve','settings-general.png');time.sleep(.5)
            saved=tomllib.loads((base/'state/noctalia/settings.toml').read_text())
            assert saved['island']['reserve_space'] is False, saved
            move(*find_text('Back','settings-general-saved.png',min_x=420));click();time.sleep(.6)
            move(*find_text('Clock','settings-groups.png',min_x=420));click();time.sleep(.6)
            run(['grim',str(out/'clock-settings.png')])
            msg('settings-close')
            move(640,35); time.sleep(1)
            run(['grim',str(out/'calendar-default.png')])
            move(1100,600)
            path=cfg/'config.toml'
            text=path.read_text().replace('[island]','[island]\ncalendar_labels="initials"\nclock_offset=-12\nexpanded_clock_offset=-12\nmedia_artwork_size=80\nvolume_bar_height=24\nvolume_show_percentage=true')
            path.write_text(text)
            msg('config-reload');time.sleep(1)
            run(['grim',str(out/'compact-offset.png')])
            move(640,35);time.sleep(1)
            run(['grim',str(out/'calendar-initials.png')])
            move(1100,600)
            # The private session intentionally has no audio server. Volume layout
            # settings are covered by the schema tests; do not touch the real sink.
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            env['ISLAND_TEST_TITLE']='A little closer to home — a long track title that keeps scrolling as playback advances'
            env['ISLAND_TEST_TICK']='1'
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log')
            time.sleep(4)
            move(640,35);time.sleep(1)
            run(['grim',str(out/'media-large.png')])
            time.sleep(2.2)
            run(['grim',str(out/'media-scroll.png')])
            before=Image.open(out/'media-large.png').convert('RGB')
            after=Image.open(out/'media-scroll.png').convert('RGB')
            assert ImageChops.difference(before.crop((601,47,777,70)),after.crop((601,47,777,70))).getbbox(), 'Long title must keep scrolling across playback updates'
            assert ImageChops.difference(before.crop((505,150,570,165)),after.crop((505,150,570,165))).getbbox(), 'Playback time must keep updating'
            # Switching to a short title should restore a stationary label.
            move(707,187);click();time.sleep(1)
            run(['grim',str(out/'media-short.png')])
            time.sleep(1.2)
            run(['grim',str(out/'media-short-later.png')])
            short=Image.open(out/'media-short.png').convert('RGB').crop((601,47,777,70))
            later=Image.open(out/'media-short-later.png').convert('RGB').crop((601,47,777,70))
            assert ImageChops.difference(short,later).getbbox() is None, 'Short title must stay still'
            # The larger cover shifts playback controls down by 24 pixels.
            move(640,187);time.sleep(1)
            run(['grim',str(out/'media-hover-pause.png')])
            idle=Image.open(out/'media-short-later.png').convert('RGB')
            hovered=Image.open(out/'media-hover-pause.png').convert('RGB')
            assert ImageChops.difference(idle.crop((618,169,662,217)),hovered.crop((618,169,662,217))).getbbox(), 'Playback button must highlight on hover'
            assert ImageChops.difference(idle.crop((580,223,700,280)),hovered.crop((580,223,700,280))).getbbox(), 'Delayed tooltip must appear below playback button'
            command(pointer,'press');time.sleep(.2)
            run(['grim',str(out/'media-pressed.png')])
            move(780,190);command(pointer,'release');time.sleep(.2)
            assert 'PlayPause' not in (out/'player-actions.log').read_text(), 'Releasing outside the button must cancel activation'
            move(640,187);click();time.sleep(1)
            run(['grim',str(out/'media-hover-play.png')])
            events=(out/'player-actions.log').read_text()
            assert 'PlayPause' in events,events
            move(640,35);time.sleep(.5)
            move(640,138);command(pointer,'press');time.sleep(.3)
            run(['grim',str(out/'seek-preview-middle.png')])
            time.sleep(1.3)
            run(['grim',str(out/'seek-preview-held.png')])
            def time_crop(name): return Image.open(out/name).convert('RGB').crop((505,150,570,165))
            assert ImageChops.difference(time_crop('seek-preview-middle.png'),time_crop('seek-preview-held.png')).getbbox() is None, 'Preview must survive timer updates'
            move(721,138);time.sleep(.3)
            run(['grim',str(out/'seek-preview-later.png')])
            assert ImageChops.difference(time_crop('seek-preview-middle.png'),time_crop('seek-preview-later.png')).getbbox(), 'Preview must follow the drag'
            assert 'SetPosition' not in (out/'player-actions.log').read_text(), 'Dragging must not seek until release'
            command(pointer,'release');time.sleep(.5)
            seeks=[line for line in (out/'player-actions.log').read_text().splitlines() if line.startswith('SetPosition ')]
            assert len(seeks)==1,seeks
            target=ast.literal_eval(seeks[0].removeprefix('SetPosition '))[1]
            assert abs(target-174400000)<100000,target
            msg('theme-mode-set','light');time.sleep(1)
            run(['grim',str(out/'media-light.png')])
            # Source name shares the existing media-panel action.
            move(650,99);click();time.sleep(1)
            run(['grim',str(out/'media-panel.png')])
            # The Island-hosted panel may not have keyboard focus, so close it over IPC; Escape on
            # Hyprland is covered by the island privacy check.
            msg('panel-close');move(1100,600);time.sleep(.5)
            long_body='\n'.join(f'Line {i:02}: A complete notification stays readable when expanded.' for i in range(1,61))+'\nEND OF FULL MESSAGE'
            run(['notify-send','-a','Island test','-t','0','Long notification',long_body])
            time.sleep(1)
            run(['grim',str(out/'notification-collapsed.png')])
            # Let the pointer's arrival settle (hover enter) before clicking, as a person would.
            move(600,120);time.sleep(.6);click();time.sleep(1)
            run(['grim',str(out/'notification-expanded.png')])
            collapsed=Image.open(out/'notification-collapsed.png').convert('RGB')
            expanded=Image.open(out/'notification-expanded.png').convert('RGB')
            assert ImageChops.difference(collapsed.crop((440,400,840,620)),expanded.crop((440,400,840,620))).getbbox(), 'Long notification must expand'
            move(650,400);command(pointer,'scroll 100');time.sleep(1)
            run(['grim',str(out/'notification-scrolled.png')])
            # The complete body must survive D-Bus ingestion (formerly capped at 1 KiB).
            history=base/'state/noctalia/notification_history.json'
            wait(lambda: history.exists() and 'END OF FULL MESSAGE' in history.read_text(), 'Notification body retained in history')
            scrolled=Image.open(out/'notification-scrolled.png').convert('RGB')
            assert ImageChops.difference(expanded.crop((455,60,805,615)),scrolled.crop((455,60,805,615))).getbbox(), 'Full notification must scroll'
            move(784,28);click();time.sleep(1)
            run(['grim',str(out/'notification-recollapsed.png')])
            recollapsed=Image.open(out/'notification-recollapsed.png').convert('RGB')
            assert ImageChops.difference(collapsed.crop((440,400,840,620)),recollapsed.crop((440,400,840,620))).getbbox() is None, 'Collapse must restore compact height'
            move(819,28);time.sleep(1)
            run(['grim',str(out/'notification-dismiss-hover.png')])
            click();time.sleep(.5)
            move(1100,600)
            notification=start(['notify-send','-a','Island test','-t','0','--wait','--action=confirm=Mark as read','Short notification','Everything fits.'],'notification-action.log')
            time.sleep(1)
            run(['grim',str(out/'notification-short.png')])
            # As on macOS, hovering the banner shows its single action as a pill in place of the
            # time stamp, at the top right.
            move(640,70);time.sleep(1)
            run(['grim',str(out/'notification-action-hover.png')])
            move(770,31);time.sleep(.3);click()
            wait(lambda: 'confirm' in (out/'notification-action.log').read_text(),'Notification action invocation')
            move(1100,600)
            msg('notification-clear-active');msg('notification-clear-history')
            msg('notification-dnd-set','true');time.sleep(2.5)  # Let the DND OSD expire.
            run(['grim',str(out/'badge-empty.png')])
            for i in range(2):
                run(['notify-send','-a','Badge test','-t','1000',f'Unread message {i+1}','Saved quietly during Do Not Disturb.'])
            time.sleep(1.5)
            history_entries=lambda: json.loads(history.read_text())['entries']
            assert sum(not entry['seen'] for entry in history_entries())==2
            run(['grim',str(out/'badge-two.png')])
            empty=Image.open(out/'badge-empty.png').convert('RGB')
            counted=Image.open(out/'badge-two.png').convert('RGB')
            assert ImageChops.difference(empty.crop((686,28,710,52)),counted.crop((686,28,710,52))).getbbox(), 'Unread badge must appear'
            move(698,40);time.sleep(1)
            run(['grim',str(out/'badge-hover.png')])
            hovered=Image.open(out/'badge-hover.png').convert('RGB')
            assert ImageChops.difference(empty.crop((490,30,515,50)),hovered.crop((490,30,515,50))).getbbox() is None, 'Badge hover must keep the compact island still'
            click();time.sleep(1)
            run(['grim',str(out/'badge-history.png')])
            wait(lambda: all(entry['seen'] for entry in history_entries()),'Badge opens history and marks notifications seen')
            msg('panel-close');move(1100,600);time.sleep(1)  # Island-hosted; see the media panel above.
            run(['grim',str(out/'badge-cleared.png')])
            cleared=Image.open(out/'badge-cleared.png').convert('RGB')
            assert ImageChops.difference(empty.crop((686,28,710,52)),cleared.crop((686,28,710,52))).getbbox() is None, 'Read badge must disappear'
            for i in range(2):
                run(['notify-send','-a','Hover count test','-t','1000',f'Unread message {i+1}','Shown in the expanded island.'])
            time.sleep(1.5)
            move(640,40);time.sleep(1)
            run(['grim',str(out/'expanded-unread-count.png')])
            # During playback the expanded Island ends with its icon row; the unread bell is its lowest glyph.
            unread=Image.open(out/'expanded-unread-count.png').convert('RGB')
            bell_rows=[y for y in range(100,400) if max(unread.getpixel((560,y)))<12
                       and any(min(unread.getpixel((x,y)))>200 for x in range(632,648))]
            assert bell_rows,'Expanded unread bell missing'
            bell=[y for y in bell_rows if y>max(bell_rows)-20]
            move(640,(min(bell)+max(bell))//2);click();time.sleep(1)
            wait(lambda: all(entry['seen'] for entry in history_entries()),'Expanded unread bell opens history')
            # Polish pass at the normal artwork, clock and calendar sizes.
            msg('panel-close');move(1100,600)
            text=path.read_text().replace('calendar_labels="initials"','calendar_labels="abbreviated"').replace('clock_offset=-12','clock_offset=0').replace('media_artwork_size=80','media_artwork_size=56')
            path.write_text(text);msg('config-reload');msg('theme-mode-set','dark')
            msg('notification-dnd-set','false');time.sleep(2.5)
            msg('notification-clear-active');msg('notification-clear-history');time.sleep(.7)
            run(['grim',str(out/'polish-rest.png')])
            for _ in range(5):
                move(640,40);time.sleep(.08)
                move(1100,600);time.sleep(.1)
            move(640,40);time.sleep(.7)
            run(['grim',str(out/'polish-calendar.png')])
            move(1100,600);time.sleep(.7)
            run(['grim',str(out/'polish-rest-after-hover.png')])
            def lower_crop(name): return Image.open(out/name).convert('RGB').crop((450,85,830,250))
            assert ImageChops.difference(lower_crop('polish-rest.png'),lower_crop('polish-rest-after-hover.png')).getbbox() is None, 'Rapid hover must leave no expanded content behind'
            msg('media','play');time.sleep(.5)
            move(640,40);time.sleep(.7)
            run(['grim',str(out/'polish-media.png')])
            move(640,163);click();time.sleep(.7)
            run(['grim',str(out/'polish-media-paused.png')])
            move(650,55);click();time.sleep(.7)
            run(['grim',str(out/'polish-media-panel.png')])
            msg('panel-close');move(1100,600);time.sleep(.7)  # Island-hosted; see the media panel above.
            run(['grim',str(out/'polish-rest-after-panel.png')])
            assert ImageChops.difference(lower_crop('polish-rest.png'),lower_crop('polish-rest-after-panel.png')).getbbox() is None, 'Closing the panel must restore the compact island'
            run(['notify-send','-a','Polish test','-t','0','First card','Start a click here.']);time.sleep(.7)
            move(640,60);command(pointer,'press')
            run(['notify-send','-a','Polish test','-t','0','Replacement card','A new card must not inherit the held click.']);time.sleep(.7)
            command(pointer,'release');time.sleep(.7)
            run(['grim',str(out/'polish-replaced-during-click.png')])
            assert history_entries() and all(not entry['seen'] for entry in history_entries()), 'Replacing a notification during a click must not open history'
        else:
            env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS']=str(out/'player-actions.log')
            player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log')
            time.sleep(2);msg('media','pause');time.sleep(.5)
        # Explicit keyboard focus keeps timed notifications alive, and Tab
        # reaches their actions without moving the pointer into the island.
        move(1100,600);msg('notification-clear-active')
        run(['notify-send','-a','Keyboard test','-t','1200','Timed notification','Stay visible while keyboard focused.'])
        assert msg('island-focus').strip()=='ok';time.sleep(.7)
        run(['grim',str(out/'keyboard-timed-before.png')]);time.sleep(1.5)
        run(['grim',str(out/'keyboard-timed-after.png')])
        # The text area only: the close button's focus highlight can land between the two shots.
        before_image=Image.open(out/'keyboard-timed-before.png').convert('RGB').crop((460,10,790,100))
        after_image=Image.open(out/'keyboard-timed-after.png').convert('RGB').crop((460,10,790,100))
        assert ImageChops.difference(before_image,after_image).getbbox() is None, 'Focused notification must not expire'
        key(1);time.sleep(.5)
        keyboard_notification=start(['notify-send','-a','Keyboard test','-t','0','--wait','--action=confirm=Confirm','Keyboard notification','Tab to Confirm, then Enter.'],'keyboard-notification.log')
        time.sleep(.3);assert msg('island-focus').strip()=='ok';time.sleep(1.5)
        assert keyboard_notification.poll() is None, 'Notification must await keyboard action'
        run(['grim',str(out/'keyboard-notification.png')])
        key(15);key(28)
        wait(lambda: 'confirm' in (out/'keyboard-notification.log').read_text(),'Keyboard notification action')
        msg('notification-clear-active');msg('notification-clear-history');time.sleep(.5)
        # Paused media can also be opened explicitly. Focus survives the
        # playback-status rebuild, including reverse Tab navigation.
        assert msg('island-focus').strip()=='ok';time.sleep(.7)
        key(15)
        events_path=out/'player-actions.log'
        before=events_path.read_text().count('PlayPause')
        key(28);time.sleep(.7)
        assert events_path.read_text().count('PlayPause')==before+1, 'Tab must reach playback'
        run(['grim',str(out/'keyboard-media.png')])
        key(28);time.sleep(.7)
        assert events_path.read_text().count('PlayPause')==before+2, 'Playback focus must survive status updates'
        key(15);command(keyboard,'shift-tab');key(28);time.sleep(.5)
        assert events_path.read_text().count('PlayPause')==before+3, 'Shift+Tab must return to playback'
        key(1);time.sleep(.7)
        stopped=events_path.read_text();key(28);time.sleep(.3)
        assert events_path.read_text()==stopped, 'Escape must release island keyboard control'
        run(['grim',str(out/'keyboard-released.png')])
        # A new automatic notification must never inherit media keyboard focus.
        msg('island-focus');time.sleep(.7)
        replacement=start(['notify-send','-a','Keyboard test','-t','0','--wait','--action=confirm=Confirm','Automatic notification','This card must not inherit focus.'],'keyboard-replacement.log')
        time.sleep(.7);key(15);key(28);time.sleep(.3)
        assert replacement.poll() is None, 'Incoming notification must release previous keyboard focus'
        msg('island-focus');time.sleep(.7);key(1);time.sleep(.5)
        wait(lambda: replacement.poll() is not None,'Escape dismisses focused notification')
        player.terminate();player.wait(timeout=6);time.sleep(1)
        msg('island-focus');time.sleep(.7)
        run(['grim',str(out/'keyboard-calendar.png')])
        key(1);time.sleep(.7)
        assert shell.poll() is None
        print('PASS: media, notifications, unread bell, transitions, keyboard actions, focus preservation and release')
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.terminate()
                try:p.wait(timeout=6)
                except subprocess.TimeoutExpired:p.kill();p.wait()
