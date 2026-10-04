"""Exercise live per-window dock previews and capture lifecycle on private outputs."""
import csv
import io
import json
import pathlib
import subprocess
import time

from PIL import Image
from dock_motion_smoke import prepare as prepare_dock


def prepare(base, cfg, env):
    prepare_dock(base, cfg, env)
    config = cfg/'config.toml'
    config.write_text(config.read_text().replace('hide_delay_ms=400', 'hide_delay_ms=200\nlayer="overlay"\nwindow_previews=true\npreview_delay_ms=450'))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    protocol = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(protocol), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(protocol), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'), '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def command(value):
        pointer.stdin.write(value+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'
    def move(x, y, delay=.12):
        dispatch(f'hl.dsp.cursor.move({{x={int(x)},y={int(y)}}})')
        # Cursor warps alone can omit Wayland motion; send real input after positioning.
        command('relative 1 0'); command('relative -1 0'); time.sleep(delay)
    def click(x, y, right=False):
        move(x, y)
        command('right-press' if right else 'press'); time.sleep(.06)
        command('right-release' if right else 'release'); time.sleep(.3)
    def shot(name, output='TEST-1'):
        dest = out/(name+'.png'); run(['grim', '-o', output, str(dest)])
        return dest
    def words(name='current', output='TEST-1', threshold=False):
        dest = shot(name, output)
        # Card titles are small; OCR reads a 3x upscale and positions are mapped back.
        image = Image.open(dest).convert('L')
        image = image.resize((image.width*3, image.height*3), Image.LANCZOS)
        if threshold:
            # Large dark cards can confuse Tesseract's automatic background detection.
            image = image.point(lambda value: 0 if value > 150 else 255)
        processed = out/'ocr-x2.png'; image.save(processed)
        tsv = run(['tesseract', str(processed), 'stdout', '--tessdata-dir', str(repo/'build-rishot/test-data/tessdata'), '--psm', '11', '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=288'])
        return [dict(row, x=int(row['left'])//3, y=int(row['top'])//3, w=int(row['width'])//3, h=int(row['height'])//3) for row in csv.DictReader(io.StringIO(tsv), delimiter='\t') if (row.get('text') or '').strip() and row.get('left', '').isdigit()]
    def matches(text, name='current', output='TEST-1', count=1):
        found = [w for w in words(name, output) if text.lower() in w['text'].lower()]
        if len(found) < count:
            alternate = [w for w in words(name, output, True) if text.lower() in w['text'].lower()]
            if len(alternate) > len(found): found = alternate
        return found
    def word(text, name='current', output='TEST-1'):
        found = matches(text, name, output)
        assert found, f'{text} missing from {name}'
        return found[0]
    def clients(): return [c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'dock-motion-test']
    def launch(title, color):
        return start(['kitty', '--config', 'NONE', '--class', 'dock-motion-test', '--title', title, '-o', 'background='+color, '-o', 'confirm_os_window_close=0', '/bin/sh'], title+'.log')
    def reveal(output=1):
        move(1100, 350 if output == 1 else 1050, .4)
        move(640, 719 if output == 1 else 1439, .35)
        move(640, 670 if output == 1 else 1390, .85)
    def config_change(old, new):
        config = cfg/'config.toml'; current = config.read_text(); assert old in current
        config.write_text(current.replace(old, new)); msg('config-reload'); time.sleep(.7)
    def capture_buffers():
        buffers = set()
        for fd in pathlib.Path(f'/proc/{shell.pid}/fd').iterdir():
            try:
                if 'noctalia-toplevel-thumbnail' in str(fd.readlink()):
                    buffers.add(fd.stat().st_ino)
            except FileNotFoundError:
                pass
        return buffers
    def assert_color(label, color, name):
        # Wait for the compositor, async copy and popup presentation, rather
        # than assuming a screenshot observes the immediately preceding commit.
        for attempt in range(12):
            image = Image.open(shot(name)).convert('RGB')
            crop = image.crop((label['x']+label['w']//2-35, label['y']-80, label['x']+label['w']//2+35, label['y']-30))
            matching = sum(max(abs(a-b) for a,b in zip(pixel,color)) < 8 for pixel in crop.getdata())
            if matching > 2000:
                return
            time.sleep(.1)
        raise AssertionError(f'Live preview did not update in {name}; buffers={capture_buffers()}')
    # Change a terminal's contents without changing its title, focus or geometry.
    painter = base/'preview-painter.py'
    painter.write_text("import pathlib, sys, time\np=pathlib.Path(sys.argv[1])\nlast=''\nwhile True:\n c=p.read_text().strip()\n if c != last:\n  print('\\033]11;'+c+'\\007\\033[2J\\033[?25l', end='', flush=True)\n  last=c\n time.sleep(.03)\n")
    def launch_live(title, color):
        control = base/(title+'.color'); control.write_text(color)
        start(['kitty', '--config', 'NONE', '--class', 'dock-motion-test', '--title', title, '-o', 'background='+color, '-o', 'confirm_os_window_close=0', 'python3', str(painter), str(control)], title+'.log')
        return control
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        alpha_control = launch_live('PreviewAlpha', '#c04444')
        beta_control = launch_live('PreviewBeta', '#228866')
        wait(lambda: len(clients()) == 2, 'Two preview windows did not appear')
        assert not capture_buffers(), 'Capture started before the popup opened'
        reveal()
        alpha = word('PreviewAlpha', 'preview-dark')
        beta = word('PreviewBeta', 'preview-dark')
        image = Image.open(out/'preview-dark.png').convert('RGB')
        for label, color in [(alpha, (192, 68, 68)), (beta, (34, 136, 102))]:
            crop = image.crop((label['x']+label['w']//2-35, label['y']-80, label['x']+label['w']//2+35, label['y']-30))
            matching = sum(max(abs(a-b) for a,b in zip(pixel,color)) < 8 for pixel in crop.getdata())
            assert matching > 2000, 'Preview did not capture the individual window contents'
        # Cross the gap into the first card; smart hide must keep the parent visible.
        move(alpha['x']+alpha['w']//2, alpha['y']-55, .8)
        word('PreviewAlpha', 'preview-hover-held')
        buffers = capture_buffers()
        assert len(buffers) == 2, buffers
        # Both cards must change while this same popover remains open.
        for index in range(3):
            alpha_color = '#3355cc' if index % 2 == 0 else '#c04444'
            beta_color = '#ccaa33' if index % 2 == 0 else '#228866'
            alpha_control.write_text(alpha_color); beta_control.write_text(beta_color)
            time.sleep(.5)
            assert_color(alpha, tuple(bytes.fromhex(alpha_color[1:])), 'preview-live-alpha'+str(index))
            assert_color(beta, tuple(bytes.fromhex(beta_color[1:])), 'preview-live-beta'+str(index))
            assert capture_buffers() == buffers, 'Live frames did not reuse capture buffers'
        # Resize a source while its stream remains active; new constraints must
        # replace the buffer and continue delivering frames to the same card.
        alpha_address = next(c['address'] for c in clients() if c['title'] == 'PreviewAlpha')
        dispatch('hl.dsp.window.float({window="address:'+alpha_address+'"})')
        dispatch('hl.dsp.window.resize({x=500,y=400,window="address:'+alpha_address+'"})')
        alpha_control.write_text('#44bbbb'); time.sleep(.8)
        assert_color(alpha, (68, 187, 187), 'preview-live-after-resize')
        assert len(capture_buffers()) == 2 and capture_buffers() != buffers, 'Resize did not replace the source buffer'
        # Return to tiling so follow-mouse cannot refocus the floating source
        # underneath the pointer when the next activation dismisses the popup.
        dispatch('hl.dsp.window.float({window="address:'+alpha_address+'"})')
        # A static source can remain pending beyond the initial capture timeout.
        time.sleep(1.3)
        move(beta['x']+beta['w']//2, beta['y']-55)
        command('press')
        beta_control.write_text('#9944bb'); time.sleep(.5)
        assert_color(beta, (153, 68, 187), 'preview-live-during-press')
        command('release'); time.sleep(.3)
        assert json.loads(ctl('-j', 'activewindow'))['title'] == 'PreviewBeta'
        move(1100, 350, .8)
        assert not any('PreviewAlpha' in w['text'] for w in words('preview-dismissed'))
        assert not capture_buffers(), 'Capture buffers survived closing the popover'

        reveal(); alpha = word('PreviewAlpha')
        # A title is centered in a 220px card; close is inset 18px from its right edge.
        click(alpha['x']+alpha['w']/2+92, alpha['y']-108)
        shot('preview-close-click')
        wait(lambda: len(clients()) == 1, 'Preview close button did not close exactly one window')
        word('PreviewBeta', 'preview-after-close')
        move(1100, 350, .8)

        # More windows than fit in the popup must remain reachable without growing off-screen.
        twins = []
        for index in range(6):
            proc = launch('SharedTitle' if index < 2 else 'ExtraWindow'+str(index), '#334488')
            wait(lambda: any(c['pid'] == proc.pid for c in clients()), 'Extra window did not appear')
            if index < 2: twins.append(next(c['address'] for c in clients() if c['pid'] == proc.pid))
        wait(lambda: len(clients()) == 7, 'Pagination test windows did not appear')
        reveal(); shot('preview-many-windows')
        header = word('Motion', 'preview-pages')
        assert len(capture_buffers()) == 6, 'Only six visible windows should capture'
        # The fixture's three-column page has its navigation buttons in the header.
        header_y = header['y'] + header['h']/2
        click(header['x']+666, header_y)
        time.sleep(.6); word('ExtraWindow', 'preview-second-page')
        assert len(capture_buffers()) == 1, 'Hidden page captures were retained'
        click(header['x']+582, header_y)
        labels = matches('SharedTitle', 'preview-twin-titles', count=2)
        assert len(labels) == 2, labels
        twin = sorted(labels, key=lambda w: (w['y']//20, w['x']))[1]
        click(twin['x']+twin['w']/2, twin['y']-55)
        assert json.loads(ctl('-j', 'activewindow'))['address'] == twins[1], 'Same-title preview activated the wrong window'
        reveal()
        labels = matches('SharedTitle', count=2)
        twin = sorted(labels, key=lambda w: (w['y']//20, w['x']))[1]
        click(twin['x']+twin['w']/2+92, twin['y']-108)
        wait(lambda: len(clients()) == 6, 'Same-title preview did not close a window')
        assert twins[0] in [c['address'] for c in clients()] and twins[1] not in [c['address'] for c in clients()], 'Same-title preview closed the wrong window'

        move(1100, 350, .7)
        config_change('[shell.animation]\nenabled=true', '[shell.animation]\nenabled=false')
        msg('theme-mode-set', 'light'); reveal(); word('Motion', 'preview-light-reduced')
        beta = word('PreviewBeta')
        beta_control.write_text('#cc9933'); time.sleep(.5)
        assert_color(beta, (204, 153, 51), 'preview-live-reduced-motion')
        move(1100, 350, .7)
        config_change('window_previews=true', 'window_previews=false'); reveal()
        assert not any('ExtraWindow' in w['text'] for w in words('preview-disabled'))
        config_change('window_previews=false', 'window_previews=true')
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); reveal(2)
        word('Motion', 'preview-fractional', 'TEST-2')
        # Configuration reload while a popup owns captures must tear it down safely.
        config_change('window_previews=true', 'window_previews=false')
        assert not capture_buffers(), 'Reload retained capture buffers'
        for client in clients(): dispatch('hl.dsp.window.close({window="address:'+client['address']+'"})')
        wait(lambda: not clients(), 'Test windows did not close before settings check')
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 350, .5)
        msg('settings-open', 'dock'); time.sleep(.6); word('Dock', 'preview-settings')
        msg('settings-close')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: live frames, buffer reuse/release, resize recovery, clicks during updates, idle resume, hover/grace, smart hide, activate/close, pagination, opt-out, light/dark, reduced motion, fractional output and reload cleanup', flush=True)
    finally:
        for client in clients(): dispatch('hl.dsp.window.close({window="address:'+client['address']+'"})')
        if pointer.poll() is None: pointer.terminate(); pointer.wait(timeout=5)
