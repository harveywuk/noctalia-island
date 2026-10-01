"""The launcher hosted by the Dynamic Island: size and shape on private outputs.

Seeds desktop entries so the result list is long, opens the launcher from the Island,
records the panel's on-screen bounds, and captures both outputs.
"""
import json
import pathlib
import time


def prepare(base, cfg, env):
    apps = pathlib.Path(env['XDG_DATA_HOME'])/'applications'
    apps.mkdir(parents=True, exist_ok=True)
    for index in range(60):
        (apps/f'launcher-test-{index:02d}.desktop').write_text(
            f'[Desktop Entry]\nType=Application\nName=Launcher Test {index:02d}\n'
            f'Comment=Fixture application number {index}\nExec=true\nIcon=utilities-terminal\n'
        )


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    def shot(name):
        for output in ('TEST-1', 'TEST-2'):
            run(['grim', '-o', output, str(out/f'{name}-{output}.png')])

    def panel_bounds():
        layers = json.loads(ctl('-j', 'layers'))
        found = []
        for output, data in layers.items():
            for level in data.get('levels', {}).values():
                for layer in level:
                    if 'panel' in layer.get('namespace', '') or 'island' in layer.get('namespace', ''):
                        found.append((output, layer['namespace'], layer['x'], layer['y'], layer['w'], layer['h']))
        return found

    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    dispatch('hl.dsp.cursor.move({x=640,y=400})')
    time.sleep(1)
    msg('panel-toggle', 'launcher')
    time.sleep(1.5)
    bounds = panel_bounds()
    (out/'launcher-bounds.json').write_text(json.dumps(bounds, indent=2))
    print('surfaces:', bounds, flush=True)
    shot('launcher-open')
    msg('panel-close')
    time.sleep(.5)

    # A large output, like a 1440p desktop monitor: the panel should widen, not stretch down.
    config = cfg/'config.toml'
    original = config.read_text()
    config.write_text(original + '\n[shell.hyprland_displays.TEST-2]\nmanaged=true\nscale=1.0\ntransform=0\n')
    msg('config-reload')
    wait(lambda: next(m for m in json.loads(ctl('-j', 'monitors')) if m['name'] == 'TEST-2')['scale'] == 1.0,
         'TEST-2 did not switch to scale 1')
    time.sleep(1.5)
    dispatch('hl.dsp.focus({monitor="TEST-2"})')
    dispatch('hl.dsp.cursor.move({x=960,y=1300})')
    time.sleep(1)
    msg('panel-toggle', 'launcher')
    time.sleep(1.5)
    run(['grim', '-o', 'TEST-2', str(out/'launcher-large-TEST-2.png')])
    msg('panel-close')
    time.sleep(.5)
    assert shell.poll() is None
    print('PASS: island launcher captured', flush=True)
