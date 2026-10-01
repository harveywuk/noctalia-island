#!/usr/bin/env python3
"""Hyprland 0.56 Lua integration on two private, GPU-backed virtual outputs."""
import os, pathlib, subprocess, tempfile, time, json, re, sys, tomllib
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
        if '--island-routing-only' in sys.argv or '--island-cupertino-only' in sys.argv:
            bluetooth=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_bluetooth.py')],env=env,
                                       stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(bluetooth)
            assert bluetooth.stdout.readline().strip()=='ok'
        if '--island-cupertino-only' in sys.argv:
            battery=subprocess.Popen([sys.executable,str(REPO/'tests/fixtures/island_battery.py')],env=env,
                                     stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(battery)
            assert battery.stdout.readline().strip()=='ok'
            from island_cupertino_smoke import prepare
            prepare(base,cfg,env)
        if '--startup-only' in sys.argv:
            from startup_apps_smoke import prepare
            prepare(base,cfg,env)
        if '--default-apps-only' in sys.argv:
            from default_apps_smoke import prepare
            prepare(base,cfg,env)
        if '--cupertino-only' in sys.argv:
            from cupertino_smoke import prepare
            prepare(base,cfg,env)
        if '--dock-preview-only' in sys.argv:
            from dock_preview_smoke import prepare
            prepare(base,cfg,env)
        if '--dock-preview-perf-only' in sys.argv:
            from dock_preview_perf_smoke import prepare
            prepare(base,cfg,env)
        if '--lockscreen-only' in sys.argv:
            from lockscreen_smoke import prepare
            prepare(base,cfg,env)
        if '--island-launcher-only' in sys.argv:
            from island_launcher_smoke import prepare
            prepare(base,cfg,env)
        if '--starter-only' in sys.argv:
            from starter_smoke import prepare
            prepare(base,cfg,env)
        if '--dock-motion-only' in sys.argv:
            from dock_motion_smoke import prepare
            prepare(base,cfg,env)
        if '--text-fit-only' in sys.argv:
            from text_fit_smoke import prepare
            prepare(base,cfg,env)
        binary=str(REPO/'build-rishot/noctalia')
        if '--performance-only' in sys.argv:
            import shutil
            immutable=base/'noctalia-benchmark'
            shutil.copy2(os.environ.get('NOCTALIA_TEST_BINARY',binary),immutable)
            binary=str(immutable)
        shell=start([binary],'noctalia.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'Noctalia start')
        def msg(*args):
            reply=run([binary,'msg',*args]).strip()
            assert not reply.startswith('error'),reply
            return reply
        wait(lambda:msg('record-status')=='idle','Noctalia IPC')
        if '--island-cupertino-only' in sys.argv:
            from island_cupertino_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell,bluetooth,battery)
            raise SystemExit(0)
        if '--cupertino-only' in sys.argv:
            from cupertino_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--starter-only' in sys.argv:
            from starter_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--island-launcher-only' in sys.argv:
            from island_launcher_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--lockscreen-only' in sys.argv:
            from lockscreen_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--dock-preview-perf-only' in sys.argv:
            from dock_preview_perf_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--dock-preview-only' in sys.argv:
            from dock_preview_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--dock-motion-only' in sys.argv:
            from dock_motion_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--text-fit-only' in sys.argv:
            from text_fit_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--settings-layout-only' in sys.argv:
            from settings_layout_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--default-apps-only' in sys.argv:
            from default_apps_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--backups-only' in sys.argv:
            from backups_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--performance-only' in sys.argv:
            from performance_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--displays-only' in sys.argv:
            from hyprland_displays_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--startup-only' in sys.argv:
            from startup_apps_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--keybinds-only' in sys.argv:
            from hyprland_keybinds_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--workspaces-only' in sys.argv:
            from hyprland_workspaces_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--gestures-only' in sys.argv:
            from hyprland_gestures_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--native-input-only' in sys.argv:
            from hyprland_native_input_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--island-activity-cycle-only' in sys.argv:
            from island_activity_cycle_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--island-hover-timing-only' in sys.argv:
            from island_hover_timing_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        if '--island-routing-only' in sys.argv:
            from island_routing_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell,bluetooth)
            raise SystemExit(0)
        if '--island-bars-only' in sys.argv:
            from island_bar_smoke import run_checks
            run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
            raise SystemExit(0)
        # Appearance overrides must survive compositor reloads and shell restarts,
        # and disabling them must restore the user's original Lua configuration.
        original_config=(cfg/'config.toml').read_text()
        def option(key):
            # Hyprland 0.56's Lua string options omit JSON escaping. Use its
            # plain output for the exclusions test, which deliberately contains quotes.
            if key=='plugin:kinetic-scroll:disabled_classes':
                return {'str':ctl('getoption',key).removeprefix('str: ').rsplit('\nset: ',1)[0]}
            return json.loads(ctl('-j','getoption',key))
        initial_rounding=option('decoration:rounding')['int']
        initial_opacity=option('decoration:active_opacity')['float']
        initial_border=option('general:col.active_border')
        effect_keys=('shadow:color','shadow:color_inactive','shadow:offset','shadow:scale','shadow:render_power','shadow:sharp','glow:enabled','glow:range','glow:render_power','glow:color','glow:color_inactive')
        initial_effects={key:option('decoration:'+key) for key in effect_keys}
        focus_values={'rounding_power':3.5,'fullscreen_opacity':.97,'dim_inactive':True,'dim_strength':.12,'dim_special':.3,
                      'blur:brightness':.92,'blur:contrast':.85,'blur:vibrancy':.25,'blur:noise':.02,
                      'blur:popups':True,'blur:special':True,'blur:popups_ignorealpha':.3}
        initial_focus={key:option('decoration:'+key) for key in focus_values}
        def focus_matches():
            return all((option('decoration:'+key)['bool']==value if isinstance(value,bool)
                        else abs(option('decoration:'+key)['float']-value)<.001) for key,value in focus_values.items())
        def animations():
            return {a['name']:a for a in json.loads(ctl('-j','animations'))[0]}
        initial_animations=animations()
        appearance='[shell.hyprland_appearance]\nenabled=true\nfollow_theme=true\ngaps_in=9\ngaps_out=17\nborder_size=3\nrounding=23\nactive_opacity=0.93\ninactive_opacity=0.85\nblur_enabled=true\nblur_size=5\nblur_passes=3\nshadow_enabled=true\nshadow_range=18\nanimations_enabled=false\n'
        appearance += 'custom_animations=true\nanimation_speed=1.5\nanimation_easing="snappy"\nwindow_animation="fade"\nworkspace_animation="vertical"\n'
        appearance += 'blur_focus_managed=true\n'+''.join(key.replace(':','_')+'='+str(value).lower()+'\n' for key,value in focus_values.items())
        (cfg/'config.toml').write_text(original_config+'\n'+appearance)
        msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==23,'appearance application')
        wait(focus_matches,'blur and focus application')
        assert option('general:border_size')['int']==3
        assert abs(option('decoration:active_opacity')['float']-.93)<.001
        assert option('animations:enabled')['bool'] is False
        assert option('general:col.active_border')!=initial_border
        assert animations()['windowsIn']['style']=='popin 100%'
        assert abs(animations()['windowsIn']['speed']-4/1.5)<.01
        assert animations()['workspacesIn']['style']=='slidevert'
        ctl('reload')
        time.sleep(.2)
        wait(lambda:option('decoration:rounding')['int']==23,'appearance after Hyprland reload')
        wait(focus_matches,'blur and focus after Hyprland reload')
        shell.terminate();shell.wait(timeout=5)
        ctl('reload');time.sleep(.2)
        assert option('decoration:rounding')['int']==initial_rounding
        shell=start([binary],'noctalia-appearance-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'Noctalia restart')
        wait(lambda:option('decoration:rounding')['int']==23,'appearance after shell restart')
        wait(focus_matches,'blur and focus after shell restart')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance.replace('blur_focus_managed=true','blur_focus_managed=false'));msg('config-reload')
        wait(lambda:all(option('decoration:'+key)==value for key,value in initial_focus.items()),'restore blur and focus config')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance);msg('config-reload')
        wait(focus_matches,'reapply blur and focus')
        assert animations()['workspacesIn']['style']=='slidevert'
        # Exercise each style/curve, including disabled leaves and multiplier direction.
        for easing,window,workspace,speed,expected_window,expected_workspace in [
            ('smooth','pop','slide',.25,'popin 90%','slide'),
            ('snappy','slide','vertical',1,'slide','slidevert'),
            ('gentle','fade','fade',2,'popin 100%','fade'),
            ('linear','instant','instant',3,'',''),
        ]:
            changed=appearance.replace('animation_easing="snappy"',f'animation_easing="{easing}"').replace('window_animation="fade"',f'window_animation="{window}"').replace('workspace_animation="vertical"',f'workspace_animation="{workspace}"').replace('animation_speed=1.5',f'animation_speed={speed}')
            (cfg/'config.toml').write_text(original_config+'\n'+changed);msg('config-reload')
            wait(lambda:animations()['windowsIn']['enabled']==(window!='instant') and animations()['windowsIn']['style']==expected_window,'window animation style')
            a=animations()
            assert a['workspacesIn']['style']==expected_workspace and a['workspacesIn']['enabled']==(workspace!='instant')
            if window!='instant':assert abs(a['windowsIn']['speed']-4/speed)<.01
            assert not ctl('configerrors').strip(),ctl('configerrors')
        # Independent timing/curves, overshoot, springs, and unloaded plugin guards.
        advanced=appearance+'opening_duration=800\nclosing_duration=150\nmoving_duration=600\nworkspace_duration=900\nglass_managed=true\ncursor_managed=true\noverview_managed=true\n'
        advanced+='decoration_effects_managed=true\nshadow_color="#446688"\nshadow_inactive_color="#112233"\nshadow_opacity=0.5\nshadow_inactive_opacity=0.25\nshadow_offset_x=-12\nshadow_offset_y=8\nshadow_scale=0.9\nshadow_power=4\nshadow_sharp=true\nglow_enabled=true\nglow_range=7\nglow_power=2\nglow_color="#123456"\nglow_opacity=0.5\nglow_inactive_opacity=0.0\n'
        advanced+='[shell.hyprland_appearance.opening_curve]\neasing="spring"\nstiffness=300\ndamping=18\n[shell.hyprland_appearance.closing_curve]\neasing="custom"\nx1=0.2\ny1=1.4\nx2=0.4\ny2=1.0\n'
        (cfg/'config.toml').write_text(original_config+'\n'+advanced);msg('config-reload')
        wait(lambda:animations()['windowsIn']['bezier']=='spring:noctalia_opening','independent spring curve')
        assert abs(animations()['windowsIn']['speed']-8/1.5)<.01
        assert abs(animations()['windowsOut']['speed']-1/1.0)<.01
        assert abs(animations()['windowsMove']['speed']-6/1.5)<.01
        assert abs(animations()['workspacesIn']['speed']-9/1.5)<.01
        assert animations()['windowsOut']['bezier']=='noctalia_closing'
        assert option('decoration:shadow:offset')['vec2']==[-12,8]
        assert option('decoration:shadow:color')['gradient'].startswith('80446688')
        assert option('decoration:shadow:color_inactive')['gradient'].startswith('40112233')
        assert option('decoration:glow:enabled')['bool'] is True
        assert option('decoration:glow:color')['gradient'].startswith('80123456')
        themed=advanced.replace('glow_color="#123456"','glow_color="primary"')
        (cfg/'config.toml').write_text(original_config+'\n'+themed);msg('config-reload')
        wait(lambda:option('decoration:glow:color')['gradient'][2:8]==option('general:col.active_border')['gradient'][2:8],'theme glow colour')
        unmanaged=themed.replace('decoration_effects_managed=true','decoration_effects_managed=false')
        (cfg/'config.toml').write_text(original_config+'\n'+unmanaged);msg('config-reload')
        wait(lambda:all(option('decoration:'+key)==value for key,value in initial_effects.items()),'restore shadow and glow config')
        (cfg/'config.toml').write_text(original_config+'\n'+advanced);msg('config-reload')
        assert not ctl('configerrors').strip(),ctl('configerrors')
        # Optionally validate the installed plugins in this isolated compositor.
        plugin_dir=os.environ.get('NOCTALIA_TEST_HYPR_PLUGINS')
        if plugin_dir:
            config.write_text(config.read_text()+''.join('\nhl.plugin.load('+json.dumps(str(pathlib.Path(plugin_dir)/name))+')\n' for name in ('hyprglass.so','dynamic-cursors.so','Hyprspace.so','hypr-kinetic-scroll.so','libhypr-edgehover.so')))
            ctl('reload')
            time.sleep(2)
            assert 'hyprglass' in ctl('plugin','list'),ctl('plugin','list')
            wait(lambda:abs(option('plugin:hyprglass:blur_strength')['float']-1.5)<.001,'Hyprglass apply after load')
            assert option('plugin:overview:panelHeight')['int']==220
            overview_keys=('panelColor','panelBorderColor','workspaceActiveBackground','workspaceInactiveBackground',
                           'workspaceActiveBorder','workspaceInactiveBorder','workspaceMargin','panelBorderWidth',
                           'workspaceBorderSize','centerAligned','disableBlur','dragAlpha','overrideAnimSpeed')
            def overview_values():
                return {key:{k:v for k,v in option('plugin:overview:'+key).items() if k not in ('set','option')}
                        for key in overview_keys}
            original_overview=overview_values()
            overview_settings='overview_style_managed=true\noverview_margin=18\noverview_panel_border=3\noverview_workspace_border=4\noverview_centered=false\noverview_blur=false\noverview_drag_opacity=0.7\noverview_duration=600\noverview_panel_color="#112233"\noverview_panel_opacity=0.5\n'
            styled=advanced.replace('overview_managed=true\n','overview_managed=true\n'+overview_settings)
            (cfg/'config.toml').write_text(original_config+'\n'+styled);msg('config-reload')
            wait(lambda:option('plugin:overview:panelColor')['int']==0x80112233,'overview fixed colour and opacity')
            assert option('plugin:overview:workspaceMargin')['int']==18
            assert option('plugin:overview:panelBorderWidth')['int']==3
            assert option('plugin:overview:workspaceBorderSize')['int']==4
            assert option('plugin:overview:centerAligned')['int']==0
            assert option('plugin:overview:disableBlur')['int']==1
            assert abs(option('plugin:overview:dragAlpha')['float']-.7)<.001
            assert option('plugin:overview:overrideAnimSpeed')['float']==6
            active_colour=int(option('general:col.active_border')['gradient'][:8],16)&0xffffff
            assert option('plugin:overview:workspaceActiveBorder')['int']==(0xd9000000|active_colour)
            ctl('reload');time.sleep(.3)
            wait(lambda:option('plugin:overview:panelColor')['int']==0x80112233,'overview style after compositor reload')
            shell.terminate();shell.wait(timeout=5)
            shell=start([binary],'noctalia-overview-restart.log')
            wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'overview shell restart')
            wait(lambda:option('plugin:overview:panelColor')['int']==0x80112233,'overview style after shell restart')
            (cfg/'config.toml').write_text(original_config+'\n'+styled.replace('overview_style_managed=true','overview_style_managed=false'));msg('config-reload')
            wait(lambda:overview_values()==original_overview,'restore overview style without relinquishing panel layout')
            assert option('plugin:overview:panelHeight')['int']==220
            # Measure the rendered panel, not just the accepted speed option.
            from PIL import Image
            def overview_frame(duration,label):
                render_style=styled.replace('animations_enabled=false','animations_enabled=true').replace('overview_duration=600',f'overview_duration={duration}').replace('overview_panel_color="#112233"','overview_panel_color="#ff00ff"').replace('overview_panel_opacity=0.5','overview_panel_opacity=1')
                (cfg/'config.toml').write_text(original_config+'\n'+render_style);msg('config-reload');time.sleep(.4)
                before=animations()['windows']['speed']
                assert ctl('eval','hl.plugin.overview.open()').strip()=='ok';time.sleep(.2)
                frame=out/f'overview-motion-{label}.png'
                run(['grim','-o','TEST-1',str(frame)])
                assert animations()['windows']['speed']==before,'Overview changed normal window animation speed'
                assert ctl('eval','hl.plugin.overview.close()').strip()=='ok';time.sleep(duration/1000+.3)
                pixels=Image.open(frame).convert('RGB')
                return sum(r>230 and g<30 and b>230 for r,g,b in (pixels.getpixel((5,y)) for y in range(720)))
            slow_rows=overview_frame(2000,'slow')
            fast_rows=overview_frame(100,'fast')
            assert fast_rows>slow_rows+20,(slow_rows,fast_rows)
            print(f'PASS: overview animation speed changes rendered motion ({slow_rows} vs {fast_rows} panel rows)',flush=True)
            changed=advanced.replace('glass_managed=true','glass_managed=true\nglass_blur=3.2\nglass_refraction=0.7\nglass_layers=false\ncursor_stretch=true\noverview_height=280')
            (cfg/'config.toml').write_text(original_config+'\n'+changed);msg('config-reload')
            wait(lambda:abs(option('plugin:hyprglass:blur_strength')['float']-3.2)<.001,'Hyprglass live adjustment')
            assert option('plugin:dynamic_cursors:mode')['str']=='stretch'
            assert option('plugin:overview:panelHeight')['int']==280
            assert not ctl('configerrors').strip(),ctl('configerrors')
            # Relinquishing management reloads plugin defaults while retaining geometry.
            changed=changed.replace('glass_managed=true','glass_managed=false').replace('cursor_managed=true','cursor_managed=false').replace('overview_managed=true','overview_managed=false')
            (cfg/'config.toml').write_text(original_config+'\n'+changed);msg('config-reload')
            wait(lambda:abs(option('plugin:hyprglass:blur_strength')['float']-2.0)<.001,'restore Hyprglass configuration')
            assert option('decoration:rounding')['int']==23
        appearance=appearance.replace('animations_enabled=false','animations_enabled=true')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance);msg('config-reload')
        msg('settings-open','appearance');time.sleep(1)
        run(['grim','-o','TEST-1',str(out/'appearance-settings.png')])
        # Use a private virtual pointer to open the animations group.
        proto=REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
        run(['wayland-scanner','client-header',str(proto),str(base/'pointer-client.h')])
        run(['wayland-scanner','private-code',str(proto),str(base/'pointer-code.c')])
        run(['cc','-I'+str(base),str(REPO/'tests/fixtures/island_pointer.c'),str(base/'pointer-code.c'),'-lwayland-client','-o',str(base/'pointer')])
        pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);processes.append(pointer)
        dispatch('hl.dsp.cursor.move({x=510,y=283})')
        for action in ('press','release'):
            pointer.stdin.write(action+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok';time.sleep(.1)
        time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'animation-settings.png')])
        content_offset=42  # Three rows of group pills at the integration baseline gaps.
        def point(x,y):
            if y>=360:y+=content_offset
            dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
            time.sleep(.1)  # Let the shell receive pointer motion before button input.
        def mouse(action):
            pointer.stdin.write(action+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok';time.sleep(.1)
        point(766,283);mouse('press');mouse('release');time.sleep(.3)
        point(1000,590);mouse('scroll 5');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'curve-editor.png')])
        # Save and reload a named appearance preset through the real controls.
        keyboard_proto=REPO/'protocols/virtual-keyboard-unstable-v1.xml'
        run(['wayland-scanner','client-header',str(keyboard_proto),str(base/'keyboard-client.h')])
        run(['wayland-scanner','private-code',str(keyboard_proto),str(base/'keyboard-code.c')])
        run(['cc','-I'+str(base),str(REPO/'tests/fixtures/island_keyboard.c'),str(base/'keyboard-code.c'),'-lwayland-client','-lxkbcommon','-o',str(base/'keyboard')])
        keyboard=subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);processes.append(keyboard)
        time.sleep(.3)
        point(627,283);mouse('press');mouse('release');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'presets-before-save.png')])
        point(740,527);mouse('press');mouse('release')
        for key in (20,18,31,20):
            keyboard.stdin.write(str(key)+'\n');keyboard.stdin.flush();assert keyboard.stdout.readline().strip()=='ok'
        point(740,575);mouse('press');mouse('release');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'presets-after-save.png')])
        settings_state=base/'state/noctalia/settings.toml'
        saved=tomllib.loads(settings_state.read_text()).get('shell',{}).get('hyprland_appearance_profiles',{})
        assert 'test' in saved,'Named preset was not saved'
        assert tomllib.loads(saved['test'])['rounding']==23
        assert tomllib.loads(saved['test'])['opening_duration']==400
        assert tomllib.loads(saved['test'])['blur_focus_managed'] is True
        assert abs(tomllib.loads(saved['test'])['blur_brightness']-.92)<.001
        (cfg/'config.toml').write_text(original_config+'\n'+appearance.replace('rounding=23','rounding=31'));msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==31,'new appearance before preset restore')
        time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'presets-before-restore.png')])
        point(707,610);mouse('press');mouse('release')
        run(['grim','-o','TEST-1',str(out/'presets-after-restore.png')])
        wait(lambda:option('decoration:rounding')['int']==23,'restore named preset')
        # Reset the resulting override so subsequent checks can change the base config.
        point(820,416);mouse('press');mouse('release')
        wait(lambda:option('decoration:rounding')['int']==31,'reset preset overrides')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance);msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==23,'restore integration baseline')
        effects_preview=appearance+'glow_enabled=true\nglow_color="primary"\nshadow_opacity=0.55\nglow_opacity=0.16\n'
        (cfg/'config.toml').write_text(original_config+'\n'+effects_preview);msg('config-reload')
        point(705,325);mouse('press');mouse('release');time.sleep(.5)
        point(1180,436);mouse('press');mouse('release')
        wait(lambda:option('decoration:glow:enabled')['bool'],'enable shadow and glow controls in UI')
        time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'shadow-glow-settings.png')])
        point(1180,436);mouse('press');mouse('release')
        wait(lambda:not option('decoration:glow:enabled')['bool'],'disable shadow and glow controls in UI')
        point(627,283);mouse('press');mouse('release');time.sleep(.5)
        point(627,283);mouse('press');mouse('release');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'soft-glass-before.png')])
        point(875,469);mouse('press');mouse('release')
        wait(lambda:option('decoration:rounding')['int']==16,'Soft Glass preset')
        assert abs(option('decoration:dim_strength')['float']-.08)<.001
        assert option('decoration:rounding_power')['float']==3
        assert option('decoration:fullscreen_opacity')['float']==1
        assert option('decoration:blur:popups')['bool'] is True
        assert option('decoration:blur:special')['bool'] is False
        assert option('decoration:shadow:range')['int']==18
        assert option('decoration:glow:enabled')['bool'] is True
        # App rules keeps the group pills on three rows even with Soft Glass gaps.
        content_offset=42
        point(675,319);mouse('press');mouse('release');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'blur-focus-settings.png')])
        point(627,283);mouse('press');mouse('release');time.sleep(.3)
        point(627,283);mouse('press');mouse('release');time.sleep(.3)
        run(['grim','-o','TEST-1',str(out/'soft-glass-before-undo.png')])
        point(625,416);mouse('press');mouse('release')
        wait(lambda:option('decoration:rounding')['int']==23,'undo Soft Glass')
        wait(focus_matches,'undo blur and focus preset values')
        content_offset=42
        point(820,416);mouse('press');mouse('release')
        wait(focus_matches,'reset appearance overrides after Soft Glass')
        if os.environ.get('NOCTALIA_TEST_EDITOR_INSPECT'):
            (out/'inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Editor inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue').exists():break
                time.sleep(.1)
        msg('settings-close')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance.replace('custom_animations=true','custom_animations=false'))
        msg('config-reload')
        wait(lambda:animations()==initial_animations,'restore configured animation styles')
        assert option('decoration:rounding')['int']==23
        (cfg/'config.toml').write_text(original_config+'\n'+appearance.replace('follow_theme=true','follow_theme=false'))
        msg('config-reload')
        wait(lambda:option('general:col.active_border')==initial_border,'restore border colours')
        assert option('decoration:rounding')['int']==23
        (cfg/'config.toml').write_text(original_config)
        msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==initial_rounding,'appearance disabled restoration')
        wait(lambda:all(option('decoration:'+key)==value for key,value in initial_focus.items()),'full blur and focus restoration')
        assert abs(option('decoration:active_opacity')['float']-initial_opacity)<.001
        assert animations()==initial_animations
        assert not ctl('configerrors').strip(),ctl('configerrors')
        assert shell.poll() is None
        # Literal app matching, real window properties, scoped rules and removal.
        app_class='hypr-rule.app+[one]'
        for name in (app_class,'hypr-ruleXappone'):
            start(['kitty','--config','NONE','--class',name,'-e','sleep','180'],'app-rule-'+str(len(processes))+'.log')
        def app_window(name):
            return next((w for w in json.loads(ctl('-j','clients')) if w['class']==name),None)
        wait(lambda:app_window(app_class) and app_window('hypr-ruleXappone'),'app rule test windows')
        address=app_window(app_class)['address']
        neighbor=app_window('hypr-ruleXappone')['address']
        def prop(name,window=address):
            return json.loads(ctl('-j','getprop','address:'+window,name))[name]
        def has_glass_tag():
            return any(t.rstrip('*')=='hyprglass_disabled' for t in app_window(app_class)['tags'])
        # Existing Lua rules must reappear after Noctalia relinquishes an effect.
        config.write_text(config.read_text()+'\nhl.window_rule({name="existing-app-rule",match={class="^hypr-rule\\\\.app\\\\+\\\\[one\\\\]$"},opacity="0.77 override",rounding=9})\n')
        ctl('reload');time.sleep(.3)
        baseline={k:prop(k) for k in ('opacity','opacity_inactive','opacity_fullscreen','rounding','no_shadow','no_anim')}
        assert abs(baseline['opacity']-.77)<.001
        rule='\n[shell.hyprland_app_rules.test]\napp_class='+json.dumps(app_class)+'\nenabled=true\nopacity_managed=true\nactive_opacity=0.94\ninactive_opacity=0.88\nfullscreen_opacity=1.0\nrounding_managed=true\nrounding=7\nblur="off"\nshadow="off"\nglass="off"\n'
        def apply_rules(text):
            (cfg/'config.toml').write_text(original_config+'\n'+appearance+text);msg('config-reload')
        apply_rules(rule)
        wait(lambda:abs(prop('opacity')-.94)<.001 and prop('rounding')==7,'per-app live appearance')
        assert abs(prop('opacity_inactive')-.88)<.001 and prop('opacity_fullscreen')==1
        assert prop('opacity_override') is True and prop('no_shadow') is True
        assert prop('opacity',neighbor)==1,'Literal class matching affected a different app'
        if plugin_dir:
            wait(has_glass_tag,'per-app Hyprglass tag')
        overlap=rule+'\n[shell.hyprland_app_rules.overlap]\napp_class='+json.dumps(app_class)+'\nglass="off"\n'
        apply_rules(overlap)
        wait(has_glass_tag,'overlapping Hyprglass rules')
        ctl('reload');time.sleep(.2)
        wait(lambda:abs(prop('opacity')-.94)<.001,'app rules after compositor reload')
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-app-rule-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'app rules shell restart')
        wait(lambda:abs(prop('opacity')-.94)<.001,'app rules after shell restart')
        scoped=rule.replace('rounding_managed=true','scope="fullscreen"\nrounding_managed=false')
        apply_rules(scoped)
        wait(lambda:prop('rounding')==9 and abs(prop('opacity')-.77)<.001,'removed effects and fullscreen scope')
        wait(lambda:not has_glass_tag(),'fullscreen-only glass before entering fullscreen')
        dispatch('hl.dsp.window.fullscreen({window="address:'+address+'",action="set"})')
        wait(lambda:abs(prop('opacity')-.94)<.001,'fullscreen rule activation')
        wait(has_glass_tag,'fullscreen-only glass activation')
        dispatch('hl.dsp.window.fullscreen({window="address:'+address+'",action="unset"})')
        wait(lambda:abs(prop('opacity')-.77)<.001,'fullscreen rule deactivation')
        wait(lambda:not has_glass_tag(),'fullscreen-only glass deactivation')
        apply_rules(scoped.replace('scope="fullscreen"','scope="floating"'))
        dispatch('hl.dsp.window.float({window="address:'+address+'",action="set"})')
        wait(lambda:abs(prop('opacity')-.94)<.001,'floating rule activation')
        dispatch('hl.dsp.window.float({window="address:'+address+'",action="unset"})')
        wait(lambda:abs(prop('opacity')-.77)<.001,'floating rule deactivation')
        apply_rules(rule.replace('enabled=true','enabled=false'))
        wait(lambda:all(prop(k)==v for k,v in baseline.items()),'disable app rule')
        apply_rules(rule)
        wait(lambda:abs(prop('opacity')-.94)<.001,'reenable app rule')
        apply_rules('')
        wait(lambda:all(prop(k)==v for k,v in baseline.items()),'remove app rule')
        wait(lambda:not has_glass_tag(),'remove Hyprglass app tag')
        dispatch('hl.dsp.focus({workspace="3"})')
        msg('settings-open','appearance');time.sleep(1)
        # The first click after this shell restart can be consumed while the
        # settings surface gains focus. Confirm navigation actually moved the body.
        from PIL import Image
        app_editor_frame=out/'app-rule-editor.png'
        run(['grim','-o','TEST-1',str(app_editor_frame)])
        before_navigation=Image.open(app_editor_frame).crop((280,425,1210,650)).tobytes()
        for _ in range(3):
            point(918,325);mouse('press');mouse('release');time.sleep(.5)
            run(['grim','-o','TEST-1',str(app_editor_frame)])
            if Image.open(app_editor_frame).crop((280,425,1210,650)).tobytes()!=before_navigation:break
        else:raise AssertionError('App rules tab did not open')
        point(745,501);mouse('press');mouse('release');time.sleep(.3)
        run(['grim','-o','TEST-1',str(out/'app-picker.png')])
        for key in (52,30,25,25):  # '.app' uniquely identifies the literal-match fixture.
            keyboard.stdin.write(str(key)+'\n');keyboard.stdin.flush();assert keyboard.stdout.readline().strip()=='ok'
        time.sleep(.3)
        point(633,334);mouse('press');mouse('release');time.sleep(.5)
        ui_rules=tomllib.loads(settings_state.read_text())['shell']['hyprland_app_rules']
        assert ui_rules['app-0001']['app_class']==app_class,'Running-app picker used the wrong class'
        point(1000,568)
        for _ in range(6):mouse('scroll 1')
        time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'app-rule-controls.png')])
        if os.environ.get('NOCTALIA_TEST_APP_RULE_INSPECT'):
            (out/'app-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('App rule inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue').exists():break
                time.sleep(.1)
        msg('settings-close')
        (cfg/'config.toml').write_text(original_config);msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==initial_rounding,'restore after app rule tests')
        switching='\n[shell.hyprland_profile_switching]\nenabled=true\n'
        def apply_profile_mode(mode,selection='',profiles=''):
            (cfg/'config.toml').write_text(original_config+'\n'+appearance+'overview_managed=true\noverview_style_managed=true\n'+rule+switching+selection+profiles
                +'\n[theme]\nmode="dark"\nshell_mode='+json.dumps(mode)+'\n')
            msg('config-reload')
        def profile_brightness():return option('decoration:blur:brightness')['float']
        apply_profile_mode('dark')
        wait(lambda:abs(profile_brightness()-.95)<.001,'dark appearance profile')
        assert abs(prop('opacity')-.94)<.001 and has_glass_tag(),'Profile changed per-app rules'
        apply_profile_mode('light')
        wait(lambda:abs(profile_brightness()-1.04)<.001,'shell light mode appearance profile')
        if plugin_dir:assert option('plugin:hyprglass:default_theme')['str']=='light'
        if plugin_dir:
            wait(lambda:option('plugin:overview:workspaceActiveBorder')['int']&0xffffff == int(option('general:col.active_border')['gradient'][:8],16)&0xffffff,'overview light palette')
            light_overview_panel=option('plugin:overview:panelColor')['int']
        assert abs(prop('opacity')-.94)<.001 and has_glass_tag(),'Light profile changed per-app rules'
        ctl('reload');time.sleep(.2)
        wait(lambda:abs(profile_brightness()-1.04)<.001,'light profile after compositor reload')
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-theme-profile-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'theme profile shell restart')
        wait(lambda:abs(profile_brightness()-1.04)<.001,'light profile after shell restart')
        apply_profile_mode('dark')
        wait(lambda:abs(profile_brightness()-.95)<.001,'return to dark appearance profile')
        if plugin_dir:assert option('plugin:hyprglass:default_theme')['str']=='dark'
        if plugin_dir:
            wait(lambda:option('plugin:overview:panelColor')['int']!=light_overview_panel,'overview follows dark palette')
        custom_profile='rounding=19\nblur_focus_managed=true\nblur_brightness=1.17\nglass_managed=true\nglass_light=true\n'
        saved_profile='\n[shell.hyprland_appearance_profiles]\n"Custom light"='+json.dumps(custom_profile)+'\n'
        apply_profile_mode('light','light_profile="Custom light"\n',saved_profile)
        wait(lambda:abs(profile_brightness()-1.17)<.001 and option('decoration:rounding')['int']==19,'assigned saved light profile')
        apply_profile_mode('light','light_profile="Missing look"\n')
        wait(lambda:abs(profile_brightness()-.92)<.001,'missing profile fallback')
        apply_profile_mode('light','light_profile="Custom light"\n',saved_profile.replace(json.dumps(custom_profile),json.dumps('invalid toml')))
        wait(lambda:abs(profile_brightness()-.92)<.001,'invalid profile fallback')
        apply_profile_mode('light')
        wait(lambda:abs(profile_brightness()-1.04)<.001,'restore built-in light profile')
        msg('settings-open','appearance');time.sleep(.6)
        dispatch('hl.dsp.cursor.move({x=688,y=283})');mouse('press');mouse('release');time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'theme-profiles.png')])
        dispatch('hl.dsp.cursor.move({x=745,y=501})');mouse('press');mouse('release')
        wait(lambda:tomllib.loads(settings_state.read_text())['shell']['hyprland_profile_switching']['enabled'] is False,'Keep current look pauses automatic switching')
        apply_profile_mode('dark');time.sleep(1)
        assert abs(profile_brightness()-1.04)<.001,'Manual look changed with theme'
        assert abs(prop('opacity')-.94)<.001,'Manual look changed app rules'
        ctl('reload');time.sleep(.3)
        wait(lambda:abs(profile_brightness()-1.04)<.001,'manual look after compositor reload')
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-manual-profile-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'manual profile shell restart')
        wait(lambda:abs(profile_brightness()-1.04)<.001,'manual look after shell restart')
        msg('settings-open','appearance');time.sleep(.5)
        dispatch('hl.dsp.cursor.move({x=1014,y=325})');mouse('press');mouse('release');time.sleep(.4)
        run(['grim','-o','TEST-1',str(out/'theme-manual-profile.png')])
        dispatch('hl.dsp.cursor.move({x=745,y=554})');mouse('press');mouse('release')
        wait(lambda:abs(profile_brightness()-.95)<.001,'resume automatic appearance switching')
        apply_profile_mode('light')
        wait(lambda:abs(profile_brightness()-1.04)<.001,'light switch after resume')
        if os.environ.get('NOCTALIA_TEST_THEME_PROFILE_INSPECT'):
            (out/'theme-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Theme profile inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue').exists():break
                time.sleep(.1)
        msg('settings-close')
        (cfg/'config.toml').write_text(original_config);msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==initial_rounding,'restore after theme profile tests')
        (cfg/'config.toml').write_text(original_config+'\n'+appearance);msg('config-reload')
        msg('settings-open','appearance');time.sleep(1)
        dispatch('hl.dsp.cursor.move({x=150,y=175})');time.sleep(.1);mouse('press');mouse('release')
        for key in (31,24,33,20,57,34,38,30,31,31,57,24,47,18,19,47,23,18,17):  # soft glass overview
            keyboard.stdin.write(str(key)+'\n');keyboard.stdin.flush();assert keyboard.stdout.readline().strip()=='ok'
        time.sleep(.5)
        run(['grim','-o','TEST-1',str(out/'overview-preset-search.png')])
        before_preset=tomllib.loads(settings_state.read_text())
        dispatch('hl.dsp.cursor.move({x=750,y=333})');time.sleep(.1);mouse('press');mouse('release')
        wait(lambda:tomllib.loads(settings_state.read_text())['shell']['hyprland_appearance'].get('overview_style_managed',False),'Soft Glass overview preset button')
        after_preset=tomllib.loads(settings_state.read_text())
        for values in (before_preset,after_preset):
            values['shell']['hyprland_appearance']={k:v for k,v in values['shell'].get('hyprland_appearance',{}).items() if not k.startswith('overview_')}
        assert before_preset==after_preset,'Overview preset changed other appearance settings'
        if plugin_dir:
            assert option('plugin:overview:overrideAnimSpeed')['float']==3.5
            assert option('plugin:overview:workspaceMargin')['int']==12
        dispatch('hl.dsp.cursor.move({x=336,y=175})');time.sleep(.1);mouse('press');mouse('release')
        dispatch('hl.dsp.cursor.move({x=150,y=175})');time.sleep(.1);mouse('press');mouse('release')
        for key in (24,47,18,19,47,23,18,17):  # overview
            keyboard.stdin.write(str(key)+'\n');keyboard.stdin.flush();assert keyboard.stdout.readline().strip()=='ok'
        time.sleep(.4)
        dispatch('hl.dsp.cursor.move({x=1000,y=590})');time.sleep(.1)
        for _ in range(5):mouse('scroll 1')
        time.sleep(.4)
        run(['grim','-o','TEST-1',str(out/'overview-style-controls.png')])
        if os.environ.get('NOCTALIA_TEST_OVERVIEW_INSPECT'):
            (out/'overview-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Overview inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue').exists():break
                time.sleep(.1)
        msg('settings-close')
        (cfg/'config.toml').write_text(original_config);msg('config-reload')
        wait(lambda:option('decoration:rounding')['int']==initial_rounding,'restore after overview editor')
        if plugin_dir:
            input_options=('dynamic_cursors:mode','dynamic_cursors:tilt:full','dynamic_cursors:rotate:length',
                'dynamic_cursors:shake:enabled','dynamic_cursors:shake:threshold','dynamic_cursors:shake:limit',
                'kinetic-scroll:enabled','kinetic-scroll:decel','kinetic-scroll:delta_multiplier',
                'kinetic-scroll:disabled_classes','kinetic-scroll:stop_on_click','kinetic-scroll:stop_on_focus',
                'hypr_edgehover:edges','hypr_edgehover:max_distance','hypr_edgehover:keyboard_focus','hypr_edgehover:gap_pass')
            def input_values():
                return {key:{k:v for k,v in option('plugin:'+key).items() if k not in ('set','option')} for key in input_options}
            baseline_input=input_values()
            panel_forwarding=option('plugin:hypr_edgehover:layer_pass')['str']
            excluded='steam, app"quoted\\name'
            input_config='\n[shell.hyprland_input]\ncursor_managed=true\ncursor_mode="rotate"\nrotate_length=40\ntilt_angle=30\nshake_threshold=7.0\nshake_limit=3.0\nscroll_managed=true\nscroll_decay=0.88\nscroll_multiplier=1.4\nscroll_stop_click=true\nscroll_stop_focus=true\nscroll_excluded='+json.dumps(excluded)+'\nedge_managed=true\nedge_top=false\nedge_bottom=false\nedge_distance=7\nedge_focus=0\nedge_click=false\n'
            def apply_input(extra=''):
                (cfg/'config.toml').write_text(original_config+input_config+extra);msg('config-reload')
            apply_input()
            wait(lambda:option('plugin:dynamic_cursors:mode')['str']=='rotate','independent input with appearance management off')
            assert option('decoration:rounding')['int']==initial_rounding
            assert option('plugin:dynamic_cursors:rotate:length')['int']==40
            assert option('plugin:dynamic_cursors:tilt:full')['int']==30
            assert option('plugin:dynamic_cursors:shake:threshold')['float']==7
            assert abs(option('plugin:kinetic-scroll:decel')['float']-.88)<.001
            assert option('plugin:kinetic-scroll:disabled_classes')['str']==excluded
            assert option('plugin:kinetic-scroll:stop_on_click')['int']==1
            assert option('plugin:hypr_edgehover:edges')['str']=='lr'
            assert option('plugin:hypr_edgehover:keyboard_focus')['int']==0
            assert option('plugin:hypr_edgehover:gap_pass')['str']=='hover,keyboard,scroll'
            assert option('plugin:hypr_edgehover:layer_pass')['str']==panel_forwarding
            chosen_input=input_values()
            for mode in ('light','dark'):
                apply_input('\n'+appearance+'cursor_managed=true\ncursor_stretch=true\n[shell.hyprland_profile_switching]\nenabled=true\n[theme]\nshell_mode="'+mode+'"\n')
                time.sleep(.5)
                assert input_values()==chosen_input,'Appearance profile overrode independent input'
            ctl('reload');time.sleep(.3)
            wait(lambda:input_values()==chosen_input,'input after compositor reload')
            shell.terminate();shell.wait(timeout=5)
            shell=start([binary],'noctalia-input-restart.log')
            wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'input shell restart')
            wait(lambda:input_values()==chosen_input,'input after shell restart')
            # Return to appearance-off input to inspect the new page and its presets.
            apply_input()
            msg('settings-open','input-motion');time.sleep(1)
            run(['grim','-o','TEST-1',str(out/'input-motion-settings.png')])
            for x,mode,enabled in ((820,'none',0),(710,'stretch',1),(635,'tilt',1)):
                dispatch(f'hl.dsp.cursor.move({{x={x},y=458}})');time.sleep(.1);mouse('press');mouse('release')
                wait(lambda:option('plugin:dynamic_cursors:mode')['str']==mode,'input preset '+mode)
                assert option('plugin:kinetic-scroll:enabled')['int']==enabled
                assert option('plugin:dynamic_cursors:shake:enabled')['bool']==bool(enabled)
                assert option('plugin:hypr_edgehover:edges')['str']=='lr'
                assert option('plugin:hypr_edgehover:keyboard_focus')['int']==0
                assert option('plugin:kinetic-scroll:stop_on_focus')['int']==1
                assert option('plugin:kinetic-scroll:disabled_classes')['str']==excluded
                assert option('decoration:rounding')['int']==initial_rounding
            for x,group in ((405,'cursor'),(530,'scroll'),(635,'edge')):
                dispatch(f'hl.dsp.cursor.move({{x={x},y=284}})');time.sleep(.1);mouse('press');mouse('release');time.sleep(.3)
                run(['grim','-o','TEST-1',str(out/f'input-{group}-settings.png')])
            if os.environ.get('NOCTALIA_TEST_INPUT_INSPECT'):
                (out/'input-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
                print('Input inspection ready',flush=True)
                for _ in range(1800):
                    if (out/'continue-input').exists():break
                    time.sleep(.1)
            msg('settings-close')
            (cfg/'config.toml').write_text(original_config);msg('config-reload')
            wait(lambda:input_values()==baseline_input,'restore input plugin defaults')
            print('PASS: independent input controls, theme isolation, Lua quoting, reload/restart persistence and restoration',flush=True)
        assert not ctl('configerrors').strip(),ctl('configerrors')
        behaviour_groups={
            'focus':('input:follow_mouse','input:follow_mouse_threshold','input:mouse_refocus'),
            'resize':('general:resize_on_border','general:extend_border_grab_area','general:hover_icon_on_border'),
            'snap':('general:snap:enabled','general:snap:window_gap','general:snap:monitor_gap','general:snap:border_overlap','general:snap:respect_gaps'),
            'activation':('misc:focus_on_activate',)}
        def behaviour_values():
            return {key:next(v for k,v in option(key).items() if k in ('int','float','bool'))
                    for keys in behaviour_groups.values() for key in keys}
        baseline_behaviour=behaviour_values()
        behaviour_config='''
[shell.hyprland_window_behaviour]
focus_managed=true
focus_mode=2
focus_threshold=8.5
mouse_refocus=false
resize_managed=true
resize_on_border=true
border_grab=23
border_cursor=false
snap_managed=true
snap_enabled=true
snap_window_distance=12
snap_monitor_distance=17
snap_border_overlap=true
snap_respect_gaps=true
activation_managed=true
focus_on_activate=true
'''
        chosen_behaviour=dict(zip((key for keys in behaviour_groups.values() for key in keys),
                                 (2,8.5,False,True,23,False,True,12,17,True,True,True)))
        def apply_behaviour(text=behaviour_config,extra=''):
            (cfg/'config.toml').write_text(original_config+text+extra);msg('config-reload')
        apply_behaviour()
        wait(lambda:behaviour_values()==chosen_behaviour,'window behaviour with appearance management off')
        assert option('decoration:rounding')['int']==initial_rounding
        for mode in ('light','dark'):
            apply_behaviour(extra='\n'+appearance+switching+'\n[theme]\nshell_mode="'+mode+'"\n')
            time.sleep(.5)
            assert behaviour_values()==chosen_behaviour,'Appearance profile changed window behaviour'
        ctl('reload');time.sleep(.3)
        wait(lambda:behaviour_values()==chosen_behaviour,'window behaviour after compositor reload')
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-behaviour-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'window behaviour shell restart')
        wait(lambda:behaviour_values()==chosen_behaviour,'window behaviour after shell restart')
        for group,keys in behaviour_groups.items():
            apply_behaviour(behaviour_config.replace(group+'_managed=true',group+'_managed=false'))
            expected=chosen_behaviour|{key:baseline_behaviour[key] for key in keys}
            wait(lambda:behaviour_values()==expected,'restore only '+group+' configuration')
        apply_behaviour()
        wait(lambda:behaviour_values()==chosen_behaviour,'restore managed window behaviour')
        msg('settings-open','window-behaviour');time.sleep(1)
        run(['grim','-o','TEST-1',str(out/'window-behaviour-settings.png')])
        if os.environ.get('NOCTALIA_TEST_BEHAVIOUR_INSPECT'):
            (out/'behaviour-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Window behaviour inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue-behaviour').exists():break
                time.sleep(.1)
        msg('settings-close')
        apply_behaviour('')
        wait(lambda:behaviour_values()==baseline_behaviour,'restore all window behaviour')
        assert not ctl('configerrors').strip(),ctl('configerrors')
        print('PASS: independent window behaviour, theme isolation, reload/restart persistence and individual group restoration',flush=True)
        tiling_groups={
            'layout':('general:layout',),
            'dwindle':('dwindle:preserve_split','dwindle:smart_split','dwindle:force_split','dwindle:use_active_for_splits','dwindle:default_split_ratio','dwindle:split_width_multiplier','dwindle:split_bias'),
            'master':('master:mfact','master:orientation','master:new_status','master:new_on_active','master:new_on_top'),
            'special':('misc:close_special_on_empty','binds:hide_special_on_workspace_change','input:special_fallthrough','cursor:warp_on_toggle_special')}
        def tiling_values():
            return {key:next(v for k,v in option(key).items() if k in ('int','float','bool','str'))
                    for keys in tiling_groups.values() for key in keys}
        baseline_tiling=tiling_values()
        tiling_config='''
[shell.hyprland_tiling]
layout_managed=true
layout="master"
dwindle_managed=true
preserve_split=true
smart_split=true
force_split=2
use_active_for_splits=false
default_split_ratio=1.25
split_width_multiplier=1.5
split_bias=1
master_managed=true
master_factor=0.65
master_orientation="right"
new_status="inherit"
new_on_active="after"
new_on_top=true
special_managed=true
close_special_on_empty=false
hide_special_on_workspace_change=true
special_fallthrough=true
warp_on_special=2
'''
        chosen_tiling=dict(zip((key for keys in tiling_groups.values() for key in keys),
                              ('master',True,True,2,False,1.25,1.5,1,.65,'right','inherit','after',True,False,True,True,2)))
        def apply_tiling(text=tiling_config,extra=''):
            (cfg/'config.toml').write_text(original_config+text+extra);msg('config-reload')
        apply_tiling()
        wait(lambda:tiling_values()==chosen_tiling,'tiling with appearance management off')
        assert option('decoration:rounding')['int']==initial_rounding
        assert behaviour_values()==baseline_behaviour,'Tiling changed window behaviour'
        def active_layout():return json.loads(ctl('-j','activeworkspace'))['tiledLayout']
        wait(lambda:active_layout()=='master','master layout on active workspace')
        for layout in ('dwindle','scrolling','monocle','master'):
            apply_tiling(tiling_config.replace('layout="master"','layout="'+layout+'"'))
            wait(lambda:active_layout()==layout,'activate '+layout+' layout')
        # Existing per-workspace layout rules override the managed global default.
        original_lua=config.read_text()
        workspace=json.loads(ctl('-j','activeworkspace'))['name']
        config.write_text(original_lua+'\nhl.workspace_rule({workspace='+json.dumps(workspace)+',layout="monocle"})\n')
        ctl('reload');time.sleep(.3)
        wait(lambda:active_layout()=='monocle','workspace layout rule takes precedence')
        assert option('general:layout')['str']=='master'
        config.write_text(original_lua);ctl('reload');time.sleep(.3)
        wait(lambda:active_layout()=='master','restore default after workspace rule removal')
        for mode in ('light','dark'):
            apply_tiling(extra='\n'+appearance+switching+'\n[theme]\nshell_mode="'+mode+'"\n')
            time.sleep(.5)
            assert tiling_values()==chosen_tiling,'Appearance profile changed tiling'
        ctl('reload');time.sleep(.3)
        wait(lambda:tiling_values()==chosen_tiling,'tiling after compositor reload')
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-tiling-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'tiling shell restart')
        wait(lambda:tiling_values()==chosen_tiling,'tiling after shell restart')
        for group,keys in tiling_groups.items():
            apply_tiling(tiling_config.replace(group+'_managed=true',group+'_managed=false'))
            expected=chosen_tiling|{key:baseline_tiling[key] for key in keys}
            wait(lambda:tiling_values()==expected,'restore only '+group+' tiling configuration')
        apply_tiling()
        wait(lambda:tiling_values()==chosen_tiling,'restore managed tiling')
        msg('settings-open','workspace-tiling');time.sleep(1)
        run(['grim','-o','TEST-1',str(out/'workspace-tiling-settings.png')])
        if os.environ.get('NOCTALIA_TEST_TILING_INSPECT'):
            (out/'tiling-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Workspace tiling inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue-tiling').exists():break
                time.sleep(.1)
        msg('settings-close')
        apply_tiling('')
        wait(lambda:tiling_values()==baseline_tiling,'restore all tiling configuration')
        assert not ctl('configerrors').strip(),ctl('configerrors')
        print('PASS: all four layouts, workspace rule precedence, independent tiling, theme isolation, reload/restart and group restoration',flush=True)
        dispatch('hl.dsp.focus({monitor="TEST-1"})')
        dispatch('hl.dsp.focus({workspace="3"})')
        placement_class='place.app+[one]'
        placement_serial=0
        def placement_client(address):
            return next((w for w in json.loads(ctl('-j','clients')) if w['address']==address),None)
        def open_placement(app=placement_class):
            global placement_serial
            placement_serial+=1
            before={w['address'] for w in json.loads(ctl('-j','clients'))}
            process=start(['kitty','--config','NONE','--override','remember_window_size=no','--class',app,'-e','sleep','300'],f'placement-{placement_serial}.log')
            def created():return next((w for w in json.loads(ctl('-j','clients')) if w['class']==app and w['address'] not in before),None)
            wait(created,'new placement test window')
            address=created()['address'];time.sleep(.3)
            return process,address
        def close_placement(pair):
            process,address=pair;process.terminate();process.wait(timeout=5)
            wait(lambda:placement_client(address) is None,'close placement window')
        old_placement=open_placement()
        old_workspace=placement_client(old_placement[1])['workspace']
        placement_config='''
[shell.hyprland_placement_rules.test]
app_class="place.app+[one]"
enabled=true
mode="floating"
workspace="number"
workspace_number=7
workspace_silent=true
size_managed=true
width=600
height=400
position="offset"
x=120
y=140
pin="off"
'''
        def apply_placement(text=placement_config,extra=''):
            (cfg/'config.toml').write_text(original_config+text+extra);msg('config-reload');time.sleep(.25)
        apply_placement(placement_config.replace('enabled=true','enabled=false'))
        disabled_window=open_placement()
        assert not placement_client(disabled_window[1])['floating']
        close_placement(disabled_window)
        apply_placement()
        assert not placement_client(old_placement[1])['floating'],'Placement rule changed an already open window'
        assert placement_client(old_placement[1])['workspace']==old_workspace
        placed=open_placement()
        (out/'placement-initial-window.json').write_text(json.dumps(placement_client(placed[1]),indent=2))
        if os.environ.get('NOCTALIA_TEST_PLACEMENT_DEBUG'):
            (out/'placement-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('Placement geometry inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue-geometry').exists():break
                time.sleep(.1)
        def offset_matches():
            w=placement_client(placed[1]);return w and w['floating'] and w['workspace']['id']==7 and w['size']==[600,400] and w['at']==[120,140]
        wait(offset_matches,'floating size, monitor offset and numbered workspace')
        assert json.loads(ctl('-j','activeworkspace'))['id']==3,'Silent placement stole workspace focus'
        neighbor=open_placement('placeXappone')
        assert not placement_client(neighbor[1])['floating'],'Placement class matcher was not exact'
        close_placement(neighbor)
        original_placed={k:placement_client(placed[1])[k] for k in ('at','size','workspace','floating','pinned')}
        centered=placement_config.replace('workspace="number"','workspace="named"\nworkspace_name="Place-Test"').replace('workspace_silent=true','workspace_silent=false').replace('position="offset"','position="center"')
        apply_placement(centered)
        assert {k:placement_client(placed[1])[k] for k in original_placed}==original_placed,'Editing placement moved an existing window'
        named=open_placement()
        wait(lambda:placement_client(named[1])['workspace']['name']=='Place-Test','named workspace placement')
        wait(lambda:json.loads(ctl('-j','activeworkspace'))['name']=='Place-Test','non-silent placement follows workspace')
        mon=next(m for m in json.loads(ctl('-j','monitors')) if m['name']=='TEST-1')
        left,top,right,bottom=mon['reserved']
        centered_at=[round(mon['x']+left+(mon['width']/mon['scale']-left-right-600)/2),round(mon['y']+top+(mon['height']/mon['scale']-top-bottom-400)/2)]
        wait(lambda:all(abs(a-b)<=2 for a,b in zip(placement_client(named[1])['at'],centered_at)),'initial centred position')
        close_placement(named)
        pinned=placement_config.replace('workspace="number"','workspace="inherit"').replace('pin="off"','pin="on"')
        apply_placement(pinned)
        pin_window=open_placement()
        assert placement_client(pin_window[1])['pinned']
        dispatch('hl.dsp.focus({workspace="4"})')
        wait(lambda:placement_client(pin_window[1])['workspace']['id']==4,'pinned floating window follows workspace')
        close_placement(pin_window)
        special=placement_config.replace('workspace="number"','workspace="special"\nworkspace_name="placement"')
        apply_placement(special)
        special_window=open_placement()
        wait(lambda:placement_client(special_window[1])['workspace']['name']=='special:placement','special workspace placement')
        close_placement(special_window)
        apply_placement(special.replace('workspace_name="placement"','workspace_name="bad silent"'))
        invalid_window=open_placement()
        assert not placement_client(invalid_window[1])['floating'],'Invalid placement rule partially applied'
        close_placement(invalid_window)
        # A tiled rule must override a Lua floating rule; removing it restores Lua for future windows.
        placement_lua=config.read_text()
        config.write_text(placement_lua+'\nhl.window_rule({name="placement-existing",match={class="^placement-tile-source$"},float=true})\n')
        ctl('reload');time.sleep(.3)
        tile_rule=placement_config.replace('place.app+[one]','placement-tile-source').replace('mode="floating"','mode="tiled"').replace('workspace="number"','workspace="inherit"')
        apply_placement(tile_rule)
        tiled=open_placement('placement-tile-source')
        assert not placement_client(tiled[1])['floating'],'Tiled placement did not override Lua float rule'
        close_placement(tiled)
        apply_placement('')
        inherited=open_placement('placement-tile-source')
        assert placement_client(inherited[1])['floating'],'Removing placement did not preserve original Lua rule'
        close_placement(inherited)
        config.write_text(placement_lua);ctl('reload');time.sleep(.3)
        for mode in ('light','dark'):
            apply_placement(extra='\n'+appearance+switching+'\n[theme]\nshell_mode="'+mode+'"\n')
            fresh=open_placement()
            assert placement_client(fresh[1])['workspace']['id']==7 and placement_client(fresh[1])['floating']
            close_placement(fresh)
        apply_placement()
        ctl('reload');time.sleep(.3)
        shell.terminate();shell.wait(timeout=5)
        shell=start([binary],'noctalia-placement-restart.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'placement shell restart')
        time.sleep(.3)
        fresh=open_placement()
        assert placement_client(fresh[1])['workspace']['id']==7 and placement_client(fresh[1])['size']==[600,400]
        close_placement(fresh)
        apply_placement('')
        removed=open_placement()
        assert not placement_client(removed[1])['floating'],'Removed placement still affects new windows'
        close_placement(removed)
        assert {k:placement_client(placed[1])[k] for k in original_placed}==original_placed,'Disabling placement changed an existing placed window'
        close_placement(placed);close_placement(old_placement)
        dispatch('hl.dsp.focus({monitor="TEST-1"})');dispatch('hl.dsp.focus({workspace="3"})')
        msg('settings-open','app-placement');time.sleep(1)
        run(['grim','-o','TEST-1',str(out/'app-placement-settings.png')])
        if os.environ.get('NOCTALIA_TEST_PLACEMENT_INSPECT'):
            (out/'placement-inspect.json').write_text(json.dumps({k:env[k] for k in ('HOME','XDG_RUNTIME_DIR','WAYLAND_DISPLAY','HYPRLAND_INSTANCE_SIGNATURE','DBUS_SESSION_BUS_ADDRESS','XDG_STATE_HOME')}))
            print('App placement inspection ready',flush=True)
            for _ in range(1800):
                if (out/'continue-placement').exists():break
                time.sleep(.1)
        msg('settings-close');apply_placement('')
        assert not ctl('configerrors').strip(),ctl('configerrors')
        print('PASS: opt-in exact-class placement, new-window-only geometry/workspaces/pinning, Lua restoration, theme isolation and reload/restart',flush=True)
        print('PASS: automatic theme profiles, saved profile assignment, fallback, app rule independence and restart persistence',flush=True)
        print('PASS: overview styling, preset UI, light/dark colours, reload/restart persistence and restoration',flush=True)
        print('PASS: app rules, exact matching, existing-rule restoration, scopes, plugin tags and reload/restart persistence',flush=True)
        print('PASS: Hyprland appearance, shadows/glow, theme colours, animation styles/speed, saved presets, reload/restart persistence and restoration',flush=True)
        if '--appearance-only' in sys.argv:
            sys.exit(0)
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
