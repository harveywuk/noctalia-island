#!/usr/bin/env python3
"""Check island settings and resized media controls on a private Umbriel display."""
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
    (cfg/'config.toml').write_text('[island]\nenabled=true\n[bar.default]\nenabled=false\n[dock]\nenabled=false\n[shell]\nsetup_wizard_enabled=false\npolkit_agent=false\n[shell.screenshot]\ndirectory="'+str(out)+'"\n')
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
    if '--timer-only' in sys.argv or '--polish-only' in sys.argv:
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
    (base/'config/user-dirs.dirs').write_text('XDG_VIDEOS_DIR="'+str(out)+'"\n')
    config = base/'umbriel.toml'; config.write_text('[output."HEADLESS-1"]\nmode="1280x720"\n')
    env=dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base/'config'), XDG_STATE_HOME=str(base/'state'), XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'), NOCTALIA_CONFIG_HOME=str(base/'config'), NOCTALIA_STATE_HOME=str(base/'state'), NOCTALIA_DATA_HOME=str(base/'data'), WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_LIBINPUT_NO_DEVICES='1', LIBGL_ALWAYS_SOFTWARE='1', XDG_VIDEOS_DIR=str(out))
    env['NOCTALIA_ASSETS_DIR']=str(REPO/'assets')
    env['HOME']=str(base)
    env['DBUS_SYSTEM_BUS_ADDRESS']=env['DBUS_SESSION_BUS_ADDRESS']
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
        if any(mode in sys.argv for mode in ('--battery-only','--privacy-only','--polish-only','--hover-layout-only','--progress-outline-only')):
            env['DBUS_SYSTEM_BUS_ADDRESS'] = env['DBUS_SESSION_BUS_ADDRESS']
            battery = subprocess.Popen([sys.executable, str(REPO/'tests/fixtures/island_battery.py')], env=env,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            processes.append(battery)
            assert battery.stdout.readline().strip() == 'ok'
        binary=str(REPO/'build-rishot/noctalia'); shell=start([binary],'noctalia.log')
        wait(lambda:(runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(),'shell start')
        def msg(*words): return run([binary,'msg',*words])
        def ready():
            assert shell.poll() is None, f'Shell exited: {shell.returncode}'
            try: return msg('theme-mode-get').strip() in ('dark', 'light')
            except subprocess.CalledProcessError: return False
        wait(ready, 'shell IPC startup')
        key(1)  # Dismiss Umbriel's first-run keybinding hint.
        time.sleep(3)
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
            msg('settings-open','island');time.sleep(.8);move(495,273);click();time.sleep(.7);shot('settings')
            move(1070,384);click();time.sleep(.8);shot('settings-enabled')
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
            move(827,273);click();time.sleep(.7);shot('settings')
            title=Image.open(out/'hover-editor-settings.png').convert('RGB').crop((400,347,465,367))
            def editor_click(x,y):
                # Rebuilding settings can scroll the focused control into view.
                # Locate the section title so clicks follow the visible editor.
                shot('position')
                frame=Image.open(out/'hover-editor-position.png').convert('RGB')
                for top in range(300,600):
                    if ImageChops.difference(title,frame.crop((400,top,465,top+20))).getbbox() is None:
                        move(x,y+top-347);click();return
                raise AssertionError('Widgets editor title is not visible')
            original=saved()
            # Drop outside the editor cancels without changing settings.
            drag(425,538,1115,380)
            assert saved()==original, 'Outside drop must not change the layout'
            # Move a custom widget into the centre lane after the plugin.
            drag(425,538,740,573);shot('moved')
            assert saved()['hover_widgets']==[], saved()
            assert saved()['hover_widgets_center']==['test/hover:widget','test_button'], saved()
            # Reorder within a group, then drop into the now-empty left group.
            drag(657,640,750,565);shot('reordered')
            assert saved()['hover_widgets_center']==['test_button','test/hover:widget'], saved()
            drag(657,576,500,575);shot('empty-drop')
            assert saved()['hover_widgets']==['test_button'], saved()
            assert saved()['hover_widgets_center']==['test/hover:widget'], saved()
            # Widget settings shortcuts must open an actual inspector for both kinds.
            move(574,538);click();time.sleep(.8);shot('custom-settings');key(1);time.sleep(.5)
            move(806,538);click();time.sleep(.8);shot('plugin-settings')
            move(968,226);click();time.sleep(.6)
            widget_data=tomllib.loads((base/'state/noctalia/settings.toml').read_text()).get('widget',{})
            assert widget_data['test/hover:widget']['enable_scroll'] is False, widget_data
            move(986,146);click();time.sleep(.7)
            move(1090,600);command(pointer,'scroll 20');time.sleep(.5);shot('after-inspector')
            def layout_state():
                data=saved()
                return {**{key:data.get(key,[]) for key in ('hover_widgets','hover_widgets_center','hover_widgets_right')},
                        **{key:data.get(key,True) for key in ('hover_show_clock','hover_show_calendar','hover_show_media','hover_show_downloads','hover_show_timers','hover_show_batteries','hover_show_unread')}}
            before=layout_state()
            # Presets replace the hover choices together; undo restores custom and plugin references.
            for x,name,groups in [(435,'minimal',[[],['clock'],[]]),(510,'media',[['volume'],['media'],['audio_visualizer']]),(600,'system',[['sysmon'],['network'],['battery']])]:
                editor_click(x,428);time.sleep(.8);shot(name)
                current=saved()
                assert [current.get(key,[]) for key in ('hover_widgets','hover_widgets_center','hover_widgets_right')]==groups, (name,current)
                assert current.get('enabled') is True and current.get('hover_show_calendar') is False
                editor_click(730,428);time.sleep(.8);shot(name+'-undo')
                assert layout_state()==before, (name,layout_state(),before)
                assert tomllib.loads((base/'state/noctalia/settings.toml').read_text())['widget']['test/hover:widget']['enable_scroll'] is False
            # The shared picker appends to the selected group, including its existing items.
            editor_click(837,503);time.sleep(.7);shot('picker')
            move(550,267);click()
            for code in (46,38,24,46,37): key(code)  # clock
            time.sleep(.5);shot('picker-filtered');key(28);time.sleep(.8);shot('added-clock')
            assert saved()['hover_widgets_center']==['test/hover:widget','clock'], saved()
            # Arrow buttons provide the same moves as dragging.
            editor_click(734,603);time.sleep(.8);shot('arrow-moved')
            assert saved()['hover_widgets_center']==['clock'], saved()
            assert saved()['hover_widgets_right']==['right_button','test/hover:widget'], saved()
            move(1090,600);command(pointer,'scroll 1');time.sleep(.5)
            editor_click(1067,640);time.sleep(.8)
            assert saved()['hover_widgets_right']==['right_button'], saved()
            # Plugin adds use the normal named-instance workflow and keep existing entries.
            editor_click(1070,503);time.sleep(.7)
            move(550,267);click()
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
                before=plugin_state().get('clicks',0)
                move(640,y);click()
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
            move(925,273);click();time.sleep(.7);shot('section-settings')
            move(1070,384);click();time.sleep(.7);shot('section-settings-edited')
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
            key(1);move(1100,600);time.sleep(1)
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
            key(1);time.sleep(1)
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
            # The footer must remain bounded even at a larger UI scale.
            before=Image.open(out/'polish-busy-top.png').convert('RGB')
            assert before.getpixel((640,715)) != before.getpixel((640,650)), 'Island must leave clearance at the bottom'
            move(640,580);command(pointer,'scroll 100');time.sleep(.8);shot('busy-bottom')
            bottom=Image.open(out/'polish-busy-bottom.png').convert('RGB')
            assert ImageChops.difference(before.crop((410,430,870,690)),bottom.crop((410,430,870,690))).getbbox(), 'Crowded footer must scroll'
            # Progress updates should not reset scroll position or the long name marquee.
            download(0,.65);time.sleep(.3);shot('progress')
            after=Image.open(out/'polish-progress.png').convert('RGB')
            assert ImageChops.difference(bottom.crop((450,670,830,687)),after.crop((450,670,830,687))).getbbox() is None, 'Progress preserves footer position'
            # Keyboard entry scrolls the first timer into view; reverse Tab reveals Close.
            msg('island-focus');time.sleep(.7);shot('keyboard-first')
            key('shift-tab');time.sleep(.5);shot('keyboard-last');key(28);time.sleep(.7);shot('closed')
            closed=Image.open(out/'polish-closed.png').convert('RGB')
            assert ImageChops.difference(after.crop((450,200,830,680)),closed.crop((450,200,830,680))).getbbox(), 'Keyboard Close collapses the crowded view'
            msg('island-focus');time.sleep(.6);key(28)
            wait(lambda:state('timer').get('state')=='PAUSED','First keyboard control pauses Timer after reopening')
            key(1);assert shell.poll() is None
            print('PASS: crowded scaled island, footer scrolling, stable progress updates, keyboard reveal/close/pause')
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
            key(3);key(11);key(11);key(28)  # Enter 200 (2:00) in the actual plugin panel.
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
            shot('filtered');assert differs('all','filtered'), 'Existing privacy filters must apply'
            settings.write_text(original);msg('config-reload');time.sleep(.8)
            camera.terminate();camera.wait(timeout=6);screen.terminate();screen.wait(timeout=6);time.sleep(2.5)
            # Compact mic icon opens Noctalia's existing audio tab.
            move(724,40);click();time.sleep(.8);shot('audio-controls')
            assert differs('mic-hover','audio-controls',(400,90,880,330)), 'Microphone click must open the audio panel'
            key(1);move(1100,600)
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
            run(['grim',str(out/'settings.png')])
            move(1070,459);click();time.sleep(.5)
            saved=tomllib.loads((base/'state/noctalia/settings.toml').read_text())
            assert saved['island']['reserve_space'] is False, saved
            move(560,197);click();time.sleep(.5)
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
            key(1);move(1100,600);time.sleep(.5)
            long_body='\n'.join(f'Line {i:02}: A complete notification stays readable when expanded.' for i in range(1,61))+'\nEND OF FULL MESSAGE'
            run(['notify-send','-a','Island test','-t','0','Long notification',long_body])
            time.sleep(1)
            run(['grim',str(out/'notification-collapsed.png')])
            move(600,120);click();time.sleep(1)
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
            move(640,125);time.sleep(1)
            run(['grim',str(out/'notification-action-hover.png')])
            click()
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
            key(1);move(1100,600);time.sleep(1)
            run(['grim',str(out/'badge-cleared.png')])
            cleared=Image.open(out/'badge-cleared.png').convert('RGB')
            assert ImageChops.difference(empty.crop((686,28,710,52)),cleared.crop((686,28,710,52))).getbbox() is None, 'Read badge must disappear'
            for i in range(2):
                run(['notify-send','-a','Hover count test','-t','1000',f'Unread message {i+1}','Shown in the expanded island.'])
            time.sleep(1.5)
            move(640,40);time.sleep(1)
            run(['grim',str(out/'expanded-unread-count.png')])
            move(640,156);click();time.sleep(1)
            wait(lambda: all(entry['seen'] for entry in history_entries()),'Expanded unread-count row opens history')
            # Polish pass at the normal artwork, clock and calendar sizes.
            key(1);move(1100,600)
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
            key(1);move(1100,600);time.sleep(.7)
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
        before_image=Image.open(out/'keyboard-timed-before.png').convert('RGB').crop((460,10,820,100))
        after_image=Image.open(out/'keyboard-timed-after.png').convert('RGB').crop((460,10,820,100))
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
