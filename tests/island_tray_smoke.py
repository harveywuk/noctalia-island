"""System tray in the expanded Island.

A fixture StatusNotifierItem (a white square with a one-entry menu) must appear in the hover
view; left click activates it, right click opens its menu from the Island (which stays expanded
meanwhile), and choosing the entry reaches the app.
"""
import json
import os
import pathlib
import subprocess
import sys
import time


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace(
        '[island]\nenabled=true\n',
        '[island]\nenabled=true\nheight=64\nclock_size=24\nreserve_space=true\npaused_media_seconds=30\n', 1)
    text = text.replace('[shell]\n', '[shell]\noffline_mode=true\n', 1)
    path.write_text(text)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    from PIL import Image

    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
    events_path = base/'tray-events.txt'
    events_path.write_text('')
    env['ISLAND_TEST_EVENTS'] = str(events_path)
    capture = None

    def command(value):
        pointer.stdin.write(value+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.3)
        command('relative 1 0'); command('relative -1 0'); time.sleep(.4)

    def click(button=''):
        prefix = button+'-' if button else ''
        command(prefix+'press'); time.sleep(.08); command(prefix+'release'); time.sleep(.8)

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)]); return path

    def events():
        return events_path.read_text()

    def media_tray(name):
        picture = Image.open(shot(name)).convert('RGB')
        # The contextual header keeps the tray at the trailing edge beside live indicators.
        pixels={(x,y) for x in range(720,900) for y in range(20,70) if min(picture.getpixel((x,y)))>230}
        while pixels:
            pending=[pixels.pop()]; component=[]
            while pending:
                x,y=pending.pop(); component.append((x,y))
                for other in ((x-1,y),(x+1,y),(x,y-1),(x,y+1)):
                    if other in pixels:
                        pixels.remove(other); pending.append(other)
            left,right=min(x for x,y in component),max(x for x,y in component)
            top,bottom=min(y for x,y in component),max(y for x,y in component)
            width,height=right-left+1,bottom-top+1
            if 14<=width<=18 and 14<=height<=18 and len(component)>.9*width*height:
                return round((left+right)/2),round((top+bottom)/2)
        return None

    def status_row(name, count):
        # Paused media has a black background, so all three icon types can be located together.
        tray = media_tray(name+'-locate')
        assert tray, (name, 'Tray fixture missing')
        picture = Image.open(shot(name)).convert('RGB')
        columns = {}
        for x in range(720, 900):
            ys = [y for y in range(tray[1]-12, tray[1]+13) if max(picture.getpixel((x, y))) > 100]
            if ys:
                columns[x] = ys
        groups = []
        for x in columns:
            if not groups or x > groups[-1][-1] + 1:
                groups.append([])
            groups[-1].append(x)
        assert len(groups) == count, ('Status icons and tray do not share one row', name, groups)
        centers = [((xs[0]+xs[-1])/2, (min(y for x in xs for y in columns[x])
                    + max(y for x in xs for y in columns[x]))/2) for xs in groups]
        assert max(y for x, y in centers)-min(y for x, y in centers) <= 3, ('Icons are not aligned', centers)
        assert 830 <= groups[-1][-1] <= 880, ('Status row lost its trailing inset', centers)
        return [(round(x), round(y)) for x, y in centers]

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        fixture = start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'tray.log'); time.sleep(2)
        move(1100, 600); time.sleep(1)
        compact = Image.open(shot('tray-compact')).convert('RGB')
        xs = [x for x in range(compact.width) if max(compact.getpixel((x, 20))) < 12]
        ys = [y for y in range(0, 200) if max(compact.getpixel((compact.width//2, y))) < 12]
        left, right, top, bottom = min(xs), max(xs), min(ys), max(ys)
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        expanded = Image.open(shot('tray-expanded')).convert('RGB')
        # The fixture icon is a solid white square, unlike any glyph or text.
        square = None
        for y in range(bottom, 600):
            for x in range(compact.width//2 - 200, compact.width//2 + 200):
                if all(min(expanded.getpixel((x+dx, y+dy))) > 230 for dx in range(0, 12, 3) for dy in range(0, 12, 3)):
                    square = (x+6, y+6); break
            if square:
                break
        assert square, 'Tray icon missing from the expanded Island'
        move(*square); click()
        assert 'Activate' in events(), 'Left click did not activate the tray item: '+events()
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        move(*square); click('right'); time.sleep(1.5)
        menu = Image.open(shot('tray-menu')).convert('RGB')
        # The Island stays expanded while the menu it opened is showing.
        island_bottom = max(y for y in range(0, 600) if max(menu.getpixel((menu.width//2 - 150, y))) < 12)
        assert island_bottom > bottom + 40, f'Island collapsed under its tray menu (bottom {island_bottom})'
        # The menu opens just below the icon; read its only entry inside the entry's light box.
        x0, y0, x1, y1 = square[0]-110, square[1]+30, square[0]+110, square[1]+60
        region = menu.crop((x0, y0, x1, y1)).convert('L')
        region.resize((region.width*3, region.height*3), Image.LANCZOS).save(out/'tray-menu-ocr.png')
        words = run(['tesseract', str(out/'tray-menu-ocr.png'), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'Fixture' in words, 'Tray menu did not open below its icon: '+words
        ex, ey = (x0+x1)//2, (y0+y1)//2
        move(ex, ey); click(); time.sleep(.5)
        assert 'MenuEvent 1 clicked' in events(), 'Menu entry did not reach the app: '+events()
        # An item without Activate (AppIndicator apps such as Steam) opens its menu on left click instead.
        fixture.terminate(); fixture.wait(timeout=5); time.sleep(1)
        before = events()
        menu_only = dict(env, ISLAND_TRAY_MENU_ONLY='1')
        fixture = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_tray.py')], env=menu_only,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); time.sleep(2)
        move((left+right)//2 - 60, (top+bottom)//2); time.sleep(1.5)
        move(*square); click(); time.sleep(1.5)
        menu = Image.open(shot('tray-menu-only')).convert('RGB')
        region = menu.crop((x0, y0, x1, y1)).convert('L')
        region.resize((region.width*3, region.height*3), Image.LANCZOS).save(out/'tray-menu-only-ocr.png')
        words = run(['tesseract', str(out/'tray-menu-only-ocr.png'), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'Fixture' in words, 'Left click on a menu-only tray item did not open its menu: '+words
        assert 'Activate' not in events()[len(before):].replace('SecondaryActivate', ''), events()
        fixture.terminate(); fixture.wait(timeout=5)

        # Media retains the tray with the configured idle widgets still present in config.
        move(1100, 600); click()
        fixture = start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'media-tray.log')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Tray media fixture'
        player = start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'media-player.log')
        time.sleep(1.5); move(580, 40); time.sleep(1)
        square = media_tray('media-tray-expanded')
        assert square, 'Tray missing beneath expanded playback controls'
        assert square[0] > 820, ('Media tray is not in the contextual header', square)
        assert square[1] < 70, ('Tray is not aligned with the title', square)
        before = events().count('Activate')
        move(*square); click()
        assert events().count('Activate') == before+1, 'Media tray did not activate its app'
        move(*square); click('right')
        # A playback update must keep both the tray and its open menu alive.
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.Pause'])
        time.sleep(.5)
        menu = Image.open(shot('media-tray-menu')).convert('RGB')
        region = menu.crop((square[0]-110, square[1]+30, square[0]+110, square[1]+60)).convert('L')
        region.resize((region.width*3, region.height*3), Image.LANCZOS).save(out/'media-tray-menu-ocr.png')
        words = run(['tesseract', str(out/'media-tray-menu-ocr.png'), 'stdout', '--tessdata-dir', tessdata, '--psm', '7'])
        assert 'action' in words.lower(), 'Media tray menu did not survive playback update: '+words
        before = events().count('MenuEvent 1 clicked')
        move(square[0], square[1]+45); click()
        assert events().count('MenuEvent 1 clicked') == before+1, 'Media tray menu action did not reach the app'
        move(580, 40); time.sleep(1)
        before = events().count('PlayPause')
        move(640, 233); click()
        assert events().count('PlayPause') == before+1, 'Tray footer interfered with playback controls'

        # Privacy, unread history and tray items occupy one shared row, including after tray changes.
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=tray-mic',
             'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=tray-mic', '--client-name=Tray privacy test',
                                    '--stream-name=Tray capture'], env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        time.sleep(2.5)
        run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
             '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
             'Mail', '0', '', 'Tray row notification', 'Unread fixture', '[]', '{}', '0'])
        msg('notification-clear-active'); move(580, 40); time.sleep(.5)
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.Pause'])
        time.sleep(.5)
        icons = status_row('media-tray-status-row', 3)
        extra = start([sys.executable, str(repo/'tests/fixtures/island_tray.py')], 'extra-tray.log')
        time.sleep(1)
        wider = status_row('media-tray-status-row-added', 4)
        assert wider[0][0] < icons[0][0]-5, 'Status icons did not recenter when another tray item appeared'
        extra.terminate(); extra.wait(timeout=5); time.sleep(.8)
        icons = status_row('media-tray-status-row-removed', 3)
        before = events().count('Activate')
        move(*icons[-1]); click()
        assert events().count('Activate') == before+1, 'Tray click missed after sharing the status row'
        move(455, 42); click()
        panel_status = json.loads(msg('status'))
        content=run(['tesseract',str(shot('media-tray-microphone')),'stdout','--tessdata-dir',tessdata,'--psm','11'])
        assert not panel_status['panelOpen'] and 'Live input' in content, ('Microphone button missed its Live Activity', content, panel_status)
        move(455, 42); click(); time.sleep(.5)
        icons = status_row('media-tray-status-row-activity-return', 3)
        move(*icons[1]); click()
        assert json.loads(msg('status'))['activePanelId'] == 'notification-center', 'Unread button missed its panel'
        msg('panel-close')
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.Play'])
        capture.terminate(); capture.wait(timeout=5); capture = None
        msg('notification-clear-history'); time.sleep(2.5)

        # The existing tray preference also controls the media footer.
        path = cfg/'config.toml'
        original = path.read_text()
        path.write_text(original.replace('[island]\n', '[island]\nhover_show_tray=false\n', 1))
        msg('config-reload'); move(1100, 600); time.sleep(1); move(580, 40); time.sleep(1)
        assert not media_tray('media-tray-disabled'), 'Disabled tray still appears in media'
        path.write_text(original); msg('config-reload')
        move(1100, 600); time.sleep(1); move(580, 40); time.sleep(1)
        assert media_tray('media-tray-restored'), 'Tray did not return after enabling it'
        fixture.terminate(); fixture.wait(timeout=5); time.sleep(1)
        assert not media_tray('media-tray-empty'), 'Removed tray item left an icon in media'
        player.terminate(); player.wait(timeout=5)
        assert shell.poll() is None
        print('PASS: idle and media trays activate, hold the Island for menus, dispatch menu actions, '
              'survive playback updates, preserve playback controls, respect the tray preference, '
              'and hide removed items; media status indicators share the header with the tray, '
              'keep their trailing inset on item changes and title clicks switch activities; menu-only items open on left click', flush=True)
    finally:
        if capture:
            capture.terminate(); capture.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
