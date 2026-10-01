"""Startup launches and restart recovery inside the private Hyprland fixture."""
import json
import os
import pathlib
import subprocess
import time


def prepare(base, cfg, env):
    # A private bus has no user systemd manager. Emulate only systemd-run's spawn
    # boundary, keeping real desktop-entry and /bin/sh execution inside this fixture.
    binaries = base / 'bin'
    binaries.mkdir()
    runner = binaries / 'systemd-run'
    runner.write_text('''#!/usr/bin/python3
import os,subprocess,sys
args=sys.argv[sys.argv.index('--')+1:]
work=next((a.split('=',1)[1] for a in sys.argv if a.startswith('--working-directory=')),None)
subprocess.Popen(args,cwd=work,env=os.environ,start_new_session=True,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
''')
    runner.chmod(0o755)
    env['PATH'] = str(binaries) + ':' + env['PATH']
    apps = base / 'data/applications'
    apps.mkdir(parents=True)
    app = apps / 'noctalia-startup-test.desktop'
    app.write_text(f'''[Desktop Entry]
Type=Application
Name=Startup Test App
Exec=/bin/sh -c "printf app >> {base}/app-fired"
Path={base}
Terminal=false
''')
    def command(name, delay=0, enabled=True):
        return f'''[shell.session.startup_apps.{name}]
enabled={str(enabled).lower()}
kind="command"
command="printf {name} >> {base}/{name}-fired"
delay_seconds={delay}
'''
    with (cfg / 'config.toml').open('a') as f:
        f.write('\n' + command('immediate') + command('delayed', 20) + command('disabled', enabled=False))
        f.write('''[shell.session.startup_apps.app]
enabled=true
kind="app"
desktop_id="noctalia-startup-test"
''')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    binary = str(pathlib.Path(__file__).resolve().parents[1] / 'build-rishot/noctalia')
    wait(lambda: (base/'immediate-fired').exists() and (base/'app-fired').exists(), 'startup command and desktop app')
    assert (base/'immediate-fired').read_text() == 'immediate'
    assert (base/'app-fired').read_text() == 'app'
    assert not (base/'delayed-fired').exists() and not (base/'disabled-fired').exists()
    state = next(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-startup-*.json'))
    plan = json.loads(state.read_text())
    due = next(r['due'] for r in plan['entries'] if r['id']=='delayed')
    # Adding an enabled entry and reloading must not launch it in the same login.
    with (cfg/'config.toml').open('a') as f:
        f.write(f'''[shell.session.startup_apps.new]
enabled=true
kind="command"
command="touch {base}/new-fired"
''')
    msg('config-reload'); time.sleep(.5)
    assert not (base/'new-fired').exists()
    shell.terminate(); shell.wait(timeout=10)
    shell = start([binary], 'noctalia-restarted.log')
    def ready():
        try: return run([binary,'msg','record-status']).strip()=='idle'
        except subprocess.CalledProcessError: return False
    wait(ready, 'restarted shell IPC')
    assert next(r['due'] for r in json.loads(state.read_text())['entries'] if r['id']=='delayed') == due
    for _ in range(250):
        if (base/'delayed-fired').exists(): break
        time.sleep(.1)
    assert (base/'delayed-fired').read_text() == 'delayed'
    assert (base/'immediate-fired').read_text() == 'immediate'
    assert (base/'app-fired').read_text() == 'app'
    assert not (base/'new-fired').exists() and not (base/'disabled-fired').exists()
    shell.terminate(); shell.wait(timeout=10)
    shell = start([binary], 'noctalia-restarted-again.log')
    wait(ready, 'second restarted shell IPC'); time.sleep(.5)
    assert (base/'delayed-fired').read_text() == 'delayed'
    assert (base/'immediate-fired').read_text() == 'immediate'
    assert (base/'app-fired').read_text() == 'app'
    dispatch('hl.dsp.focus({monitor="TEST-1"})'); dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    msg('settings-open', 'session'); time.sleep(.8)
    (out/'inspect-env.json').write_text(json.dumps(env))
    run(['grim','-o','TEST-1',str(out/'startup-settings.png')])
    if os.environ.get('NOCTALIA_TEST_STARTUP_INSPECT'):
        print('INSPECT: startup settings ready',flush=True)
        deadline=time.monotonic()+int(os.environ['NOCTALIA_TEST_STARTUP_INSPECT'])
        while time.monotonic()<deadline and not (out/'inspect-done').exists():time.sleep(.25)
    assert shell.poll() is None
    assert not ctl('configerrors').strip()
    print('PASS: command and desktop app launch, delay, restart recovery, no duplicates or reload launches, disabled entries',flush=True)
