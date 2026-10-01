"""Hover delays, cancellation and monitor overrides in private Hyprland."""
import json
import os
import pathlib
import subprocess
import time
from PIL import Image


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base / 'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base / 'pointer-code.c')])
    run(['cc', '-I' + str(base), str(repo / 'tests/fixtures/island_pointer.c'),
         str(base / 'pointer-code.c'), '-lwayland-client', '-o', str(base / 'pointer')])
    # A headless seat needs a pointer device before clients receive pointer events.
    pointer = subprocess.Popen([str(base / 'pointer')], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    try:
        pointer.stdin.write('move 1100 600\n')
        pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'
        _run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell)
    finally:
        pointer.terminate()
        pointer.wait(timeout=5)


def _run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    hypr = base / 'hyprland.lua'
    hypr.write_text(hypr.read_text().replace(
        'mode="1920x1080@60",position="0x720",scale=1.5,transform=2',
        'mode="1280x720@60",position="0x720",scale=1,transform=0'))
    ctl('reload')
    time.sleep(1)
    baseline = (cfg / 'config.toml').read_text().replace(
        '[island]\nenabled=true', '[island]\nenabled=false')
    (cfg / 'config.toml').write_text(baseline + '''
[shell.animation]
enabled=false
[bar]
order=["capsule","default"]
[bar.capsule]
presentation="island"
reserve_space=false
[bar.capsule.island]
hover_open_delay_ms=1500
hover_close_delay_ms=1500
[bar.capsule.monitor.TEST-2.island]
hover_open_delay_ms=0
hover_close_delay_ms=0
''')
    msg('config-reload')
    msg('theme-mode-set', 'dark')
    time.sleep(.8)

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')

    def expanded(name, output, expected):
        path = out / (name + '-' + output + '.png')
        run(['grim', '-o', output, str(path)])
        picture = Image.open(path).convert('RGB')
        left = 640
        # Ignore Hyprland's warning overlay in the upper-right corner.
        while left > 200 and max(picture.getpixel((left - 1, 12))) < 45:
            left -= 1
        width = 2 * (640 - left)
        assert (width > 300) == expected, (name, output, width, expected)

    move(1100, 600)
    time.sleep(1.8)
    expanded('rest', 'TEST-1', False)
    move(640, 40)
    time.sleep(.3)
    expanded('waiting-open', 'TEST-1', False)
    move(1100, 600)
    time.sleep(1.7)
    expanded('cancelled-open', 'TEST-1', False)
    move(640, 40)
    time.sleep(1.7)
    expanded('opened', 'TEST-1', True)
    move(1100, 600)
    time.sleep(.3)
    expanded('waiting-close', 'TEST-1', True)
    move(640, 40)
    time.sleep(1.7)
    expanded('cancelled-close', 'TEST-1', True)
    move(1100, 600)
    time.sleep(1.7)
    expanded('closed', 'TEST-1', False)

    move(640, 760)
    time.sleep(.3)
    expanded('immediate-open', 'TEST-2', True)
    expanded('independent-monitor', 'TEST-1', False)
    move(1100, 1320)
    time.sleep(.3)
    expanded('immediate-close', 'TEST-2', False)

    move(640, 40)
    time.sleep(.1)
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('island-focus')
    time.sleep(.3)
    expanded('keyboard-immediate', 'TEST-1', True)
    msg('panel-close')
    time.sleep(.4)
    msg('settings-open', 'bar')
    time.sleep(.8)
    run(['grim', '-o', 'TEST-1', str(out / 'hover-settings.png')])
    (out / 'inspect-env.json').write_text(json.dumps(env))
    if os.environ.get('NOCTALIA_TEST_HOVER_INSPECT'):
        print('INSPECT: hover settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_HOVER_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists():
            time.sleep(.25)
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: hover open/close delays, cancelled opening, re-entry cancels closing, '
          'zero-delay monitor overrides, independent monitor state and immediate keyboard access', flush=True)
