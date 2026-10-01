"""Focused native Settings pages, search and keyboard navigation."""
import csv
import io
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
        move(x,y); command('press'); time.sleep(.12); command('release'); time.sleep(.9)
    def screenshot(name='current'):
        path = out/(name+'.png'); run(['grim','-o','TEST-1',str(path)]); return path
    def words():
        # OCR locates actual labels; coordinates vary with translated text, fonts,
        # status banners and the length of the review, so fixed clicks are brittle.
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        result = run(['tesseract',str(screenshot()),'stdout','--tessdata-dir',tessdata,'--psm','11',
                      '-c','tessedit_create_tsv=1','-c','user_defined_dpi=96'])
        (out/'ocr-last.tsv').write_text(result)
        return [r for r in csv.DictReader(io.StringIO(result), delimiter='\t', quoting=csv.QUOTE_NONE) if r.get('text','').strip()]
    def locate(text, starts_line=False):
        target = text.lower().split()
        rows = words()
        for i in range(len(rows)-len(target)+1):
            if starts_line and rows[i]['word_num'] != '1': continue
            if [r['text'].lower() for r in rows[i:i+len(target)]] == target:
                selected = rows[i:i+len(target)]
                left = min(int(r['left']) for r in selected); top = min(int(r['top']) for r in selected)
                right = max(int(r['left'])+int(r['width']) for r in selected)
                bottom = max(int(r['top'])+int(r['height']) for r in selected)
                return ((left+right)//2, (top+bottom)//2)
        return None
    def scroll(steps):
        move(1000,590); command(f'scroll {steps}'); time.sleep(.4)
    def click_text(text, direction=1, starts_line=False):
        for _ in range(16):
            point = locate(text, starts_line)
            if point and point[1] > 650:
                if point[0] < 300:
                    move(180,590);command('scroll 2');time.sleep(.4)
                else: scroll(2)
                continue
            if point: click(*point); return
            if text.startswith('Monitor'):
                move(180,590);command(f'scroll {2*direction}');time.sleep(.4)
            else: scroll(2*direction)
        raise AssertionError('UI text not found: '+text)
    def bottom():
        for _ in range(12): scroll(30)
    def top():
        for _ in range(12): scroll(-30)

    proto=repo/'protocols/virtual-keyboard-unstable-v1.xml'
    run(['wayland-scanner','client-header',str(proto),str(base/'keyboard-client.h')])
    run(['wayland-scanner','private-code',str(proto),str(base/'keyboard-code.c')])
    run(['cc','-I'+str(base),str(repo/'tests/fixtures/island_keyboard.c'),str(base/'keyboard-code.c'),'-lwayland-client','-lxkbcommon','-o',str(base/'keyboard')])
    keyboard=subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def key(value):
        keyboard.stdin.write(str(value)+'\n');keyboard.stdin.flush()
        assert keyboard.stdout.readline().strip()=='ok'
        time.sleep(.15)
    def open_page(section, group):
        msg('settings-close');msg('settings-open',section);time.sleep(1.2)
        click_text(group)
        assert locate('Back'), 'Missing Back after opening '+section+'/'+group
    original=(cfg/'config.toml').read_bytes()
    try:
        dispatch('hl.dsp.focus({monitor="TEST-1"})');move(1100,600)
        msg('settings-open','system');time.sleep(.8)
        screenshot('settings-system-overview')
        assert locate('Default applications') and locate('Backups')
        click_text('Default applications')
        assert locate('Web browser') and not locate('Screen Time')
        screenshot('settings-default-apps')
        key('chord 4 105') # Alt+Left
        assert locate('Backups') and not locate('Web browser')
        click_text('Backups');assert locate('Save backup')
        click_text('Back');assert locate('Default applications')
        open_page('island','Activities')
        screenshot('settings-island-activities')
        click_text('Back');wait(lambda:locate('General'), 'Island overview after Back')
        open_page('input-motion','Touchpad')
        screenshot('settings-touchpad')
        point=locate('Overridden');assert point
        click(1224,point[1])
        assert locate('No settings found') and locate('Back')
        click(1224,point[1])
        # Ctrl+F finds controls across pages, and clearing search restores the page.
        key('chord 2 33')
        for k in [48,30,46,37,22,25,31]: key(k) # backups
        time.sleep(.5)
        assert locate('Save backup')
        screenshot('settings-search')
        key('chord 2 30');key(14);time.sleep(.4)
        assert locate('Back') and not locate('Save backup')
        open_page('plugins','Sources')
        screenshot('settings-plugin-sources')
        click_text('Back')
        open_page('bar','General')
        screenshot('settings-bar')
        settings=base/'state/noctalia/settings.toml'
        settings.write_text('config_version=14\n[bar.default]\nreserve_space=false\nwidget_spacing=17\n')
        msg('config-reload');time.sleep(1.5)
        open_page('bar','General')
        screenshot('settings-reset-ready')
        reset=locate('Reset Page');assert reset
        click(*reset)
        assert tomllib.loads(settings.read_text())['bar']['default']['reserve_space'] is False
        screenshot('settings-reset-confirm')
        # The destructive button's colour can hide its black text from OCR;
        # it replaces the same anchored header button after confirmation starts.
        click(*reset)
        saved=tomllib.loads(settings.read_text())
        assert 'reserve_space' not in saved['bar']['default']
        assert saved['bar']['default']['widget_spacing']==17
        settings.write_text('');msg('config-reload')
        (cfg/'config.toml').write_bytes(original+b'\n[bar.default.monitor.TEST-2]\nscale=1.25\n')
        msg('config-reload');time.sleep(1.5)
        msg('settings-close');msg('settings-open','bar');time.sleep(.6)
        click_text('Monitor: TEST-2');click_text('General')
        assert locate('Inherited')
        screenshot('settings-monitor-override')
        click_text('Monitor Override');key(1)
        point=locate('Overridden');assert point
        click(1224,point[1]);assert locate('Back')
        click(1224,point[1])
        (cfg/'config.toml').write_bytes(original);msg('config-reload')
        msg('settings-close');msg('settings-open','displays');time.sleep(.8)
        assert locate('Identify displays')
        screenshot('settings-displays')
        assert (cfg/'config.toml').read_bytes()==original
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: category overview, focused editors, Back and Alt+Left, global search and return, plugins, scoped reset, monitor inheritance, dialog cancellation and display editor',flush=True)
    except Exception:
        screenshot('settings-layout-failure')
        if (base/'state/noctalia/settings.toml').exists():
            (out/'failed-settings.toml').write_text((base/'state/noctalia/settings.toml').read_text())
        raise
    finally:
        pointer.terminate();pointer.wait(timeout=5)
        keyboard.terminate();keyboard.wait(timeout=5)
