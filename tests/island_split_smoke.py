"""Split Island promotion, lifetime, input and artwork checks in a private desktop."""
import json
import os
import pathlib
import re
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageStat

REPO = pathlib.Path(__file__).resolve().parents[1]
TESSDATA = os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
                    .replace('[shell]\n', '[shell]\noffline_mode=true\n') + '''
[bar.live]
presentation="island"
reserve_space=false
[bar.live.island]
height=64
clock_size=24
clock_seconds=false
hover_widgets=[]
media_gradient=true
track_preview_seconds=0
split_activities=true
activity_priority="media-downloads-timers"
hover_open_delay_ms=400
[accessibility]
ui_scale=1.0
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
    publishers = [subprocess.Popen([sys.executable, str(REPO/'tests/fixtures/island_downloads.py')],
                                  env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) for _ in range(2)]
    baseline = (cfg/'config.toml').read_text()

    def send(proc, command):
        proc.stdin.write(command+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        send(pointer, f'move {round(x)} {round(y)}'); time.sleep(.1)

    def click(x, y):
        move(x, y); send(pointer, 'press'); send(pointer, 'release'); time.sleep(.6)

    def download(index=0, visible=True, progress=.2):
        send(publishers[index], json.dumps({'uri':'application://split-test.desktop',
                                         'properties':{'progress':progress, 'progress-visible':visible}}))
        time.sleep(.15)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def text(name, output='TEST-1', crop=None):
        im = shot(name, output)
        if output == 'TEST-2': im = im.resize((1280,720))
        im = im.crop(crop or (380,0,1020,350))
        path = out/(name+'-ocr.png'); im.resize((im.width*4,im.height*4)).save(path)
        return run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','6']).strip()

    def close():
        move(1100,600); time.sleep(.9)

    def expand(output='TEST-1'):
        move(640,40 if output == 'TEST-1' else 760); time.sleep(1)
        move(800,180 if output == 'TEST-1' else 900)

    def expect_media(name, output='TEST-1'):
        result = text(name, output)
        assert 'short' in result.lower(), (name,result)

    def percentage(name):
        im = shot(name).crop((695,25,761,55))
        path = out/(name+'-percent.png'); im.resize((im.width*6,im.height*6)).save(path)
        result = run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','7',
                      '-c','tessedit_char_whitelist=0123456789%'])
        match = re.search(r'(20|80)\s*%',result)
        assert match, (name,result)
        return int(match[1])

    def motion_handoff():
        (cfg/'config.toml').write_text(baseline+'\n[shell.animation]\nenabled=true\nspeed=0.25\n')
        msg('config-reload'); time.sleep(.7)
        download(1,False,.8); download(0,True,.2)
        msg('caffeine-for','15'); time.sleep(2.2)
        before = shot('bubble-fade-before')
        began = time.monotonic()
        send(publishers[0],json.dumps({'uri':'application://split-test.desktop',
                                    'properties':{'progress-visible':False}}))
        frames = []
        for index, offset in enumerate((.03,.12,.24,.36,.5,.75,1.1)):
            time.sleep(max(0,began+offset-time.monotonic()))
            frames.append(shot('bubble-fade-'+str(index)))
        pixels = [list(im.crop((810,28,834,52)).getdata()) for im in [before,*frames]]
        old,new = pixels[0],pixels[-1]
        old_mask = [i for i,(r,g,b) in enumerate(old) if b>140 and b>r+70 and b>g+20]
        new_mask = [i for i,(r,g,b) in enumerate(new) if r>150 and r>b+90 and g>75]
        assert len(old_mask)>10 and len(new_mask)>10, ('Missing bubble symbols',len(old_mask),len(new_mask))

        def fraction(sample, mask, foreground, background, channel):
            mean = lambda data: sum(data[i][channel] for i in mask)/len(mask)
            return (mean(sample)-mean(background))/(mean(foreground)-mean(background))

        old_values = [fraction(p,old_mask,old,new,2) for p in pixels[1:]]
        new_values = [fraction(p,new_mask,new,old,0) for p in pixels[1:]]
        assert any(.15<v<.85 for v in old_values), ('Outgoing bubble snapped',old_values)
        assert any(.15<v<.85 for v in new_values), ('Incoming bubble snapped',new_values)
        assert old_values[-2]<.15 and new_values[-2]>.85, ('Bubble fade did not settle',old_values,new_values)
        (out/'bubble-fade.json').write_text(json.dumps(dict(outgoing=old_values,incoming=new_values),indent=2))
        # Sample a blank strip of the capsule throughout the handoff.
        strips = [im.crop((600,14,680,20)) for im in frames]
        assert all(sum(max(p)>12 for p in im.getdata())>250 for im in strips), 'Artwork flashed black'
        assert any(ImageChops.difference(strips[0],im).getbbox() for im in strips[1:]), 'Artwork froze'
        # Rapid replacements during an unfinished fade must leave usable controls.
        for _ in range(3):
            download(0,True,.2); download(0,False,.2)
        time.sleep(1); msg('caffeine-disable'); time.sleep(1)
        assert shell.poll() is None
        print('PASS: sampled incoming/outgoing bubble fade, continuous artwork, rapid replacements and teardown',flush=True)

    try:
        ctl('dismissnotify'); move(1100,600)
        msg('theme-mode-set','dark')
        env['ISLAND_TEST_ART'] = (REPO/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'A Short Song'
        env['ISLAND_TEST_EVENTS'] = str(out/'split-player-actions.log')
        player = start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'split-player.log')
        if '--island-split-motion-only' in sys.argv:
            time.sleep(1); motion_handoff(); return
        time.sleep(1); download()
        shot('media-with-download-bubble')
        expand(); expect_media('expand-follows-media')
        # Select Downloads in the expanded card, then confirm collapse retains it.
        click(724,30); close()
        assert percentage('tab-selection-survives-collapse') == 20
        expand()
        result = text('expand-follows-downloads')
        assert 'split-test' in result, result
        close(); click(832,40)  # Media bubble promotes the playing track.
        close(); expand(); expect_media('bubble-promotion-survives-expansion')
        close(); move(640,40); msg('island-focus'); time.sleep(.7)
        expect_media('keyboard-expansion-follows-selection')
        send(keyboard,'1'); close(); expand()
        # Flow continues even when the download card owns the foreground.
        click(724,30); move(800,200)
        a = shot('download-artwork-a').crop((463,65,490,120))
        time.sleep(.5)
        b = shot('download-artwork-b').crop((463,65,490,120))
        assert sum(ImageStat.Stat(ImageChops.difference(a,b)).mean) > .1, 'Artwork stopped behind downloads'
        close(); click(832,40); close()
        # The promoted media survives an urgent interruption.
        note = run(['notify-send','-p','-u','critical','-t','0','Split alert','Keep current activity']).strip()
        time.sleep(.6); shot('critical-interruption')
        run(['gdbus','call','--session','--dest','org.freedesktop.Notifications','--object-path',
             '/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.CloseNotification',note])
        time.sleep(.6); expand(); expect_media('after-critical-alert')
        close()
        # An ended bubble must not redirect a held click to its replacement.
        msg('caffeine-for','15'); time.sleep(1)
        move(822,40); send(pointer,'press')
        download(visible=False); time.sleep(.7); send(pointer,'release')
        close(); expand(); expect_media('ended-bubble-does-not-steal-click')
        close(); click(822,40); close(); expand()
        result = text('replacement-bubble-remains-clickable')
        assert 'Keep Awake' in result, result
        msg('caffeine-disable'); time.sleep(.8)
        expect_media('ended-selection-falls-back-to-media')
        close()
        # A second output has its own choice and scaled pointer geometry.
        download()
        move(822,760); send(pointer,'press'); send(pointer,'release'); time.sleep(.7)
        move(1100,1320); time.sleep(.7); expand('TEST-2')
        result = text('scaled-bubble-selection','TEST-2')
        assert 'split-test' in result, result
        move(1100,1320); time.sleep(.7); expand(); expect_media('selection-is-independent-per-output')
        close()
        # Two publishers using one app ID must swap by job identity.
        player.terminate(); player.wait(timeout=5)
        download(0,True,.2); download(1,True,.8); time.sleep(.8)
        first = percentage('same-app-first')
        click(832,40); close()
        second = percentage('same-app-second')
        assert first != second, (first,second)
        click(832,40); close()
        assert percentage('same-app-swap-back') == first
        # Ending the selected job leaves the other job, with no completion notice.
        selected = 0 if first == 20 else 1
        download(selected,False,.2 if selected == 0 else .8)
        time.sleep(.7)
        assert percentage('selected-job-ended') == second
        # Reduced motion uses the same actions with immediate presentation.
        (cfg/'config.toml').write_text(baseline+'\n[shell.animation]\nenabled=false\n')
        msg('config-reload'); time.sleep(.8)
        download(0,True,.2); download(1,True,.8); time.sleep(.5)
        first = percentage('reduced-first'); click(832,40); close()
        assert percentage('reduced-swapped') != first
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: compact/expanded selection, keyboard expansion, tab and bubble promotion, artwork continuity, '
              'urgent interruption, held click cancellation, end fallback, scaled independent output, '
              'distinct jobs from one app and reduced motion',flush=True)
    finally:
        for proc in [pointer,keyboard,*publishers]:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
