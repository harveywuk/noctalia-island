"""Private-compositor binding conflicts, real key dispatch, reload and restoration."""
import json
import os
import pathlib
import subprocess
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    hypr = base / 'hyprland.lua'
    hypr.write_text(hypr.read_text() + '''
_G.test_shortcut_hits=0
_G.test_shortcut_release_hits=0
hl.bind("SUPER + F",function() _G.test_shortcut_hits=_G.test_shortcut_hits+1 end)
hl.bind("SUPER + F",function() _G.test_shortcut_release_hits=_G.test_shortcut_release_hits+1 end,{release=true})
hl.define_submap("resize",function() hl.bind("SUPER+H",function() end) end)
hl.bind("CTRL+ALT+code:32",function() end)
''')
    ctl('reload'); time.sleep(.7)
    baseline = (cfg / 'config.toml').read_text()
    def configure(chord='SUPER+F', action='workspace', target='8', enabled=True, replace=False, extra=''):
        (cfg / 'config.toml').write_text(baseline + f'''
[shell.hyprland_keybinds.test]
enabled={str(enabled).lower()}
chord={json.dumps(chord)}
action={json.dumps(action)}
target={json.dumps(target)}
replace_existing={str(replace).lower()}
{extra}
''')
        msg('config-reload'); time.sleep(.8)
        assert not ctl('configerrors').strip(), ctl('configerrors')
    def binds(): return json.loads(ctl('-j', 'binds'))
    def owned(): return [b for b in binds() if b['description'].startswith('Noctalia shortcut:')]
    def count(): return int(ctl('repl', 'return _G.test_shortcut_hits').strip())
    proto = repo / 'protocols/virtual-keyboard-unstable-v1.xml'
    run(['wayland-scanner','client-header',str(proto),str(base/'keyboard-client.h')])
    run(['wayland-scanner','private-code',str(proto),str(base/'keyboard-code.c')])
    run(['cc','-I'+str(base),str(repo/'tests/fixtures/island_keyboard.c'),str(base/'keyboard-code.c'),'-lwayland-client','-lxkbcommon','-o',str(base/'keyboard')])
    keyboard=subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def key(code, modifiers=8):
        keyboard.stdin.write(f'chord {modifiers} {code}\n');keyboard.stdin.flush()
        assert keyboard.stdout.readline().strip()=='ok';time.sleep(.4)
    def active(): return str(json.loads(ctl('-j','activeworkspace'))['id'])
    try:
        configure(); assert not owned(); n=count();key(33);assert count()==n+1
        configure(replace=True);assert len(owned())==1
        key(33);assert active()=='8' and count()==0
        ctl('reload');time.sleep(1);assert len(owned())==1
        dispatch('hl.dsp.focus({workspace="6"})');key(33);assert active()=='8'
        configure(chord='SUPER+G',target='9');assert len(owned())==1
        key(33);assert count()==1;key(34);assert active()=='9'
        configure(chord='SUPER+G',target='9',extra='\n[shell.hyprland_input]\nmouse_managed=true\npointer_sensitivity=0.2')
        assert len(owned())==1
        configure(chord='SUPER+G',extra='\n[shell.hyprland_keybinds.duplicate]\nenabled=true\nchord="super+g"')
        assert not owned()
        configure(chord='SUPER+H',replace=True);assert not owned()
        assert any(b['submap']=='resize' and b['key']=='H' for b in binds())
        configure(chord='CTRL+ALT+P',replace=True);assert not owned()
        assert any(b['key']=='' and b['modmask']==12 for b in binds())
        configure(enabled=False,replace=True);assert not owned();key(33);assert count()==1
        marker=base/'launcher-fired'
        configure(chord='SUPER+G',action='exec',target='touch '+str(marker));assert not marker.exists()
        key(34);wait(marker.exists,'launcher shortcut')
        dispatch('hl.dsp.focus({workspace="8"})')
        start(['kitty','--config','NONE','--override','confirm_os_window_close=0','--class','shortcut-test','-e','sleep','300'],'shortcut-client.log')
        def client(): return next((c for c in json.loads(ctl('-j','clients')) if c['class']=='shortcut-test'),None)
        wait(lambda:client() is not None,'shortcut test window')
        configure(chord='SUPER+G',action='fullscreen');key(34);assert client()['fullscreen']==2
        key(34);assert client()['fullscreen']==0
        configure(chord='SUPER+G',action='move_workspace',target='7');key(34);assert client()['workspace']['id']==7
        configure(chord='SUPER+G',action='close');dispatch('hl.dsp.focus({workspace="7"})');time.sleep(.4);key(34);wait(lambda:client() is None,'close shortcut')
        configure(chord='',enabled=False)
        dispatch('hl.dsp.focus({monitor="TEST-1"})');dispatch('hl.dsp.cursor.move({x=1100,y=600})')
        msg('settings-open','keybinds');time.sleep(.8)
        (out/'inspect-env.json').write_text(json.dumps(env))
        run(['grim','-o','TEST-1',str(out/'keybind-settings.png')])
        if os.environ.get('NOCTALIA_TEST_KEYBIND_INSPECT'):
            print('INSPECT: keybind settings ready',flush=True)
            deadline=time.monotonic()+int(os.environ['NOCTALIA_TEST_KEYBIND_INSPECT'])
            while time.monotonic()<deadline and not (out/'inspect-done').exists():time.sleep(.25)
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: real shortcut dispatch, conflict blocking, explicit replacement, duplicate rejection, '
              'submap/physical-key preservation, launcher, fullscreen, window movement, close and Lua restoration',flush=True)
    finally:
        keyboard.terminate();keyboard.wait(timeout=5)
