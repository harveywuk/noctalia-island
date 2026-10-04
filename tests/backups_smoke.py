"""Backup restore, undo and display confirmation inside a private compositor."""
import csv
import io
import json
import os
import pathlib
import subprocess
import time
import tomllib


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    def command(text):
        pointer.stdin.write(text+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'
    def move(x,y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(.12)
    def click(x,y):
        move(x,y); command('press'); time.sleep(.12); command('release'); time.sleep(.5)
    def screenshot(name='current'):
        path = out/(name+'.png'); run(['grim','-o','TEST-1',str(path)]); return path
    def words(variant='plain'):
        # OCR locates actual labels; coordinates vary with translated text, fonts,
        # status banners and the length of the review, so fixed clicks are brittle.
        # It reads a 2x upscale, and an inverted one for light labels on grey buttons.
        from PIL import Image, ImageOps
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        image = Image.open(screenshot()).convert('L')
        image = image.resize((image.width*2, image.height*2), Image.LANCZOS)
        if variant == 'inverted':
            image = ImageOps.autocontrast(ImageOps.invert(image))
        image.save(out/'ocr.png')
        result = run(['tesseract',str(out/'ocr.png'),'stdout','--tessdata-dir',tessdata,'--psm','11',
                      '-c','tessedit_create_tsv=1','-c','user_defined_dpi=192'])
        rows = [r for r in csv.DictReader(io.StringIO(result), delimiter='\t', quoting=csv.QUOTE_NONE) if r.get('text','').strip()]
        for r in rows:
            for key in ('left', 'top', 'width', 'height'):
                r[key] = str(int(r[key])//2)
        return rows
    def locate(text, starts_line=False):
        target = [w.strip('<>‹›‘’“”"\'|.,:;') for w in text.lower().split()]
        for variant in ('plain', 'inverted'):
            rows = words(variant)
            for i in range(len(rows)-len(target)+1):
                if starts_line and rows[i]['word_num'] != '1': continue
                # OCR glues a link's arrow to its word ("<Back") and adds stray marks at edges.
                if [r['text'].lower().strip('<>‹›‘’“”"\'|.,:;') for r in rows[i:i+len(target)]] == target:
                    selected = rows[i:i+len(target)]
                    left = min(int(r['left']) for r in selected); top = min(int(r['top']) for r in selected)
                    right = max(int(r['left'])+int(r['width']) for r in selected)
                    bottom = max(int(r['top'])+int(r['height']) for r in selected)
                    return ((left+right)//2, (top+bottom)//2)
        return None
    def switch_at(row_y):
        # The switch at the right end of a settings row: the white knob's pixels beside the row.
        from PIL import Image
        image = Image.open(screenshot('switch')).convert('L')
        knob = [(x, y) for y in range(row_y-8, row_y+14) for x in range(1000, 1200) if image.getpixel((x, y)) > 225]
        assert knob, 'No switch on the row at y=%d' % row_y
        return sum(p[0] for p in knob)//len(knob), sum(p[1] for p in knob)//len(knob)
    def scroll(steps):
        move(1000,590); command(f'scroll {steps}'); time.sleep(.4)
    def click_text(text, direction=1, starts_line=False):
        for _ in range(16):
            point = locate(text, starts_line)
            if point and point[1] > 650:
                scroll(2); continue
            if point: click(*point); return
            scroll(2*direction)
        raise AssertionError('UI text not found: '+text)
    def bottom():
        for _ in range(12): scroll(30)
    def top():
        for _ in range(12): scroll(-30)
    settings = base/'state/noctalia/settings.toml'
    def saved(): return tomllib.loads(settings.read_text()) if settings.exists() else {}
    def backups(): return sorted((base/'state/noctalia/backups').glob('*.toml'))
    def monitors(): return {m['name']:m for m in json.loads(ctl('-j','monitors'))}
    original_config = (cfg/'config.toml').read_bytes()
    try:
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('settings-open','system'); time.sleep(.8)
        click_text('Backups')
        click_text('Save backup')
        assert len(backups()) == 1
        snapshot = tomllib.loads(backups()[0].read_text())
        assert snapshot['name'] == 'My setup' and not snapshot['automatic']
        # Change an appearance option and an unrelated input option, then restore
        # only appearance. Editing the private sidecar exercises real hot reload.
        settings.write_text(settings.read_text() + '\n[shell]\npopup_shadows=false\n[shell.hyprland_input]\npointer_sensitivity=0.4\n')
        msg('config-reload'); time.sleep(.6)
        bottom()
        point = locate('Input & motion'); assert point
        click(*switch_at(point[1]))
        click_text('Preview restore')
        screenshot('backup-review')
        click_text('Restore selected sections', direction=-1, starts_line=True)
        # The restore writes settings asynchronously; wait for it rather than reading at once.
        wait(lambda: 'popup_shadows' not in saved().get('shell',{}), 'scoped restore written')
        assert abs(saved()['shell']['hyprland_input']['pointer_sensitivity']-.4)<.001
        assert len(backups()) == 2 and tomllib.loads(backups()[-1].read_text())['automatic']
        click_text('Preview undo',direction=-1)
        click_text('Restore selected sections', direction=-1, starts_line=True)
        wait(lambda: saved().get('shell',{}).get('popup_shadows') is False, 'undo restore written')
        assert abs(saved()['shell']['hyprland_input']['pointer_sensitivity']-.4)<.001
        # Start a fresh editor and select the original named backup. Automatic
        # backups remain available across Settings closes.
        msg('settings-close'); msg('settings-open','system'); time.sleep(.6)
        click_text('Backups')
        click_text('Choose a backup')
        # Undo names contain the original name too. Match the row's first words.
        click_text('My setup', starts_line=True)
        # A managed rotation must preview restoring the original unmanaged output.
        settings.write_text(settings.read_text()+'\n[shell.hyprland_displays.TEST-2]\nmanaged=true\ntransform=1\n')
        msg('config-reload'); wait(lambda:monitors()['TEST-2']['transform']==1,'private display override')
        bottom(); click_text('Preview restore')
        click_text('Test displays before restoring',direction=-1)
        wait(lambda:monitors()['TEST-2']['transform']==2,'display restore preview')
        assert saved()['shell']['popup_shadows'] is False, 'appearance committed before display confirmation'
        screenshot('backup-display-confirmation')
        time.sleep(16)
        wait(lambda:monitors()['TEST-2']['transform']==1,'expired display test reverted')
        assert saved()['shell']['hyprland_displays']['TEST-2']['managed']
        assert saved()['shell']['popup_shadows'] is False
        click_text('Test displays before restoring',direction=-1)
        click_text('Keep and restore',direction=-1)
        wait(lambda:monitors()['TEST-2']['transform']==2,'confirmed display restore')
        assert 'hyprland_displays' not in saved().get('shell',{})
        assert 'popup_shadows' not in saved().get('shell',{})
        assert len(backups()) == 4
        assert (cfg/'config.toml').read_bytes()==original_config
        screenshot('backup-restored')
        (out/'inspect-env.json').write_text(json.dumps(env))
        if os.environ.get('NOCTALIA_TEST_BACKUP_INSPECT'):
            print('INSPECT: backup checks finished',flush=True)
            deadline=time.monotonic()+int(os.environ['NOCTALIA_TEST_BACKUP_INSPECT'])
            while time.monotonic()<deadline and not (out/'inspect-done').exists():time.sleep(.25)
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: named backup, scoped restore, automatic undo, persistent picker, display timeout and confirmed restore; base config untouched',flush=True)
    except Exception:
        screenshot('backup-failure')
        if settings.exists(): (out/'failed-private-settings.toml').write_text(settings.read_text())
        raise
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
