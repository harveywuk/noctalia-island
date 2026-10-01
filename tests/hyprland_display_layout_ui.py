"""Exercise display layout staging and confirmation on the private 1280x720 fixture."""
import json
import pathlib
import subprocess
import time
import tomllib


def run_checks(base, out, env, run, ctl, dispatch, msg):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base / 'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base / 'pointer-code.c')])
    run(['cc', '-I' + str(base), str(repo / 'tests/fixtures/island_pointer.c'),
         str(base / 'pointer-code.c'), '-lwayland-client', '-o', str(base / 'pointer')])
    pointer = subprocess.Popen([str(base / 'pointer')], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    def command(value):
        pointer.stdin.write(value + '\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'
    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.1)
    def click(x, y, hold=.12):
        move(x, y); command('press'); time.sleep(hold); command('release'); time.sleep(.6)
    def drag(x, y, target_x, target_y):
        move(x, y); command('press'); time.sleep(.15)
        move(target_x, target_y); command('release'); time.sleep(.6)
    def bottom():
        move(800, 605)
        for _ in range(12): command('scroll 30'); time.sleep(.08)
        time.sleep(.4)
    def monitors(): return {m['name']: m for m in json.loads(ctl('-j', 'monitors'))}
    def saved():
        path = base / 'state/noctalia/settings.toml'
        return tomllib.loads(path.read_text()).get('shell', {}).get('hyprland_displays', {}) if path.exists() else {}
    def check(preview, label):
        time.sleep(.6); m = monitors()
        actual = (m['TEST-1']['transform'], m['TEST-2']['x'], m['TEST-2']['y'], m['TEST-2']['scale'])
        assert actual == ((2, 1280, 0, 1.25) if preview else (0, 0, 720, 1.5)), (label, actual)
    def identifiers():
        return {name: sum(layer['namespace'] == 'noctalia-display-identify'
                         for group in data['levels'].values() for layer in group)
                for name, data in json.loads(ctl('-j', 'layers')).items()}
    try:
        click(735, 627)
        assert identifiers() == {'TEST-1': 1, 'TEST-2': 1}
        for name in ('TEST-1', 'TEST-2'):
            run(['grim', '-o', name, str(out / ('identify-' + name + '.png'))])
        time.sleep(5)
        assert all(count == 0 for count in identifiers().values())
        # Overlapping drop cancels; subsequent valid drag snaps to exactly 1280,0.
        drag(745, 514, 745, 425)
        drag(745, 514, 904, 425)
        run(['grim', '-o', 'TEST-1', str(out / 'layout-staged.png')])
        # Scroll to the selected monitor's controls without wheel acceleration.
        drag(1240, 320, 1240, 455)
        click(1100, 460); click(960, 358)  # TEST-2 scale 125%.
        click(1100, 312); click(960, 358)  # Switch to TEST-1, retaining TEST-2 edits.
        click(1100, 611); click(960, 473)  # TEST-1 transform 180 degrees.
        check(False, 'Staged edits changed live outputs'); assert not saved()
        bottom(); click(350, 625); check(True, 'Batch preview'); assert not saved()
        run(['grim', '-o', 'TEST-1', str(out / 'layout-confirm.png')])
        time.sleep(15.5); check(False, 'Batch timeout rollback'); assert not saved()
        bottom(); click(350, 625); check(True, 'Batch preview again')
        click(350, 365, 1.3); check(True, 'Held Keep')
        assert saved()['TEST-1']['transform'] == 2
        assert saved()['TEST-2']['x'] == 1280 and saved()['TEST-2']['scale'] == 1.25
        ctl('reload'); time.sleep(1); check(True, 'Reload persistence')
        bottom(); click(650, 625); time.sleep(.7)
        assert monitors()['TEST-1']['transform'] == 0
        click(1227, 53); check(True, 'Close rollback')
        # Return the private fixture to its original configuration for inspection.
        (base / 'state/noctalia/settings.toml').write_text('')
        msg('config-reload'); time.sleep(1); check(False, 'Cleanup restore')
        msg('settings-open', 'displays'); time.sleep(.8)
        assert not ctl('configerrors').strip()
        print('PASS: Identify timeout, drag snapping, fractional scale UI, multi-monitor staging, '
              '15-second rollback, Keep, reload and close rollback', flush=True)
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
