"""Active transfer rows and notice identity, pointer and keyboard actions."""
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    from island_completion_smoke import prepare as completion_prepare
    completion_prepare(base, cfg, env)
    apps = pathlib.Path(env['XDG_DATA_HOME'])/'applications'
    apps.mkdir(parents=True, exist_ok=True)
    for app_id, name, wm_class in (
        ('org.noctalia.TransferFixture', 'Transfer Fixture', 'transfer-target'),
        ('org.noctalia.OtherFixture', 'Other Fixture', 'transfer-other'),
        ('steam', 'Steam Fixture', 'transfer-steam'),
    ):
        (apps/(app_id+'.desktop')).write_text(
            f'[Desktop Entry]\nType=Application\nName={name}\nExec=/usr/bin/false\nStartupWMClass={wm_class}\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
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
    publisher = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_downloads.py')], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    helpers.append(publisher)

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.4)

    def active_class():
        return json.loads(ctl('-j', 'activewindow')).get('class', '')

    def focus(app):
        move()
        window = next(item for item in json.loads(ctl('-j', 'clients')) if item['class'] == app)
        dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
        wait(lambda: active_class() == app, 'Focus fixture '+app)

    def progress(app, value, visible=None):
        command(publisher, json.dumps({'uri': 'application://'+app+'.desktop',
                                       'properties': {'progress': value,
                                                      'progress-visible': value < 1 if visible is None else visible}}))

    def finish(app='org.noctalia.TransferFixture'):
        progress(app, .25); time.sleep(.15)
        began = time.monotonic(); progress(app, 1.0); time.sleep(.6)
        return began

    def at(began, seconds):
        time.sleep(max(0, began+seconds-time.monotonic()))

    def shot(name):
        path = out/(name+'.png'); run(['grim', '-o', 'TEST-1', str(path)])
        return Image.open(path).convert('RGB')

    def tooltip(name):
        move(640, 40); time.sleep(.8)
        # Isolate the tooltip row from terminal borders below it. Thresholding
        # separates its small light text from the dark surface and wallpaper.
        image = shot(name).crop((480, 75, 800, 102)).convert('L')
        path = out/(name+'-tooltip.png')
        image.resize((image.width*3, image.height*3)).point(lambda p: 255 if p > 110 else 0).save(path)
        words = run(['tesseract', str(path), 'stdout', '--tessdata-dir',
                     os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                     '--psm', '7']).lower()
        return ''.join(words.split())

    def click():
        command(pointer, 'press'); command(pointer, 'release'); time.sleep(.6)

    def no_notice(name):
        # Window activation can complete before the island's closing spring.
        time.sleep(.6)
        image = shot(name).crop((505, 22, 550, 60))
        assert sum(g > 70 and g > r+15 and g > b+15 for r, g, b in image.getdata()) < 10

    def key_count():
        return len((out/'other-keys').read_bytes()) if (out/'other-keys').exists() else 0

    def artwork(name, crop=(560, 16, 720, 24)):
        before = shot(name).crop(crop); time.sleep(.4)
        after = shot(name+'-artwork').crop(crop)
        assert sum(max(p) > 12 for p in after.getdata()) > after.width*after.height/2
        assert ImageChops.difference(before, after).getbbox(), 'Transfer action covered or froze the artwork'

    def focus_rows():
        msg('island-focus'); time.sleep(.7)

    def hover_rows(name, heading='downloads'):
        # Exclusive keyboard focus can leave pointer hover suppressed. Leave
        # first, then verify expansion before clicking where a row will be.
        focus('transfer-other'); move(735, 12)
        for _ in range(10):
            image = shot(name).crop((512, 68, 805, 96)).convert('L')
            path = out/(name+'-heading.png')
            image.resize((image.width*3, image.height*3)).point(lambda p: 255 if p > 110 else 0).save(path)
            words = run(['tesseract', str(path), 'stdout', '--tessdata-dir',
                         os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                         '--psm', '7']).lower()
            if heading in ''.join(words.split()):
                time.sleep(.4); move(640, 130)
                return
            time.sleep(.15)
        raise AssertionError('Transfer card did not expand: '+name)

    def focus_outline(name, row):
        # Sample the straight left edge of the row, away from its app glyph,
        # text, progress bar and the activity tabs above it. Allow the resize
        # and focus colour animations to settle before inspecting the outline.
        for _ in range(15):
            image = shot(name).crop((471, 121+55*row, 476, 140+55*row))
            if sum(b > 170 and 70 < g < 190 and r < 80 for r, g, b in image.getdata()) > 15:
                return
            time.sleep(.1)
        raise AssertionError(name)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        kitty = ['kitty', '--config', 'NONE', '-o', 'confirm_os_window_close=0']
        target = start(kitty+['--class', 'transfer-target', '-e', 'sleep', '300'], 'target.log')
        steam_window = start(kitty+['--class', 'transfer-steam', '-e', 'sleep', '300'], 'steam-window.log')
        recorder = "import os,sys,tty; tty.setraw(0)\nf=open(sys.argv[1],'ab',buffering=0)\nwhile True: f.write(os.read(0,1))"
        start(kitty+['--class', 'transfer-other', '-e', sys.executable, '-c', recorder, str(out/'other-keys')], 'other.log')
        wait(lambda: len(json.loads(ctl('-j', 'clients'))) == 3, 'Fixture windows')
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        time.sleep(1)

        app = 'org.noctalia.TransferFixture'
        other_app = 'org.noctalia.OtherFixture'
        focus('transfer-other'); progress(app, .31); time.sleep(.5)
        hover_rows('running-row-open')
        artwork('running-row-hover', (560, 148, 720, 152))
        click(); wait(lambda: active_class() == 'transfer-target', 'Running row activates source')

        # Tab and Shift+Tab traverse rows; percentage updates retain selection.
        progress(other_app, .22)
        for key, name in ((28, 'enter'), (57, 'space')):
            focus('transfer-other'); focus_rows()
            command(keyboard, 15); command(keyboard, 'shift-tab'); command(keyboard, 15)
            progress(app, .47); time.sleep(.4)
            focus_outline('running-row-'+name, 1)
            artwork('running-row-'+name+'-background')
            command(keyboard, key)
            wait(lambda: active_class() == 'transfer-target', 'Keyboard activates selected running row')

        # Removing an earlier row keeps focus on the same transfer as it moves.
        focus('transfer-other'); focus_rows(); command(keyboard, 15)
        focus_outline('running-row-before-move', 1)
        progress(other_app, .22, False); time.sleep(.5)
        focus_outline('running-row-moved', 0)
        command(keyboard, 28)
        wait(lambda: active_class() == 'transfer-target', 'Focus follows transfer identity after reordering')

        # Removing the selected row releases input instead of selecting another
        # app. Pointer gestures are also cancelled when their row disappears.
        progress(other_app, .22)
        focus('transfer-other'); focus_rows(); command(keyboard, 15)
        focus_outline('running-row-before-removal', 1)
        progress(app, .47, False); time.sleep(.6)
        before = key_count(); command(keyboard, 57)
        wait(lambda: key_count() > before, 'Removed row returned keyboard input')
        progress(app, .47)
        hover_rows('gesture-row-open')
        assert active_class() == 'transfer-other'
        command(pointer, 'press'); time.sleep(.2); shot('running-row-pressed')
        progress(other_app, .22, False); time.sleep(.5); shot('pressed-row-removed')
        command(pointer, 'release'); time.sleep(.4)
        assert active_class() != 'transfer-target', 'Removed row activated its replacement'
        progress(app, .47, False); move()

        # A script id that resembles an app does not manufacture a target.
        msg('island-activity-start', app, 'Local script')
        hover_rows('script-row-open', 'inprogress')
        click(); assert active_class() != 'transfer-target'
        msg('island-activity-end', app); move()

        focus('transfer-other'); finish()
        assert 'transferfixture' in tooltip('source-tooltip')
        click()
        wait(lambda: active_class() == 'transfer-target', 'Pointer activates source WM class')
        no_notice('source-opened')

        for key, name in ((28, 'enter'), (57, 'space')):
            focus('transfer-other'); finish()
            msg('island-focus'); time.sleep(.6)
            before = shot('keyboard-'+name).crop((560, 16, 720, 24)); time.sleep(.4)
            after = shot('keyboard-'+name+'-artwork').crop((560, 16, 720, 24))
            assert ImageChops.difference(before, after).getbbox(), 'Keyboard action covered the live artwork'
            command(keyboard, key)
            wait(lambda: active_class() == 'transfer-target', 'Keyboard activates source')
            no_notice('keyboard-'+name+'-opened')

        # Expiry releases the keyboard grab instead of transferring Enter/Space
        # onto media controls or trapping input on an invisible notice.
        focus('transfer-other'); began = finish()
        msg('island-focus'); time.sleep(.5)
        at(began, 5.8)
        before = key_count(); command(keyboard, 57)
        wait(lambda: key_count() > before, 'Expiry returned keyboard input to the app')
        no_notice('focused-notice-expired')

        # Replacing the source invalidates a keyboard selection. It requires a
        # fresh focus command before the new notice can be activated.
        focus('transfer-other'); finish(); msg('island-focus'); time.sleep(.5)
        began = finish('org.noctalia.OtherFixture')
        before = key_count(); command(keyboard, 57)
        wait(lambda: key_count() > before, 'Replacement returned keyboard input')
        assert 'otherfixture' in tooltip('replacement-source')
        move(); at(began, 5.8)

        # Cancelling a pointer gesture across a replacement must also be safe.
        focus('transfer-other'); finish(); move(640, 40)
        command(pointer, 'press')
        began = finish('org.noctalia.OtherFixture')
        command(pointer, 'release'); time.sleep(.3)
        assert 'otherfixture' in tooltip('replacement-after-press')
        move(); at(began, 5.8)

        # Source identity survives the running app disappearing. The notice
        # still names it, but clicking cannot launch a replacement process.
        focus('transfer-other'); progress(app, .42); focus_rows()
        target.terminate(); target.wait(timeout=5); time.sleep(1.3)
        before = key_count(); command(keyboard, 57)
        wait(lambda: key_count() > before, 'Closed source returned keyboard input')
        began = finish()
        assert 'transferfixture' in tooltip('closed-source-tooltip')
        click(); assert active_class() != 'transfer-target'
        move(); at(began, 5.8)

        msg('island-activity-start', 'archive', 'Documents archive')
        msg('island-activity-update', 'archive', '100')
        began = time.monotonic(); msg('island-activity-end', 'archive'); time.sleep(.6)
        assert 'documentsarchive' in tooltip('script-source-tooltip')
        click(); move(); at(began, 5.8)

        # An isolated process/log supplies a real Steam reader failure event.
        # The current desktop's Steam client and files are never used.
        root = pathlib.Path(env['HOME'])/'.steam/steam/logs'; root.mkdir(parents=True)
        steam = subprocess.Popen([sys.executable, '-c',
            "import ctypes,os,pathlib,time; ctypes.CDLL(None).prctl(15,b'steam',0,0,0); "
            "pathlib.Path(os.environ['HOME']+'/.steam/steam.pid').write_text(str(os.getpid())); time.sleep(300)"], env=env)
        helpers.append(steam)
        log = root/'content_log.txt'; log.touch(); time.sleep(2.5)
        def steam_event(message, app_id=42):
            with log.open('a') as output:
                output.write(time.strftime('[%Y-%m-%d %H:%M:%S]')+f' AppID {app_id} '+message+'\n')
        steam_event('App update changed : Running Update,Downloading,'); time.sleep(2.5)
        focus('transfer-other'); focus_rows(); focus_outline('steam-running-row', 0)
        command(keyboard, 57)
        wait(lambda: active_class() == 'transfer-steam', 'Running Steam row activates Steam')
        steam_event('update canceled : Priority (Suspended)'); time.sleep(2.5)
        focus('transfer-other'); focus_rows(); focus_outline('steam-paused-row', 0)
        artwork('steam-paused-row-background', (560, 148, 720, 152))
        # Both games share an app and fallback display name. Focus must follow
        # the game id when a running job moves ahead of the paused selection.
        steam_event('App update changed : Running Update,Downloading,', 43); time.sleep(2.5)
        focus_outline('steam-paused-row-moved', 1)
        command(keyboard, 28)
        wait(lambda: active_class() == 'transfer-steam', 'Paused Steam row activates Steam')
        steam_event('update canceled : User canceled', 43); time.sleep(2.5)
        hover_rows('steam-paused-open')
        click(); wait(lambda: active_class() == 'transfer-steam', 'Pointer activates paused Steam row')
        focus('transfer-other')
        steam_event('update canceled : Failed updating depot 42 (No connection to content servers)')
        time.sleep(2.3)
        assert 'steamfixture' in tooltip('failure-source-tooltip')
        click()
        wait(lambda: active_class() == 'transfer-steam', 'Failed transfer returns to Steam')
        assert steam_window.poll() is None and shell.poll() is None
        print('PASS: running/paused rows, Tab/Shift+Tab and Enter/Space, progress/reorder focus, removed rows, '
              'artwork, source tooltips, desktop/WM-class matching, notice actions, expiry, replacement gestures, '
              'closed apps, script titles and Steam failure actions', flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
