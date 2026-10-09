"""Local OCR, timed Keep Awake, Focus and live privacy in a private GPU desktop."""
import csv
import io
import json
import os
import pathlib
import subprocess
import sys
import time
import tomllib

from PIL import Image, ImageChops

REPO = pathlib.Path(__file__).resolve().parents[1]
TESSDATA = os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]',
                    'hover_widgets=[]\nmedia_gradient=true\ntrack_preview_seconds=0')
                    .replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    .replace('[shell.screenshot]\n', '[shell.screenshot]\nannotate=true\ntext_data_directory='+json.dumps(TESSDATA)+'\n')
                    + '\n[notification.focus.work]\nallowed_apps=["AllowedApp"]\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    for kind, proto, fixture, libs in (
        ('pointer', REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', REPO/'tests/fixtures/island_pointer.c', ['-lwayland-client']),
        ('keyboard', REPO/'protocols/virtual-keyboard-unstable-v1.xml', REPO/'tests/fixtures/island_keyboard.c', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(proto), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(proto), str(base/(kind+'-code.c'))])
        source = base/(kind+'.c')
        source.write_text(fixture.read_text().replace('x,y,1280,720', 'x,y,1280,1440'))
        run(['cc', '-I'+str(base), str(source), str(base/(kind+'-code.c')), *libs, '-o', str(base/kind)])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    captures = []

    def send(proc, command):
        proc.stdin.write(command+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        send(pointer, 'relative 1 0'); send(pointer, 'relative -1 0'); time.sleep(.12)

    def click(x, y):
        move(x, y); send(pointer, 'press'); time.sleep(.08); send(pointer, 'release'); time.sleep(.35)

    def key(command):
        send(keyboard, str(command)); time.sleep(.2)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)])
        return path

    def words(name, output='TEST-1', threshold=False):
        path = shot(name, output)
        image = Image.open(path)
        # Upscale shell text for robust matching; return coordinates in logical output units.
        zoom = out/(name+'-ocr.png'); enlarged = image.resize((image.width*2,image.height*2))
        if threshold:
            enlarged = enlarged.getchannel('G').point(lambda p: 0 if p>160 else 255)
        enlarged.save(zoom)
        result = run(['tesseract', str(zoom), 'stdout', '--tessdata-dir', TESSDATA, '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        rows = list(csv.DictReader(io.StringIO(result[result.index('level\t'):]), delimiter='\t'))
        factor = 3 if output == 'TEST-2' else 2
        return [(r['text'].strip(), (int(r['left'])+int(r['width'])/2)/factor,
                 (int(r['top'])+int(r['height'])/2)/factor) for r in rows if (r.get('text') or '').strip()]

    def text(name):
        return ' '.join(w for w,x,y in words(name))

    def click_word(word, name, occurrence=0):
        found = [(x,y) for w,x,y in words(name) if w.casefold() == word.casefold()]
        if len(found)<=occurrence:
            found = [(x,y) for w,x,y in words(name+'-threshold',threshold=True) if w.casefold() == word.casefold()]
        assert len(found)>occurrence, (word,found,name)
        x,y = found[occurrence]; click(round(x),round(y))

    def panel(tab):
        msg('panel-open','control-center',tab); time.sleep(.8)

    def close():
        msg('panel-close'); msg('settings-close'); move(1100,600); time.sleep(.7)

    def select(x0,y0,x1,y1):
        msg('text-capture'); time.sleep(.4)
        move(x0,y0); send(pointer,'press'); move(x1,y1); send(pointer,'release')

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark'); time.sleep(1)
        assert json.loads(msg('caffeine-status')) == {'enabled':False,'remaining_seconds':None}
        panel('power'); click_word('15','power-before')
        status = json.loads(msg('caffeine-status'))
        assert status['enabled'] and 880 < status['remaining_seconds'] <= 900, status
        assert 'remaining' in text('power-timed')
        click_word('Off','power-off-button',1); assert not json.loads(msg('caffeine-status'))['enabled']
        msg('caffeine-for','30'); msg('caffeine-enable')
        assert json.loads(msg('caffeine-status')) == {'enabled':True,'remaining_seconds':None}
        msg('caffeine-disable'); close()

        panel('focus'); shot('focus-initial'); click_word('Gaming','focus-gaming-button')
        assert json.loads(msg('focus-status'))['mode']=='gaming'
        click_word('Off','focus-off-button'); assert json.loads(msg('focus-status'))['mode']=='off'
        click_word('Follow','focus-auto-button'); assert json.loads(msg('focus-status'))['automatic']
        close(); msg('settings-open', 'notifications/focus'); time.sleep(.9)
        click_word('AllowedApp','focus-app-input'); key('chord 2 30')
        msg('clipboard-copy','AllowedApp, AdditionalApp'); key('chord 2 47')
        shot('focus-apps-edited')
        # Settings uses Tab between panes and Down between content controls,
        # scrolling focused controls into view on a shorter output.
        key(108); key(108); key(57)  # critical, schedule, toggle schedule
        key(108); key('chord 2 30')
        msg('clipboard-copy','23:00'); key('chord 2 47')
        key(108); key('chord 2 30')
        msg('clipboard-copy','07:00'); key('chord 2 47')
        for _ in range(8): key(108)  # seven weekday buttons, then Save
        shot('focus-save-button'); key(57); time.sleep(.7)
        saved=tomllib.loads((base/'state/noctalia/settings.toml').read_text())['notification']['focus']['work']
        assert saved['allowed_apps']==['AllowedApp','AdditionalApp'], saved
        assert (saved['start_minute'],saved['end_minute'],saved['schedule_enabled'])==(1380,420,True),saved
        shot('focus-editor-saved'); close()
        msg('focus-set','work'); time.sleep(2.5)
        run(['notify-send','-a','BlockedApp','Blocked notice','This must remain quiet'])
        time.sleep(.45); assert 'Blocked' not in text('focus-blocked')
        run(['notify-send','-a','AllowedApp','Allowed notice','This app is allowed'])
        time.sleep(.45); assert 'Allowed' in text('focus-allowed')
        msg('notification-clear-active'); msg('notification-clear-history'); msg('focus-set','off'); time.sleep(2.5)
        panel('privacy'); assert 'No capture' in text('privacy-idle'); close()

        # A linked screen source reports an app that has a real window to activate.
        app = start(['kitty','--config','NONE','--class','kitty','-o','background=#ffffff','-o','foreground=#000000',
                     '-o','font_size=28','-e',sys.executable,'-c',
                     'import time; print("\\n\\nHELLO ISLAND\\n12345", flush=True); time.sleep(300)'],'text-app.log')
        wait(lambda:any(c['class']=='kitty' for c in json.loads(ctl('-j','clients'))),'Text window')
        time.sleep(1)
        msg('clipboard-copy','unchanged')
        select(20,80,1000,370)
        wait(lambda:'HELLO ISLAND' in msg('clipboard-text'),'Recognised text missing from clipboard')
        assert '12345' in msg('clipboard-text'); shot('text-copied')
        assert not list(out.glob('screenshot_*.png')), 'Text capture saved a screenshot'
        time.sleep(2.5)
        msg('clipboard-copy','cancelled-clipboard'); msg('text-capture'); time.sleep(.3); key(1); time.sleep(.5)
        assert msg('clipboard-text')=='cancelled-clipboard'
        select(700,400,900,500); time.sleep(2)
        assert msg('clipboard-text')=='cancelled-clipboard'; shot('text-empty')

        # Move the same window onto the fractional, rotated output and read its pixels again.
        address = next(c['address'] for c in json.loads(ctl('-j','clients')) if c['class']=='kitty')
        dispatch('hl.dsp.window.move({window="address:'+address+'",monitor="TEST-2",follow=false})')
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); time.sleep(1)
        select(20,800,1000,1090)
        wait(lambda:'HELLO ISLAND' in msg('clipboard-text'),'Scaled text capture failed')
        shot('text-scaled','TEST-2')
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)

        capture = start(['pw-loopback','-n','utility-sharing','-c','1','-m','MONO',
                         '--capture-props','node.name=utility-input media.class=Stream/Input/Video application.name="Utility Screen" application.process.binary=kitty',
                         '--playback-props','node.name=utility-source media.class=Video/Source media.name="Screen capture"'],'capture.log')
        captures.append(capture); time.sleep(.7)
        for node in json.loads(run(['pw-dump'])):
            name=node.get('info',{}).get('props',{}).get('node.name')
            if name not in ('utility-input','utility-source'): continue
            direction='Input' if name=='utility-input' else 'Output'
            run(['pw-cli','set-param',str(node['id']),'PortConfig',
                 '{ direction = '+direction+' mode = dsp format = { mediaType = audio mediaSubtype = raw format = F32P rate = 48000 channels = 1 position = [ MONO ] } }'])
        time.sleep(.3)
        output=next(p.strip() for p in run(['pw-link','-o']).splitlines() if 'utility-source:' in p)
        input_=next(p.strip() for p in run(['pw-link','-i']).splitlines() if 'utility-input:' in p)
        run(['pw-link',output,input_]); time.sleep(3)
        # Open details through the actual compact sharing icon.
        frame=Image.open(shot('privacy-compact')).convert('RGB')
        xs=[x for x in range(frame.width) if max(frame.getpixel((x,20)))<12]
        ys=[y for y in range(0,200) if max(frame.getpixel((640,y)))<12]
        left,right,top,bottom=min(xs),max(xs),min(ys),max(ys)
        columns=[x for x in range(640,right) if any(min(frame.getpixel((x,y)))>200 for y in range(top,bottom))]
        start_x=columns[-1]
        while start_x-1 in columns or start_x-2 in columns: start_x-=1
        pixels=[(x,y) for x in range(start_x,columns[-1]+1) for y in range(top,bottom) if min(frame.getpixel((x,y)))>200]
        click(sum(x for x,y in pixels)//len(pixels),sum(y for x,y in pixels)//len(pixels)); time.sleep(.7)
        content=text('privacy-sharing'); assert 'Utility' in content and 'Screen' in content, content
        click_word('Open','privacy-open-app'); time.sleep(.7)
        assert json.loads(ctl('-j','activewindow'))['address']==address, 'Privacy did not activate the capture window'
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); panel('privacy')
        capture.terminate(); capture.wait(timeout=5); time.sleep(2.5)
        assert 'No capture' in text('privacy-stopped'), 'Privacy did not update while open'
        close()

        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log'); time.sleep(1.5)
        msg('focus-set','work'); time.sleep(.25)
        a=Image.open(shot('media-focus-before')).convert('RGB'); time.sleep(.45)
        b=Image.open(shot('media-focus-after')).convert('RGB')
        region=(590,14,690,20)
        assert sum(max(p)>12 for p in b.crop(region).getdata())>300, 'Focus covered the media artwork'
        assert ImageChops.difference(a.crop(region),b.crop(region)).getbbox(), 'Focus froze the media artwork'
        msg('focus-set','off')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: timed Keep Awake, Focus controls and exceptions, real local OCR, cancellation, empty text, scaling, live privacy, app activation and media artwork',flush=True)
    finally:
        for proc in (pointer,keyboard,*captures):
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
