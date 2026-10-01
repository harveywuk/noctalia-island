"""Private Hyprland workspace rule lifecycle and real widget scroll integration."""
import json
import os
import pathlib
import subprocess
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    hypr = base / 'hyprland.lua'
    hypr.write_text(hypr.read_text() + '''
hl.workspace_rule({workspace="3",monitor="TEST-1",persistent=false,gaps_in=13})
hl.workspace_rule({workspace="9",monitor="TEST-2",persistent=true})
''')
    ctl('reload'); time.sleep(.7)
    baseline = (cfg / 'config.toml').read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
    baseline += '''
[bar.workspace-test]
enabled=true
thickness=40
margin_ends=0
start=["workspaces"]
center=[]
end=[]
'''
    def configure(enabled=True, monitor='TEST-2', persistent=True, wrap=False, named=False, extra=''):
        text = baseline + f'''
[widget.workspaces]
scroll_wrap={str(wrap).lower()}
hide_when_empty=true
max_label_chars=1
[shell.hyprland_workspaces."3"]
enabled={str(enabled).lower()}
label="Writing"
icon="★"
monitor="{monitor}"
persistent={str(persistent).lower()}
[shell.hyprland_workspaces."4"]
enabled=true
label="Web"
monitor="TEST-1"
persistent=true
[shell.hyprland_workspaces."name:work"]
enabled={str(named).lower()}
label="Named"
monitor="TEST-2"
persistent=true
{extra}
'''
        (cfg / 'config.toml').write_text(text); msg('config-reload'); time.sleep(1)
        assert not ctl('configerrors').strip(), ctl('configerrors')
    def rules(): return {r['workspaceString']: r for r in json.loads(ctl('-j', 'workspacerules')) if r.get('enabled', True)}
    def workspaces(): return {str(w['id']): w for w in json.loads(ctl('-j', 'workspaces'))}
    def check(monitor):
        r = rules(); assert r['3']['monitor'] == monitor and r['3']['persistent'], r
        assert r['3']['gapsIn'] == [13, 13, 13, 13], r['3']
        assert workspaces()['3']['monitor'] == monitor
        assert r['9']['monitor'] == 'TEST-2' and r['9']['persistent']
    configure(); check('TEST-2')
    ctl('reload'); time.sleep(1); check('TEST-2')
    configure(monitor='TEST-1'); check('TEST-1')
    configure(monitor=''); check('TEST-1')  # Inherit the Lua assignment.
    configure(persistent=False)
    assert rules()['3']['persistent'] is False
    configure(enabled=False)
    assert rules()['3']['monitor'] == 'TEST-1' and not rules()['3']['persistent']
    configure(named=True)
    assert any(w['name'] == 'work' for w in workspaces().values())
    # A disconnected preferred monitor is retained as a rule and must not break the compositor.
    configure(monitor='DISCONNECTED')
    assert rules()['3']['monitor'] == 'DISCONNECTED'
    configure(monitor='TEST-1', extra='\n[shell.hyprland_workspaces."r[1-4]"]\nenabled=true\npersistent=true')
    assert 'r[1-4]' not in rules()
    configure(monitor='TEST-1')
    # Scroll over the real widget. This exercises the in-process invocation context;
    # external IPC remains bounded regardless of widget preference.
    proto = repo / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base / 'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base / 'pointer-code.c')])
    run(['cc', '-I' + str(base), str(repo / 'tests/fixtures/island_pointer.c'), str(base / 'pointer-code.c'), '-lwayland-client', '-o', str(base / 'pointer')])
    pointer = subprocess.Popen([str(base / 'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def scroll(value):
        pointer.stdin.write(f'scroll {value}\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'; time.sleep(.7)
    def active():
        return str(next(m for m in json.loads(ctl('-j', 'monitors')) if m['name'] == 'TEST-1')['activeWorkspace']['id'])
    try:
        dispatch('hl.dsp.focus({workspace="4"})'); time.sleep(.7)
        dispatch('hl.dsp.cursor.move({x=35,y=20})'); time.sleep(.3)
        scroll(15); assert active() == '4', active()
        configure(monitor='TEST-1', wrap=True)
        dispatch('hl.dsp.focus({workspace="4"})'); time.sleep(.7)
        dispatch('hl.dsp.cursor.move({x=35,y=20})'); time.sleep(.3)
        run(['grim', '-o', 'TEST-1', str(out / 'workspace-before-wrap.png')])
        scroll(15); assert active() == '3', active()
        scroll(-15); assert active() == '4', active()
        msg('workspace-switch', 'next'); time.sleep(.5); assert active() == '4'
        run(['grim', '-o', 'TEST-1', str(out / 'workspace-pills.png')])
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    msg('settings-open', 'workspaces'); time.sleep(.8)
    (out / 'inspect-env.json').write_text(json.dumps(env))
    run(['grim', '-o', 'TEST-1', str(out / 'workspace-settings.png')])
    if os.environ.get('NOCTALIA_TEST_WORKSPACE_INSPECT'):
        print('INSPECT: workspace settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_WORKSPACE_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists(): time.sleep(.25)
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: workspace persistence, monitor changes, Lua inheritance/restoration, named workspaces, '
          'unrelated rule preservation, reload, and native widget scroll wrapping', flush=True)
