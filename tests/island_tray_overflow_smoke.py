"""Crowded media trays on private Wayland outputs, including their overflow drawer."""
import csv
import io
import json
import pathlib
import subprocess
import sys
import time

from PIL import Image


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    config = cfg/'config.toml'
    config.write_text(config.read_text().replace('[island]\n', '[island]\nmedia_gradient=false\n', 1))
    msg('config-reload')
    helpers = []
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
             *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True))
    pointer, keyboard = helpers
    output, offset_y = 'TEST-1', 0
    colors = ['e65050', '50df72', '5588ef', 'e8a344', 'c65de0', '45d7da', 'ea70a0', 'a9cf41']
    events = base/'overflow-events.txt'
    events.write_text('')
    env['ISLAND_TEST_EVENTS'] = str(events)
    apps = {}

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y+offset_y}}})'); time.sleep(.3)
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0'); time.sleep(.4)

    def click(x, y, button=''):
        move(x, y)
        command(pointer, button+'press'); time.sleep(.08); command(pointer, button+'release'); time.sleep(.6)

    def shot(name):
        filename = out/(name+'.png'); run(['grim', '-o', output, str(filename)])
        return Image.open(filename).convert('RGB').resize((1280, 720))

    def media_visible(picture, name):
        # Read original capture pixels, including when the tray itself redraws.
        # A visual preview alone is not a reliable missing-content assertion.
        regions = {
            'previous': (556, 154, 588, 184, 60),
            'playback': (624, 154, 656, 184, 60),
            'next': (692, 154, 724, 184, 60),
            'artwork': (511, 36, 555, 80, 100),
            'title': (615, 40, 755, 57, 150),
        }
        for label, (left, top, right, bottom, minimum) in regions.items():
            pixels = sum(max(picture.getpixel((x, y))) > 160
                         for y in range(top, bottom) for x in range(left, right))
            assert pixels >= minimum, ('Media content disappeared during a tray update', name, label, pixels)

    def items(name, top=200, bottom=250):
        picture = shot(name)
        if top == 200:
            media_visible(picture, name)
        found = {}
        for index, color in enumerate(colors):
            rgb = bytes.fromhex(color)
            # Island content fades independently of the capsule. Match hue so
            # the check also works while that opacity is settling.
            def matches(pixel):
                brightness = max(pixel)/max(rgb)
                return brightness > .3 and max(abs(a-b*brightness) for a, b in zip(pixel, rgb)) < 6
            # Solid square interiors exclude similarly colored wallpaper/glow.
            points = [(x, y) for y in range(top+4, bottom-4) for x in range(404, 896)
                      if matches(picture.getpixel((x, y))) and all(
                          matches(picture.getpixel((x+dx, y+dy)))
                          for dx in (-4, 0, 4) for dy in (-4, 0, 4))]
            if len(points) > 20:
                found[index] = (round(sum(x for x, y in points)/len(points)),
                                round(sum(y for x, y in points)/len(points)))
        return found

    def add(index):
        env.update(ISLAND_TRAY_LABEL=f'app{index}', ISLAND_TRAY_COLOR=colors[index])
        apps[index] = start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], f'app{index}.log')
        time.sleep(.3)

    def hover():
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.Play'])
        # Initial notification/OSD transitions can still be settling when the
        # virtual pointer first enters. Verify expansion before clicking its tray.
        for attempt in range(3):
            move(1100, 600); time.sleep(1); move(580, 40); time.sleep(1.5)
            if max(shot('hover-ready').getpixel((640, 200))) < 20:
                return
        raise AssertionError('Media Island did not expand on hover')

    def open_drawer(name):
        row = items(name+'-row')
        assert 0 < len(row) <= 3, ('Inline tray exceeded its limit', row)
        x, y = max(row.values())
        click(x+24, y+12)  # Inside the padded target, below the 16 px chevron.
        assert json.loads(msg('status'))['activePanelId'] == 'tray-drawer', 'Overflow chevron did not open its drawer'
        visible = items(name+'-open')
        assert visible == row, ('Opening the drawer changed the media row', row, visible)
        drawer = items(name+'-drawer', 260, 530)
        assert drawer and not (drawer.keys() & row.keys()), ('Drawer duplicates inline apps', row, drawer)
        return row, drawer

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Tray overflow fixture'
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        for index in range(3):
            add(index)
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=overflow-mic',
             'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=overflow-mic', '--client-name=Overflow privacy test'],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        helpers.append(capture); time.sleep(2)
        run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
             '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
             'Mail', '0', '', 'Overflow notification', 'Unread fixture', '[]', '{}', '0'])
        msg('notification-clear-active'); hover()
        initial = items('three-apps')
        assert set(initial) == {0, 1, 2}, initial
        click(initial[0][0], initial[0][1]+12)
        assert 'Activate app0' in events.read_text(), 'Padded inline target did not activate its app'
        for index in range(3, 7):
            add(index)
        time.sleep(.5)
        crowded = items('seven-apps')
        assert list(sorted(crowded, key=crowded.get)) == list(sorted(initial, key=initial.get)), (
            'Visible apps shuffled', initial, crowded)
        assert len(crowded) == 3, crowded
        picture = shot('seven-apps-status')
        left = min(x for x, y in crowded.values())
        white_columns = [x for x in range(500, left-10)
                         if any(min(picture.getpixel((x, y))) > 200 for y in range(214, 234))]
        breaks = sum(i == 0 or x > white_columns[i-1]+1 for i, x in enumerate(white_columns))
        assert breaks == 2, ('Privacy and notification icons were crowded out', white_columns)
        # Playback can update while tray items come and go. Sample several
        # captured frames, checking static media content rather than just survival.
        for cycle in range(4):
            apps[6].terminate(); apps[6].wait(timeout=5)
            add(6)
            run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
                 '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.PlayPause'])
            for frame in range(4):
                name = f'media-update-{cycle}-{frame}'
                media_visible(shot(name), name)
                time.sleep(.06)
        row, drawer = open_drawer('overflow')
        assert set(row) | set(drawer) == set(apps), ('Some apps are unreachable', row, drawer)
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.Pause'])
        add(7); time.sleep(.5)
        assert 7 in items('drawer-live-arrival', 260, 530), 'Open drawer did not update for a new app'
        apps[0].terminate(); apps[0].wait(timeout=5); del apps[0]; time.sleep(.5)
        assert set(items('inline-exit')) == {1, 2}, 'Drawer apps moved into the row while it was open'
        remaining = items('drawer-after-inline-exit', 260, 530)
        target = min(remaining)
        click(remaining[target][0]-10, remaining[target][1])
        assert f'Activate app{target}' in events.read_text(), 'Overflow item did not activate'
        assert not json.loads(msg('status'))['panelOpen'], 'Drawer did not close after activation'
        hover(); row, drawer = open_drawer('overflow-keyboard')
        target = min(drawer, key=lambda index: (drawer[index][1], drawer[index][0]))
        before_focus = shot('drawer-before-focus')
        command(keyboard, 15); time.sleep(.3)  # Tab to the first overflow app.
        focused = shot('drawer-keyboard-focus')
        x, y = drawer[target]
        assert sum(focused.getpixel((x-10, y))) > sum(before_focus.getpixel((x-10, y)))+20, (
            'Keyboard focus is not visible', target)
        before_activate = events.read_text().count(f'Activate app{target}')
        command(keyboard, 28); time.sleep(.6)  # Enter activates the focused app.
        assert events.read_text().count(f'Activate app{target}') == before_activate+1
        assert not json.loads(msg('status'))['panelOpen'], 'Keyboard activation did not close the drawer'
        hover(); row, drawer = open_drawer('overflow-menu')
        target = min(drawer)
        click(*drawer[target], button='right-')
        shot('overflow-app-menu')
        data = run(['tesseract', str(out/'overflow-app-menu.png'), 'stdout', '--tessdata-dir',
                    str(repo/'build-rishot/test-data/tessdata'), '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        words = list(csv.DictReader(io.StringIO(data), delimiter='\t', quoting=csv.QUOTE_NONE))
        action = next((word for word in words if (word.get('text') or '').lower() == 'action'), None)
        assert action, 'Overflow app menu did not open'
        click(int(action['left'])+int(action['width'])//2, int(action['top'])+int(action['height'])//2)
        assert f'MenuEvent 1 clicked app{target}' in events.read_text(), 'Overflow menu action did not reach its app'
        hover(); open_drawer('escape')
        command(keyboard, 1); time.sleep(.6)
        assert not json.loads(msg('status'))['panelOpen'], 'Escape did not dismiss the drawer'
        hover(); open_drawer('outside-click'); click(1100, 600)
        assert not json.loads(msg('status'))['panelOpen'], 'Outside click did not dismiss the drawer'

        config.write_text(config.read_text()+'\n[widget.tray]\npinned=["app6"]\nhidden=["app5"]\n')
        msg('config-reload'); hover()
        pinned = items('pinned-priority')
        assert 6 in pinned and 5 not in pinned, ('Tray preferences were ignored', pinned)
        _, drawer = open_drawer('filtered-drawer')
        assert 5 not in drawer and 6 not in drawer, drawer
        msg('panel-close'); time.sleep(.5)

        config.write_text(config.read_text()+'\n[shell.animation]\nenabled=false\n')
        msg('config-reload'); hover()
        open_drawer('reduced-motion')
        command(keyboard, 1); time.sleep(.2)
        assert not json.loads(msg('status'))['panelOpen'], 'Reduced-motion drawer did not dismiss'

        output, offset_y = 'TEST-2', 720
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); hover()
        _, drawer = open_drawer('fractional-overflow')
        assert 5 not in drawer, drawer
        for index in list(apps):
            if index not in (1, 6):
                apps[index].terminate(); apps[index].wait(timeout=5); del apps[index]
        time.sleep(.7)
        assert not json.loads(msg('status'))['panelOpen'], 'An empty overflow drawer stayed open'
        hover()
        reduced = items('overflow-cleared')
        assert set(reduced) == {1, 6}, reduced
        x, y = max(reduced.values()); click(x+24, y)
        assert not json.loads(msg('status'))['panelOpen'], 'Overflow trigger remained after it was no longer needed'
        assert shell.poll() is None
        print('PASS: capped stable media tray, visible status icons, live overflow drawer, activation, app menus, '
              'Escape/outside dismissal, pinned/hidden preferences, fractional scaling, padded targets, keyboard '
              'focus/activation, reduced motion and captured media stability', flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
