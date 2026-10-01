"""Boot the shell on examples/starter.toml and capture the result.

The harness's own test keys (offline mode, no wizard or polkit agent, a dead plugin
source) are layered on top so the private session cannot block or reach the network.
"""
import pathlib
import time


def prepare(base, cfg, env):
    repo = pathlib.Path(__file__).resolve().parents[1]
    starter = (repo/'examples/starter.toml').read_text()
    starter = starter.replace('setup_wizard_enabled = true', 'setup_wizard_enabled = false\npolkit_agent = false\noffline_mode = true')
    (cfg/'config.toml').write_text(starter)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    start(['kitty', '--config', 'NONE', '--class', 'starter-test', '-e', 'sleep', '60'], 'kitty.log')
    time.sleep(3)
    run(['grim', '-o', 'TEST-1', str(out/'starter-TEST-1.png')])
    dispatch('hl.dsp.cursor.move({x=640,y=40})')
    time.sleep(1.5)
    run(['grim', '-o', 'TEST-1', str(out/'starter-island-hover.png')])
    assert shell.poll() is None
    assert 'error' not in run([str(pathlib.Path(__file__).resolve().parents[1]/'build-rishot/noctalia'),
                               'config', 'validate']).lower()
    print('PASS: starter configuration boots and validates', flush=True)
