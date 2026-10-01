"""Display persistence and restoration on two private outputs; optional UI inspection."""
import json
import os
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    baseline = (cfg / 'config.toml').read_text().replace('enabled=true\nhover_widgets', 'enabled=true\nreserve_space=false\nhover_widgets')
    def monitors(): return {m['name']: m for m in json.loads(ctl('-j', 'monitors'))}
    original = monitors()
    def configure(managed):
        (cfg / 'config.toml').write_text(baseline + '\n[shell.hyprland_displays.TEST-2]\nmanaged=' + str(managed).lower() + '\ntransform=1\nvrr=2\ncolor_mode="srgb"\nbit_depth=8\nsdr_brightness=1.2\nsdr_saturation=0.9\n')
        msg('config-reload'); time.sleep(1)
    configure(True)
    wait(lambda: monitors()['TEST-2']['transform'] == 1, 'Display override applied')
    after = monitors()
    for name in original:
        for key in ('x', 'y', 'scale'):
            assert after[name][key] == original[name][key], (name, key, after[name][key], original[name][key])
    assert after['TEST-1']['transform'] == original['TEST-1']['transform']
    assert abs(after['TEST-2']['sdrBrightness'] - 1.2) < .01
    ctl('reload'); time.sleep(1)
    assert monitors()['TEST-2']['transform'] == 1, 'Display override lost on reload'
    configure(False)
    wait(lambda: monitors()['TEST-2']['transform'] == original['TEST-2']['transform'], 'Display restored to Lua')
    # Multiple monitor positions/scales persist together and restore independently.
    first = '\n[shell.hyprland_displays.TEST-1]\nmanaged=true\nscale=1.25\nposition_managed=true\nx=100\ny=100\n'
    second = '\n[shell.hyprland_displays.TEST-2]\nmanaged=true\nscale=1.25\nposition_managed=true\nx=-1536\ny=0\n'
    (cfg / 'config.toml').write_text(baseline + first + second)
    msg('config-reload'); time.sleep(1)
    layout = monitors()
    assert [(layout[name]['x'], layout[name]['y'], layout[name]['scale']) for name in ('TEST-1', 'TEST-2')] == [(100, 100, 1.25), (-1536, 0, 1.25)]
    ctl('reload'); time.sleep(1)
    assert monitors()['TEST-2']['x'] == -1536 and monitors()['TEST-1']['scale'] == 1.25
    (cfg / 'config.toml').write_text(baseline + first + second.replace('managed=true', 'managed=false'))
    msg('config-reload'); time.sleep(1)
    restored = monitors()
    assert (restored['TEST-2']['x'], restored['TEST-2']['y'], restored['TEST-2']['scale']) == (original['TEST-2']['x'], original['TEST-2']['y'], original['TEST-2']['scale'])
    assert restored['TEST-1']['x'] == 100 and restored['TEST-1']['scale'] == 1.25
    (cfg / 'config.toml').write_text(baseline)
    msg('config-reload'); time.sleep(.7)
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('settings-open', 'displays'); time.sleep(.8)
    from hyprland_display_layout_ui import run_checks as run_ui_checks
    run_ui_checks(base, out, env, run, ctl, dispatch, msg)
    run(['grim', '-o', 'TEST-1', str(out / 'display-settings.png')])
    (out / 'inspect-env.json').write_text(json.dumps(env))
    if os.environ.get('NOCTALIA_TEST_DISPLAY_INSPECT'):
        print('INSPECT: display settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_DISPLAY_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists(): time.sleep(.25)
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: per-display configuration, layout preservation, fractional scaling, negative positions, reload persistence and independent restoration', flush=True)
