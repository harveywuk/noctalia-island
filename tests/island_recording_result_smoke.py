"""Recording result previews and actions on a private desktop and audio server."""
import csv
import io
import json
import pathlib
import re
import shutil
import subprocess
import sys
import time

from PIL import Image, ImageChops
from island_capture_menu_smoke import prepare as prepare_menu, REPO, TESSDATA


def prepare(base, cfg, env):
    prepare_menu(base, cfg, env)
    fixtures = base/'result-bin'
    fixtures.mkdir()
    (fixtures/'xdg-open').write_text(
        '#!/usr/bin/python3\nimport json,sys\nfrom pathlib import Path\n'
        'Path('+repr(str(base/'opened'))+').write_text(json.dumps(sys.argv[1:]))\n')
    (fixtures/'ffmpeg').write_text(
        '#!/usr/bin/python3\nimport os,sys,time\nfrom pathlib import Path\n'
        'mode=Path('+repr(str(base/'preview-mode'))+')\n'
        'value=mode.read_text() if mode.exists() else ""\n'
        'Path('+repr(str(base/'preview-pid'))+').write_text(str(os.getpid()))\n'
        'Path('+repr(str(base/'preview-started'))+').write_text(value)\n'
        'if value=="fail": sys.exit(1)\n'
        'if value=="delay": time.sleep(1.5)\n'
        'if value=="stall": time.sleep(10)\n'
        'os.execv('+repr(shutil.which('ffmpeg'))+', ["ffmpeg", *sys.argv[1:]])\n')
    for script in fixtures.iterdir():
        script.chmod(0o700)
    env['PATH'] = str(fixtures)+':'+env['PATH']


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
        send(pointer, f'move {round(x)} {round(y)}'); time.sleep(.12)

    def click(x, y):
        move(x, y); send(pointer, 'press'); send(pointer, 'release'); time.sleep(.35)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def words(name, output='TEST-1'):
        im = shot(name, output).resize((1280,720))
        crop = im.crop((430,0,850,260)).resize((1260,780))
        found=[]
        for variant,img in (('raw',crop),('bright',crop.convert('L').point(lambda p:255 if p>180 else 0))):
            path = out/(name+'-text-'+variant+'.png'); img.save(path)
            result = run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','11','-c','tessedit_create_tsv=1'])
            rows = csv.DictReader(io.StringIO(result[result.index('level\t'):]),delimiter='\t')
            found.extend((r['text'].strip(),430+(int(r['left'])+int(r['width'])/2)/3,
                          (int(r['top'])+int(r['height'])/2)/3) for r in rows if (r.get('text') or '').strip())
        return found

    def text(name, output='TEST-1'):
        return ' '.join(w for w,x,y in words(name,output))

    def clear():
        msg('notification-clear-active'); msg('notification-clear-history'); move(1100,600); time.sleep(.6)

    def record(mode='', output='TEST-1'):
        clear(); (base/'preview-mode').write_text(mode)
        (base/'preview-started').unlink(missing_ok=True)
        dispatch('hl.dsp.focus({monitor='+json.dumps(output)+'})')
        offset = 720 if output=='TEST-2' else 0
        move(1100,600+offset); msg('record-region'); time.sleep(.6)
        move(100,250+offset); send(pointer,'press'); move(500,500+offset); send(pointer,'release')
        wait(lambda:msg('record-status').startswith('REC'), 'Recording did not start')
        move(1100,600+offset); time.sleep(1.3)
        msg('record-stop'); wait(lambda:msg('record-status')=='idle','Recording did not stop')
        wait(lambda:(base/'preview-started').exists(),'Preview decoder did not launch')
        return max((out/'Recordings').glob('*.mp4'),key=lambda p:p.stat().st_mtime)

    def green(im, crop):
        # The translucent halo blends with the navy wallpaper, including at reduced strength.
        return sum(g>r*2 and g>b*.7 and g>40 for r,g,b in im.crop(crop).getdata())

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
        terminal=start(['kitty','--config','NONE','--override','background=#20a040','-e','sleep','300'],'result-window.log')
        wait(lambda:bool(json.loads(ctl('-j','clients'))),'Colour window did not map')
        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'result-player.log'); time.sleep(1)

        video=record(); terminal.terminate(); terminal.wait(timeout=5); time.sleep(.6)
        content=text('recording-result')
        assert all(word in content for word in ('Recording','saved','Play','Folder')),content
        assert re.search(r'\d+:\d{2}',content) and re.search(r'\b(?:B|KB|MB)\b',content),content
        im=shot('recording-result-thumbnail')
        assert green(im,(710,55,800,100))>2500,'Video thumbnail missing'
        a=shot('recording-result-art-before'); time.sleep(.5); b=shot('recording-result-art-after')
        strip=(580,14,690,20)
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(),'Saved card froze artwork'
        assert sum(max(p)>20 for p in a.crop(strip).getdata())>300,'Saved card hid artwork'
        # The pulse can be at its trough in any one frame.
        first=Image.open(out/'recording-result.png').convert('RGB')
        assert max(green(frame,(560,156,720,164)) for frame in (first,im,a,b))>20,'Green completion glow missing'
        play=next((x,y) for w,x,y in words('recording-result-actions') if w=='Play')
        click(*play)
        wait(lambda:(base/'opened').exists(),'Play did not open the video')
        assert json.loads((base/'opened').read_text())==[str(video)]

        # The timeout returns to media, while holding the card keeps the actions usable.
        (base/'opened').unlink(); record(); time.sleep(5.7)
        assert 'Recording saved' not in text('recording-result-expired')
        assert green(shot('recording-result-no-glow'),(560,156,720,164))<10

        # Fractional scale, rotated output and keyboard folder action.
        video=record(output='TEST-2'); time.sleep(.6); msg('island-focus'); time.sleep(.5)
        content=text('recording-result-scaled','TEST-2'); assert 'Folder' in content,content
        send(keyboard,'15'); send(keyboard,'15'); send(keyboard,'28')
        wait(lambda:(base/'opened').exists(),'Keyboard folder action did not run')
        assert json.loads((base/'opened').read_text())==[str(video.parent)]

        # A missing decoder leaves the saved file and actions usable.
        video=record('fail'); time.sleep(.5)
        content=text('recording-result-no-thumbnail'); assert 'saved' in content and 'Play' in content,content
        assert video.stat().st_size>0
        clear()

        # Dismissing while decoding must not bring the result back when the frame arrives.
        record('delay'); clear(); time.sleep(2)
        assert 'Recording saved' not in text('recording-result-dismissed')

        # A stuck decoder is bounded; the steady reduced-motion glow expires even while hovered.
        path=cfg/'config.toml'; path.write_text(path.read_text()+'\n[shell.animation]\nenabled=false\n')
        msg('config-reload'); record('stall'); move(640,80)
        a=shot('recording-result-reduced'); time.sleep(.4); b=shot('recording-result-reduced-after')
        edge=(560,156,720,164)
        assert green(a,edge)>20 and not ImageChops.difference(a.crop(edge),b.crop(edge)).getbbox()
        time.sleep(5)
        assert not pathlib.Path('/proc/'+(base/'preview-pid').read_text()).exists(),'Preview decoder exceeded its deadline'
        assert 'saved' in text('recording-result-held')
        assert green(shot('recording-result-held-no-glow'),edge)<10,'Completion glow did not expire'
        clear(); assert shell.poll() is None
        print('PASS: recording thumbnail, duration/size, Play, keyboard folder, artwork, five-second glow, scaled output, decoder failure/timeout, dismissal and reduced motion',flush=True)
    finally:
        if shell.poll() is None:
            msg('record-stop')
        for proc in (pointer,keyboard):
            proc.terminate(); proc.wait(timeout=5)
