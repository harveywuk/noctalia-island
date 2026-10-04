"""Cupertino surfaces and the bundled community palette on private Wayland outputs."""
import csv
import io
import json
import os
import pathlib
import subprocess
import time
import tomllib

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace('[shell]\n', '[shell]\noffline_mode=true\n')
    text = text.replace('[dock]\nenabled=false', '[dock]\nenabled=true\npinned=["cupertino-preview.desktop"]\nmargin_edge=12\nconcave_edge_corners=false')
    path.write_text(text)
    apps = pathlib.Path(env['XDG_DATA_HOME'])/'applications'
    apps.mkdir(parents=True, exist_ok=True)
    (apps/'cupertino-preview.desktop').write_text(
        '[Desktop Entry]\nType=Application\nName=Cupertino Preview\nComment=Visual shell test\n'
        'Exec=true\nIcon=preferences-desktop-theme\nCategories=Utility;\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    proto = repo/'protocols/virtual-keyboard-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'keyboard-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'keyboard-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_keyboard.c'), str(base/'keyboard-code.c'),
         '-lwayland-client', '-lxkbcommon', '-o', str(base/'keyboard')])
    keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def command(process, value):
        process.stdin.write(str(value)+'\n'); process.stdin.flush()
        assert process.stdout.readline().strip() == 'ok'
    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.15)
    def click(x, y):
        move(x, y); command(pointer, 'press'); time.sleep(.1); command(pointer, 'release'); time.sleep(.6)
    def shot(name, monitor='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', monitor, str(path)]); return path
    def words(name='current'):
        # Settings captions and text on grey fields are at the edge of what OCR reads at 1x, so
        # read a 2x upscale and report positions in screen pixels.
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        image = Image.open(shot(name))
        upscaled = out/(name+'-ocr.png')
        image.resize((image.width*2, image.height*2), Image.LANCZOS).save(upscaled)
        output = run(['tesseract', str(upscaled), 'stdout', '--tessdata-dir', tessdata, '--psm', '11',
                      '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=192'])
        rows = [r for r in csv.DictReader(io.StringIO(output), delimiter='\t', quoting=csv.QUOTE_NONE)
                if r.get('text', '').strip()]
        for r in rows:
            for key in ('left', 'top', 'width', 'height'):
                r[key] = str(int(r[key])//2)
        return rows
    def find_text(text):
        """Centre of the first visible run of words matching `text`, scrolling down to find it."""
        target = text.lower().split()
        for _ in range(12):
            rows = words()
            for i in range(len(rows)-len(target)+1):
                selected = rows[i:i+len(target)]
                # OCR sometimes reads a stray quote or bar at a word's edge.
                if [r['text'].lower().strip('‘’“”"\'|.,:;') for r in selected] == target:
                    x = int(selected[0]['left'])+int(selected[0]['width'])//2
                    y = int(selected[0]['top'])+int(selected[0]['height'])//2
                    if y < 640:
                        return x, y
            move(1000, 575); command(pointer, 'scroll 3'); time.sleep(.3)
        raise AssertionError('Missing control: '+text)
    def click_text(text):
        click(*find_text(text))
    def click_row_control(description):
        """Click the control at the right edge of the settings row whose description starts with
        `description`. Text on the grey fields (pickers, segmented buttons) is too low in contrast
        to OCR reliably; row descriptions are not, and unlike titles they don't recur in banners."""
        _, y = find_text(description)
        before = Image.open(shot('row-control-before')).convert('L')
        click(1040, y-13); time.sleep(.4)
        after = Image.open(shot('row-control-after')).convert('L')
        changed = ImageChops.difference(before, after).point(lambda v: 255 if v > 40 else 0).getbbox()
        assert changed, 'Clicking the row control for '+repr(description)+' opened nothing'
    def close():
        msg('panel-close'); msg('settings-close'); move(1100, 580); time.sleep(.4)
    def panel(name, context='', capture=None):
        close(); msg('panel-open', name, *([context] if context else [])); time.sleep(.9)
        assert shell.poll() is None
        return shot(capture or name)
    try:
        ctl('dismissnotify')
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 580)
        # First capture the common styling using the pre-existing default palette.
        panel('launcher', 'Cupertino', 'original-palette-launcher')
        text = ' '.join(r['text'] for r in words()).lower()
        assert 'cupertino' in text, text
        command(keyboard, 1); time.sleep(.4)  # Escape dismisses the launcher.

        for mode in ('dark', 'light'):
            close(); msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', mode); time.sleep(.8)
            assert msg('color-scheme-get') == 'community macOS'
            assert msg('theme-mode-get') == mode
            shot(mode+'-island-dock')
            move(640, 670); time.sleep(1.1); shot(mode+'-dock-tooltip')
            command(pointer, 'right-press'); time.sleep(.1); command(pointer, 'right-release'); time.sleep(.6)
            shot(mode+'-dock-menu'); command(keyboard, 1); move(1100, 580)
            panel('launcher', 'Cupertino', mode+'-launcher')
            for tab in ('home', 'audio', 'network', 'bluetooth', 'calendar', 'notifications'):
                panel('control-center', tab, mode+'-control-center-'+tab)
            panel('session', capture=mode+'-session')  # No session action is activated.
            panel('clipboard', capture=mode+'-clipboard')
            close(); msg('volume-osd', '65'); time.sleep(.35); shot(mode+'-volume')
            time.sleep(1.5)
            run(['notify-send', '-a', 'Calendar', '-t', '12000', 'Design review', 'The updated shell is ready to preview.'])
            time.sleep(.5); shot(mode+'-notification')
            panel('control-center', 'notifications', mode+'-notification-history')
            text = ' '.join(r['text'] for r in words()).lower()
            assert 'design' in text and 'review' in text, text
            msg('notification-clear-active')
            close(); msg('settings-open', 'appearance'); time.sleep(.9); click_text('Theme')
            click_row_control('Choose a palette')
            shot(mode+'-community-preset')
            # The trigger above was located from its row's label; choose the sole
            # offline catalog entry through the real picker keyboard path.
            command(keyboard, 108); command(keyboard, 28); time.sleep(.5)
            assert msg('color-scheme-get') == 'community macOS'
            close()

        # Segmented controls still persist changes; keyboard focus remains visible on cards.
        msg('settings-open', 'appearance'); time.sleep(.8); click_text('Theme'); click_text('Dark')
        wait(lambda: msg('theme-mode-get') == 'dark', 'Theme segment did not activate')
        close(); msg('panel-open', 'control-center', 'audio'); time.sleep(.8)
        command(keyboard, 15); command(keyboard, 15); shot('dark-keyboard-focus')
        close(); dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(1100, 1100)
        msg('panel-open', 'control-center', 'audio'); time.sleep(.9); shot('fractional-audio', 'TEST-2')
        assert not ctl('configerrors').strip(), ctl('configerrors')
        assert shell.poll() is None
        state = tomllib.loads((base/'state/noctalia/settings.toml').read_text())
        assert state['theme']['source'] == 'community' and state['theme']['community_palette'] == 'macOS'
        assert not (base/'state/noctalia/community-palettes/macOS.json').exists(), 'Bundled palette was downloaded'
        log = (out/'noctalia.log').read_text()
        assert "fetching community palette 'macOS'" not in log
        assert 'downloaded but failed to parse' not in log
        (out/'layers-final.json').write_text(ctl('-j', 'layers'))
        print('PASS: Cupertino surfaces, original palette, offline macOS light/dark, notification history, theme controls and fractional output', flush=True)
    finally:
        for process in (pointer, keyboard):
            process.terminate(); process.wait(timeout=5)
