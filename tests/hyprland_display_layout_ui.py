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
    def find_text(text, exact=False, min_y=0, park=True, min_x=0):
        """Centre of `text` on TEST-1 (see tests/ocr.py)."""
        import ocr
        if park:
            move(1276, 716); time.sleep(.2)  # keep the pointer off the text
        path = out / 'ocr.png'; run(['grim', '-o', 'TEST-1', str(path)])
        point = ocr.find(path, text, exact=exact, min_y=min_y, min_x=min_x)
        if point is None:
            raise AssertionError('Display control not found: ' + text)
        return point
    def choose(row, option, exact=True):
        """Open the dropdown on the settings row titled `row` and pick `option` from its list."""
        for _ in range(8):  # scroll up a notch at a time until the row is in view
            try:
                _, y = find_text(row, exact=True)  # the row's title stands alone on its line
                break
            except AssertionError:
                move(800, 605); command('scroll -1'); time.sleep(.4)
        else:
            raise AssertionError('Display row not found: ' + row)
        # Moving the pointer away would close the open list, so read it with the pointer in place.
        # The list opens under the dropdown, right of the tiles (which also carry display names).
        # Switching displays rebuilds the page, which can swallow a click, so retry once.
        for attempt in range(2):
            click(960, y); time.sleep(.5)
            try:
                click(*find_text(option, exact=exact, min_y=150, park=False, min_x=790)); return
            except AssertionError:
                if attempt: raise
                move(1276, 716); time.sleep(.5)
    try:
        click(*find_text('Identify displays'))
        assert identifiers() == {'TEST-1': 1, 'TEST-2': 1}
        for name in ('TEST-1', 'TEST-2'):
            run(['grim', '-o', name, str(out / ('identify-' + name + '.png'))])
        time.sleep(5)
        assert all(count == 0 for count in identifiers().values())
        # Overlapping drop cancels; subsequent valid drag snaps to exactly 1280,0. Each tile's
        # label sits at its top-left; tiles are 160 px wide and 88 px tall on this fixture.
        # The tiles are the first TEST-1 / TEST-2 text below the page heading.
        one_x, one_y = find_text('TEST-1', min_y=150)
        two_x, two_y = find_text('TEST-2', min_y=one_y + 20)
        tile_two = (two_x + 40, two_y + 30)
        drag(*tile_two, tile_two[0], one_y + 30)
        drag(*tile_two, tile_two[0] + 159, one_y + 30)
        run(['grim', '-o', 'TEST-1', str(out / 'layout-staged.png')])
        # Give TEST-2 a 125% scale, then switch to TEST-1 (keeping TEST-2's edit) and rotate it 180
        # degrees. Display options read "2 · TEST-2 · TEST-2"; the other display's name only appears
        # in the open list.
        def top():
            move(800, 400)
            for _ in range(6): command('scroll -30'); time.sleep(.08)
            time.sleep(.4)
        def select_tile(name):
            # Clicking a display's tile selects it (it gains the highlighted outline).
            top(); x, y = find_text(name, min_y=150); click(x + 40, y + 30)
            move(800, 605)
            for _ in range(3): command('scroll 3'); time.sleep(.08)
            time.sleep(.4)
        select_tile('TEST-2')
        choose('Scale', '125%')
        select_tile('TEST-1')
        choose('Rotation and flip', '180°')
        check(False, 'Staged edits changed live outputs'); assert not saved()
        bottom(); click(*find_text('Apply and test')); check(True, 'Batch preview'); assert not saved()
        run(['grim', '-o', 'TEST-1', str(out / 'layout-confirm.png')])
        time.sleep(15.5); check(False, 'Batch timeout rollback'); assert not saved()
        bottom(); click(*find_text('Apply and test')); check(True, 'Batch preview again')
        click(*find_text('Keep changes'), 1.3); check(True, 'Held Keep')
        assert saved()['TEST-1']['transform'] == 2
        assert saved()['TEST-2']['x'] == 1280 and saved()['TEST-2']['scale'] == 1.25
        ctl('reload'); time.sleep(1); check(True, 'Reload persistence')
        bottom(); click(*find_text('Use Hyprland configuration')); time.sleep(.7)
        assert monitors()['TEST-1']['transform'] == 0
        msg('settings-close'); check(True, 'Close rollback')
        # Return the private fixture to its original configuration for inspection.
        (base / 'state/noctalia/settings.toml').write_text('')
        msg('config-reload'); time.sleep(1); check(False, 'Cleanup restore')
        msg('settings-open', 'displays'); time.sleep(.8)
        assert not ctl('configerrors').strip()
        print('PASS: Identify timeout, drag snapping, fractional scale UI, multi-monitor staging, '
              '15-second rollback, Keep, reload and close rollback', flush=True)
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
