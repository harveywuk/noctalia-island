"""Default applications and terminal selection in a private compositor."""
import csv
import io
import os
import pathlib
import subprocess
import time
import tomllib


def prepare(base, cfg, env):
    apps = base/'data/applications'
    apps.mkdir(parents=True)
    types = 'x-scheme-handler/http;x-scheme-handler/https;text/html;application/xhtml+xml;'
    for name in ('Alpha', 'Bravo'):
        (apps/(name.lower()+'.desktop')).write_text('[Desktop Entry]\nType=Application\nName='+name+' Browser\nExec=/bin/true %u\nMimeType='+types+'\n')
    (apps/'mimeinfo.cache').write_text('[MIME Cache]\n'+''.join(t+'=alpha.desktop;bravo.desktop;\n' for t in types.split(';') if t))
    (base/'config/mimeapps.list').write_text('# Private test defaults\n[Default Applications]\n'+''.join(t+'=alpha.desktop;\n' for t in types.split(';') if t))
    binaries=base/'bin'; binaries.mkdir()
    terminal=binaries/'ghostty'
    terminal.write_text('#!/bin/sh\ntouch "'+str(base/'terminal-opened')+'"\n')
    terminal.chmod(0o700)
    (apps/'test-terminal.desktop').write_text('[Desktop Entry]\nType=Application\nName=Fixture Terminal\nExec='+str(terminal)+'\nCategories=TerminalEmulator;\n')


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
    def words():
        # OCR locates actual labels; coordinates vary with translated text, fonts,
        # status banners and the length of the review, so fixed clicks are brittle.
        tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
        result = run(['tesseract',str(screenshot()),'stdout','--tessdata-dir',tessdata,'--psm','11',
                      '-c','tessedit_create_tsv=1','-c','user_defined_dpi=96'])
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
                scroll(2); continue
            if point: click(*point); return
            scroll(2*direction)
        raise AssertionError('UI text not found: '+text)
    def bottom():
        for _ in range(12): scroll(30)
    def top():
        for _ in range(12): scroll(-30)
    mime = base/'config/mimeapps.list'
    before = mime.read_bytes()
    settings = base/'state/noctalia/settings.toml'
    def saved(): return tomllib.loads(settings.read_text()) if settings.exists() else {}
    try:
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('settings-open','system'); time.sleep(.8)
        click_text('Default applications')
        screenshot('default-applications')
        click_text('Alpha Browser')
        click_text('Bravo Browser', starts_line=True)
        time.sleep(.6)
        for mime_type in ('x-scheme-handler/http','x-scheme-handler/https','text/html','application/xhtml+xml'):
            assert 'bravo.desktop' in run(['gio','mime',mime_type]).splitlines()[0]
        screenshot('browser-selected')
        msg('settings-close'); msg('settings-open','system'); time.sleep(.6)
        click_text('Default applications')
        click_text('Undo app change')
        assert mime.read_bytes() == before
        click_text('Automatic discovery')
        click_text('Fixture Terminal', starts_line=True)
        time.sleep(.8)
        assert saved()['shell']['preferred_terminal'] == 'test-terminal.desktop'
        click_text('Open terminal')
        wait(lambda:(base/'terminal-opened').exists(), 'selected terminal launched')
        screenshot('terminal-selected')
        msg('settings-close'); msg('settings-open','system'); time.sleep(.6)
        click_text('Default applications')
        click_text('Undo terminal change')
        assert 'preferred_terminal' not in saved().get('shell',{})
        assert mime.read_bytes() == before
        screenshot('defaults-restored')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: browser picker, all web associations, persistent app undo, preferred terminal launch and persistent terminal undo',flush=True)
    except Exception:
        screenshot('default-apps-failure')
        raise
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
