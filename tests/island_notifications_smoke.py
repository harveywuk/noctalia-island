"""Notification stacks in the Island and OSD isolation on private Wayland outputs."""
import csv
import io
import json
import os
import pathlib
import re
import subprocess
import time

from PIL import Image


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace('hover_widgets=["workspaces","taskbar"]',
                                    'hover_widgets=[]\nmedia_gradient=false')
    text = text.replace('[shell]\n', '[shell]\noffline_mode=true\n')
    text += '\n[shell.animation]\nenabled=false\n'
    path.write_text(text)


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

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0'); time.sleep(.15)

    def click(x, y):
        move(x, y); command(pointer, 'press'); time.sleep(.08); command(pointer, 'release'); time.sleep(.5)

    def key(code):
        command(keyboard, code); time.sleep(.25)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)]); return path

    def words(name, geometry=None, psm='11'):
        path = out/(name+'.png')
        if geometry:
            run(['grim', '-g', geometry, str(path)])
        else:
            shot(name)
        enlarged = out/(name+'-ocr.png')
        frame = Image.open(path)
        frame.resize((frame.width*2, frame.height*2), Image.LANCZOS).save(enlarged)
        data = run(['tesseract', str(enlarged), 'stdout', '--tessdata-dir',
                    os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                    '--psm', psm, '-c', 'tessedit_create_tsv=1', '-c', 'user_defined_dpi=96'])
        rows = [w for w in csv.DictReader(io.StringIO(data), delimiter='\t', quoting=csv.QUOTE_NONE)
                if (w.get('text') or '').strip()]
        for row in rows:
            for key in ('left', 'top', 'width', 'height'):
                row[key] = str(int(row[key])//2)
        return rows

    def texts(name):
        return ' '.join(w['text'] for w in words(name))

    def click_word(label, name):
        matches = [w for w in words(name) if w['text'].lower() == label.lower()]
        assert matches, (label, name)
        word = matches[0]
        click(int(word['left'])+int(word['width'])//2, int(word['top'])+int(word['height'])//2)

    def notify(app, summary, actions='[]', timeout=0):
        reply = run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
                     '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
                     app, '0', '', summary, 'Ready when you are.', actions, "{'urgency': <byte 2>}", str(timeout)])
        return int(re.search(r'uint32 (\d+)', reply).group(1))

    def open_history():
        move(); msg('panel-open', 'control-center', 'notifications'); time.sleep(.6)

    def check_hosted(name):
        title = [w for w in words(name) if w['text'] == 'Notifications']
        assert title and 400 < int(title[0]['left']) < 700 and int(title[0]['top']) < 90, title
        layers = json.loads(ctl('-j', 'layers'))['TEST-1']['levels']
        assert not any('panel' in entry.get('namespace', '')
                       for level in layers.values() for entry in level), layers

    def osd_bottom(name):
        image = Image.open(shot(name)).convert('RGB')
        # The capsule is solid black; capture glow is outside its edge.
        ys = [y for y in range(8, 160) if max(image.getpixel((640, y))) < 12]
        assert ys, name
        return max(ys)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        msg('volume-osd', '65'); time.sleep(.3)
        baseline = osd_bottom('volume-clean')
        time.sleep(1.7)
        notify('Mail', 'Quiet delivery')
        msg('notification-clear-active'); time.sleep(.4)
        msg('volume-osd', '65'); time.sleep(.3)
        assert osd_bottom('volume-unread') == baseline, 'Unread history expanded the volume OSD'
        time.sleep(1.7)
        run(['pactl', 'load-module', 'module-remap-source', 'source_name=notifications-mic',
             'master=hyprland-test.monitor'])
        capture = subprocess.Popen(['parec', '--device=notifications-mic', '--client-name=History privacy test',
                                    '--stream-name=History capture'], env=env, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        helpers.append(capture); time.sleep(2.5)
        shot('privacy-and-unread')
        msg('volume-osd', '65'); time.sleep(.3)
        assert osd_bottom('volume-privacy-unread') == baseline, 'Privacy controls expanded the volume OSD'
        time.sleep(1.7); shot('privacy-restored')
        capture.terminate(); capture.wait(timeout=5); time.sleep(2)
        msg('notification-clear-history')

        for title in ('First delivery', 'Second delivery', 'Third delivery'):
            notify('Mail', title)
        notify('Calendar', 'Design review')
        open_history(); check_hosted('grouped-history')
        text = texts('grouped-text')
        assert 'Third' in text and 'First' not in text and 'Second' not in text, text
        click_word('Third', 'expand-stack')
        text = texts('expanded-history')
        assert 'Second' in text and 'Third' in text, text
        # The expanded stack scrolls at desktop card spacing on a 720px output.
        move(760, 480); command(pointer, 'scroll 4'); time.sleep(.4)
        assert 'First' in texts('expanded-history-bottom'), 'Oldest notification is not reachable'
        command(pointer, 'scroll -20'); time.sleep(.4)
        click_word('Less', 'collapse-stack')
        assert 'First' not in texts('collapsed-history')
        key(1); move(); time.sleep(.4)
        # The four persistent critical previews must not replay after reading history.
        assert 'delivery' not in texts('return-to-clock')

        # A new unread bell opens the same hosted history, directly from the capsule.
        notify('Notes', 'Saved draft'); msg('notification-clear-active'); time.sleep(.4)
        shot('unread-bell'); click(698, 40); check_hosted('unread-bell-opens-history')
        notify('Calendar', 'Arrived while reading'); time.sleep(.3)
        assert 'Arrived' in texts('arrived-in-history')
        key(1); move(); time.sleep(.4)
        assert 'Arrived' not in texts('arrival-does-not-replay')

        # Visible dismiss targets and stack expansion also work by keyboard.
        msg('notification-clear-history')
        notify('Mail', 'Older message'); notify('Mail', 'Latest message')
        open_history(); key(15); key(28)
        assert 'Older' in texts('keyboard-expanded-stack')
        time_word = next(w for w in words('individual-dismiss') if w['text'] == 'now')
        click(int(time_word['left'])+40, int(time_word['top'])+int(time_word['height'])//2)
        text = texts('individual-dismissed')
        assert 'Latest' not in text and 'Older' in text, text
        notify('Calendar', 'First reminder'); notify('Calendar', 'Second reminder'); time.sleep(.3)
        time_word = next(w for w in words('group-dismiss') if w['text'] == 'now')
        click(int(time_word['left'])+40, int(time_word['top'])+int(time_word['height'])//2)
        text = texts('group-dismissed')
        assert 'reminder' not in text and 'Older' in text, text
        # Clear all returns to a compact empty state inside the Island.
        click_word('Clear', 'clear-history')
        assert 'Recent notifications' in texts('empty-history')
        notify('Mail', 'Cleared externally'); time.sleep(.3); msg('notification-clear-history'); time.sleep(.3)
        assert 'Recent notifications' in texts('ipc-cleared-history')
        key(1); move()

        # An accepted action returns to the Island and signals the originating app.
        monitor = start(['gdbus', 'monitor', '--session', '--dest', 'org.freedesktop.Notifications'],
                        'notification-signals.log')
        time.sleep(.2)
        note = notify('Calendar', 'Choose a reminder', "['default', 'Open', 'later', 'Later']")
        open_history(); click_word('Later', 'quick-action')
        wait(lambda: 'ActionInvoked' in (out/'notification-signals.log').read_text(), 'Action was not invoked')
        signals = (out/'notification-signals.log').read_text()
        assert str(note) in signals and "'later'" in signals, signals
        move(); assert 'Choose' not in texts('action-return')
        before = signals.count('ActionInvoked')
        notify('Calendar', 'Expired reminder', "['default', 'Open', 'later', 'Later']", timeout=100)
        open_history(); time.sleep(.3); click_word('Later', 'expired-quick-action')
        wait(lambda: (out/'notification-signals.log').read_text().count('ActionInvoked') == before+1,
             'Expired notification action was not invoked')
        monitor.terminate(); monitor.wait(timeout=5)

        # Shell alerts which are not retained in history must still surface after it closes.
        open_history()
        run(['gdbus', 'call', '--session', '--dest', 'dev.noctalia.Debug', '--object-path', '/dev/noctalia/Debug',
             '--method', 'dev.noctalia.Debug.EmitInternalNotification', 'Noctalia', 'Ephemeral message',
             'Not stored in history.', '0', '2'])
        time.sleep(.2); key(1); move(); time.sleep(.3)
        assert 'Ephemeral' in texts('unstored-preview')
        msg('notification-clear-active'); msg('notification-clear-history')

        # Shared motion, interruption, and reduced-motion settling.
        path = cfg/'config.toml'
        path.write_text(path.read_text().replace('[shell.animation]\nenabled=false',
                                                 '[shell.animation]\nenabled=true'))
        msg('config-reload'); time.sleep(.4)
        notify('Mail', 'Motion sample'); open_history(); key(1)
        msg('panel-open', 'notification-center'); time.sleep(.08); msg('panel-close'); time.sleep(.6)
        open_history()
        path.write_text(path.read_text().replace('[shell.animation]\nenabled=true',
                                                 '[shell.animation]\nenabled=false'))
        msg('config-reload'); time.sleep(.5); key(1); move(); time.sleep(.4)
        assert 'Motion' not in texts('interrupted-return')
        msg('theme-mode-set', 'light'); open_history(); shot('light-history'); key(1)
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(1100, 1320)
        msg('panel-open', 'notification-center'); time.sleep(.6); shot('fractional-history', 'TEST-2'); msg('panel-close')
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        path.write_text(path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false'))
        msg('config-reload'); time.sleep(.4); open_history()
        shot('without-island')
        # Isolate the single toolbar row: sparse full-screen OCR can skip its small title.
        toolbar_words = words('without-island-toolbar', geometry='895,8 375x60', psm='6')
        title = [w for w in toolbar_words if w['text'] == 'Notifications']
        assert title, toolbar_words
        clear = next(w for w in toolbar_words if w['text'] == 'Clear')
        assert abs(int(clear['top'])-int(title[0]['top'])) < 12, 'Fallback toolbar wrapped'
        key(1)
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: OSD isolation with unread/privacy, Island history, app stacks, keyboard expansion, '
              'clear, live arrivals, quick actions, return, motion interruption, light/dark and fractional scale', flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
