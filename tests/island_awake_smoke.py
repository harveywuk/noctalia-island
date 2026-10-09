"""Timed Keep Awake Live Activity, using the real inhibitor and its expiry timer."""
import csv
import io
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops

REPO = pathlib.Path(__file__).resolve().parents[1]
TESSDATA = os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
                    .replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    .replace('[osd.kinds]\n', '[osd.kinds]\nkeyboard_layout=false\n')+'''
[bar.live]
presentation="island"
reserve_space=false
[bar.live.island]
height=64
clock_size=24
hover_widgets=[]
media_gradient=true
track_preview_seconds=0
[accessibility]
ui_scale=1.1
''')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    for kind, proto, libs in (
        ('pointer', REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', REPO/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(proto), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(proto), str(base/(kind+'-code.c'))])
        source = base/(kind+'.c')
        source.write_text((REPO/f'tests/fixtures/island_{kind}.c').read_text().replace('x,y,1280,720', 'x,y,1280,1440'))
        run(['cc', '-I'+str(base), str(source), str(base/(kind+'-code.c')), *libs, '-o', str(base/kind)])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def send(proc, command):
        proc.stdin.write(command+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        send(pointer, f'move {round(x)} {round(y)}'); time.sleep(.15)

    def click(x, y):
        move(x, y); send(pointer, 'press'); send(pointer, 'release'); time.sleep(.65)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def words(name, output='TEST-1'):
        im = shot(name, output); factor = im.width/1280
        # Raw colours retain muted text and tab labels over the flowing artwork.
        zoom = out/(name+'-text.png')
        im.resize((im.width*3, im.height*3)).save(zoom)
        result = run(['tesseract', str(zoom), 'stdout', '--tessdata-dir', TESSDATA, '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        rows = csv.DictReader(io.StringIO(result[result.index('level\t'):]), delimiter='\t')
        return [(r['text'].strip(), (int(r['left'])+int(r['width'])/2)/(3*factor),
                 (int(r['top'])+int(r['height'])/2)/(3*factor)) for r in rows if (r.get('text') or '').strip()]

    def text(name, output='TEST-1'):
        return ' '.join(w for w,x,y in words(name, output))

    def status():
        return json.loads(msg('caffeine-status'))

    def open_awake(output='TEST-1'):
        move(1100,600 if output=='TEST-1' else 1320); time.sleep(.8)
        move(620,40 if output=='TEST-1' else 760); time.sleep(.9)
        move(818,110 if output=='TEST-1' else 830)

    def close():
        msg('panel-close'); move(1100,600); time.sleep(.8)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark'); time.sleep(1)
        assert status() == {'enabled':False, 'remaining_seconds':None}
        msg('caffeine-for','1'); time.sleep(.8)
        content = text('awake-compact')
        assert 'Keep' in content and 'Awake' in content, content
        first = shot('awake-first'); time.sleep(2); second = shot('awake-tick')
        assert ImageChops.difference(first.crop((570,30,710,74)), second.crop((570,30,710,74))).getbbox(), 'Countdown did not tick'
        open_awake(); content = text('awake-expanded')
        assert all(s in content for s in ('Keep','Awake','remaining','End')), content
        before = status()['remaining_seconds']
        # A press held across countdown ticks must still activate the same button on release.
        move(551,181); send(pointer,'press'); time.sleep(1.3); send(pointer,'release'); time.sleep(.6)
        after = status()['remaining_seconds']
        assert 896 <= after-before <= 900, (before,after)
        assert 'remaining' in text('awake-extended'), 'Extension replaced the activity with an OSD'
        click(729,181); assert not status()['enabled']
        assert 'remaining' not in text('awake-ended')

        # Starting in Control Centre feeds the same activity and shared deadline.
        msg('panel-open','control-center','power'); time.sleep(.8)
        choices = [(x,y) for w,x,y in words('awake-power-start') if w=='15']
        assert choices, '15 minute Control Centre button missing'
        click(*choices[0]); assert 890 <= status()['remaining_seconds'] <= 900
        close(); open_awake(); assert 'remaining' in text('awake-from-power')
        click(798,43); assert 'Keep' in text('awake-power-link')
        assert json.loads(msg('status'))['panelOpen'], 'Power settings link failed'
        close()
        msg('caffeine-enable'); time.sleep(2)
        assert status()['enabled'] and status()['remaining_seconds'] is None
        assert 'remaining' not in text('awake-indefinite')
        open_awake(); assert 'End' not in text('awake-indefinite-hover')
        move(1100,600); time.sleep(.8)

        # Normal and fractional outputs share the actual timer, including the add-time button.
        msg('caffeine-for','30'); open_awake('TEST-2')
        assert 'remaining' in text('awake-scaled','TEST-2')
        before = status()['remaining_seconds']; click(551,901)
        assert 897 <= status()['remaining_seconds']-before <= 900
        assert 'remaining' in text('awake-scaled-extended','TEST-2')
        move(1100,1320); time.sleep(.8); move(620,40); time.sleep(.8); move(1100,600); time.sleep(.8)
        msg('island-focus'); time.sleep(.8); assert 'remaining' in text('awake-keyboard')
        send(keyboard,'1'); time.sleep(.8); assert 'End' not in text('awake-escaped')

        # Coexist with media in a split bubble, then show live artwork behind expanded controls.
        env['ISLAND_TEST_ART'] = (REPO/'assets/noctalia-wallpaper.png').as_uri()
        player = start([sys.executable,str(REPO/'tests/fixtures/island_player.py')], 'player.log'); time.sleep(1.5)
        bubble = shot('awake-bubble')
        orange = [(x,y) for x in range(770,980) for y in range(20,75)
                  if (lambda p: p[0]>200 and 115<p[1]<200 and p[2]<65)(bubble.getpixel((x,y)))]
        assert orange, 'Keep Awake split bubble missing beside media'
        click(sum(x for x,y in orange)/len(orange),sum(y for x,y in orange)/len(orange))
        move(1100,600); time.sleep(.8)
        assert 'Keep' in text('awake-promoted')
        open_awake(); click(728,35)  # Awake activity tab beside Media.
        assert 'remaining' in text('awake-media')
        a=shot('awake-art-before'); time.sleep(.4); b=shot('awake-art-after')
        strip=(580,14,690,20)
        assert sum(max(p)>12 for p in b.crop(strip).getdata())>300, 'Activity covered artwork'
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Activity froze artwork'
        run(['notify-send','-u','critical','Urgent alert','Keep Awake continues underneath'])
        time.sleep(.7); assert 'Urgent' in text('awake-urgent')
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.8)
        assert 'remaining' in text('awake-restored')

        # Let a real one-minute session expire, including reload and reduced-motion transitions.
        msg('caffeine-for','1'); deadline=time.monotonic()+60
        config=cfg/'config.toml'; original=config.read_text()
        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload')
        time.sleep(.8); open_awake(); click(728,35)
        assert 'remaining' in text('awake-reduced-motion')
        assert 50 <= status()['remaining_seconds'] <= 60, 'Reload reset the deadline'
        config.write_text(original); msg('config-reload'); time.sleep(.8); open_awake(); click(728,35)
        # Pulse through short waits so a failed shell or early expiry is caught promptly.
        while time.monotonic() < deadline-2:
            assert shell.poll() is None and status()['enabled']
            time.sleep(min(3,max(0,deadline-2-time.monotonic())))
        wait(lambda:not status()['enabled'], 'Keep Awake did not expire')
        time.sleep(.8)
        content=text('awake-expired')
        assert 'remaining' not in content and 'Keep Awake' not in content, content
        assert status()['remaining_seconds'] is None
        assert shell.poll() is None
        print('PASS: Keep Awake countdown, held-click extension, End, Control Centre sync, indefinite mode, scaled output, keyboard, split bubble, artwork, alerts, reload and real expiry', flush=True)
    finally:
        pointer.terminate(); pointer.wait(timeout=5)
        keyboard.terminate(); keyboard.wait(timeout=5)
