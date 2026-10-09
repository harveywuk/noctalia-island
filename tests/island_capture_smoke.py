"""Screen capture Live Activity, with real PipeWire links and shell-owned recordings."""
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

    def words(name, output='TEST-1', height=360):
        im = shot(name, output); factor = im.width/1280
        # Retain muted timer text, then isolate bright glyphs from animated artwork.
        # Cropping avoids Tesseract treating the black terminal windows as page content.
        left=round(280*factor)
        crop=im.crop((left,0,round(1000*factor),round(height*factor)))
        zoom=crop.resize((round(2160*factor),round(height*3*factor)))
        found=[]
        for variant,img in (('raw',zoom),('bright',zoom.point(lambda p:255 if p>180 else 0))):
            path=out/(name+'-text-'+variant+'.png'); img.save(path)
            result=run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','11','-c','tessedit_create_tsv=1'])
            rows=csv.DictReader(io.StringIO(result[result.index('level\t'):]),delimiter='\t')
            for r in rows:
                word=(r.get('text') or '').strip()
                if not word: continue
                x=280+(int(r['left'])+int(r['width'])/2)/(3*factor)
                y=(int(r['top'])+int(r['height'])/2)/(3*factor)
                if not any(word==w and abs(x-px)<8 and abs(y-py)<6 for w,px,py in found):
                    found.append((word,x,y))
        return found

    def text(name, output='TEST-1', height=360):
        return ' '.join(w for w,x,y in words(name, output, height))

    captures = []
    def capture(index, linked=True, app=None, binary="capture-test"):
        # The classifier uses the node classes and source metadata. Audio DSP ports provide a
        # deterministic transport for these video-class nodes without a real camera or portal.
        app = app or f'Sharing Test {index}'
        source, consumer = f'sharing-output-{index}', f'sharing-input-{index}'
        proc = start(['pw-loopback', '-n', f'sharing-{index}', '-c', '1', '-m', 'MONO',
                      '--capture-props', f'node.name={consumer} media.class=Stream/Input/Video application.name="{app}" application.process.binary={binary}',
                      '--playback-props', f'node.name={source} media.class=Video/Source media.name="Screen capture"'],
                     f'sharing-{index}.log')
        captures.append(proc); time.sleep(.7)
        assert proc.poll() is None
        for node in json.loads(run(['pw-dump'])):
            name = node.get('info',{}).get('props',{}).get('node.name')
            if name not in (source,consumer):
                continue
            direction = 'Output' if name == source else 'Input'
            run(['pw-cli','set-param',str(node['id']),'PortConfig',
                 '{ direction = '+direction+' mode = dsp format = { mediaType = audio mediaSubtype = raw format = F32P rate = 48000 channels = 1 position = [ MONO ] } }'])
        time.sleep(.3)
        ports = (next(v.strip() for v in run(['pw-link','-o']).splitlines() if source+':' in v),
                 next(v.strip() for v in run(['pw-link','-i']).splitlines() if consumer+':' in v))
        if linked:
            run(['pw-link',*ports]); time.sleep(2.7)
        return proc, ports

    def stop(proc):
        proc.terminate(); proc.wait(timeout=5); time.sleep(2.7)


    def close():
        msg('panel-close'); move(1100,600); time.sleep(.9)

    def open_capture(output='TEST-1'):
        move(1100,600 if output=='TEST-1' else 1320); time.sleep(.8)
        move(620,40 if output=='TEST-1' else 760); time.sleep(.9)
        move(818,85 if output=='TEST-1' else 805)

    def button(word, name, output='TEST-1', index=0):
        # Exercise fixed layout targets; muted tab labels vary over the live artwork.
        # Each click is verified by its resulting card, focus or recorder state.
        shot(name,output)
        points={'Screen':(728,35),'Stop':(744,177),'Open':(744,129+index*73)}
        x,y=points[word]
        return x,y+(720 if output=='TEST-2' else 0)

    def recording():
        return msg('record-status').startswith('REC')

    def start_recording(settle=1.3):
        close(); msg('record-region'); time.sleep(.6)
        move(100,250); send(pointer,'press'); move(500,500); send(pointer,'release')
        wait(recording, 'Private recording did not start'); move(1100,600); time.sleep(settle)

    def glow(image):
        # Just outside the 64 px capsule at UI scale 1.1, independent of artwork and glyphs.
        band=list(image.resize((1280,720)).crop((620,80,660,84)).getdata())
        return max(r-max(g,b) for r,g,b in band), max(min(r-g,b-g) for r,g,b in band)

    def recording_intro(name, sharing=False, reduced=False):
        values=[]; begin=time.monotonic()
        while time.monotonic()-begin<5.7:
            age=time.monotonic()-begin
            values.append((age,*glow(shot(name+'-'+str(len(values))))))
            time.sleep(.17)
        (out/(name+'.json')).write_text(json.dumps(values))
        red=[r for _,r,_ in values]
        if reduced:
            early=[r for age,r,_ in values if .6<age<3.5]
            assert min(early)>10 and max(early)-min(early)<=3, ('Intro was not steady with reduced motion',values)
        else:
            peaks=sum(r>15 and (i==0 or red[i-1]<=15) for i,r in enumerate(red))
            assert peaks==2, ('Expected two recording startup pulses',values)
        assert all(r<8 for age,r,_ in values if age>5), ('Recording pulse did not finish',values)
        if sharing:
            assert all(p>12 for age,_,p in values if age>5), ('Sharing glow did not return',values)
        assert recording(), 'Intro expiry stopped the recording'

    def clear_notifications():
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.7)

    def elapsed(name):
        import re
        content = text(name)
        return [int(m)*60+int(s) for m,s in re.findall(r'(\d+):(\d{2})',content)]

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark'); time.sleep(1)
        first, ports = capture(1, linked=False)
        open_capture(); assert 'Active for' not in text('capture-unlinked')
        close(); run(['pw-link',*ports]); time.sleep(2.7)
        # Clicking the purple privacy icon opens the card without opening a panel.
        compact = shot('capture-icon')
        purple = [(x,y) for x in range(640,880) for y in range(25,65)
                  if (lambda p: p[0]>140 and p[2]>170 and p[1]<125)(compact.getpixel((x,y)))]
        assert purple, 'Purple screen icon missing'
        click(sum(x for x,y in purple)/len(purple),sum(y for x,y in purple)/len(purple)); move(818,85)
        content=text('capture-expanded')
        assert all(s in content for s in ('Screen','Sharing','Test','Active','Open')), content
        assert not json.loads(msg('status'))['panelOpen']
        before=elapsed('capture-time-before'); time.sleep(2); after=elapsed('capture-time-after')
        assert before and after and after[0]>=before[0]+2, (before,after)
        # Background capture feedback, including a press held across timer ticks.
        move(*button('Open','capture-open-button')); send(pointer,'press'); time.sleep(1.3); send(pointer,'release'); time.sleep(.8)
        assert 'window' in text('capture-background-app')

        # A second stream from the same app shares its original start time.
        second,_=capture(2,app='Sharing Test 1')
        content=text('capture-two-streams'); assert content.count('Active')==1,content
        stop(first); assert 'Active' in text('capture-one-stream-remains')
        assert elapsed('capture-original-time')[0]>=after[0]
        third,_=capture(3,app='Another App',binary='capture-other')
        assert text('capture-two-apps').count('Active')==2
        stop(third); assert text('capture-other-ended').count('Active')==1

        # Open app focuses a real matching window through pointer and keyboard actions.
        terminal=start(['kitty','--config','NONE','--class','capture-test','-e','sleep','300'],'capture-window.log')
        other=start(['kitty','--config','NONE','--class','capture-other','-e','sleep','300'],'other-window.log')
        wait(lambda:len(json.loads(ctl('-j','clients')))==2,'Fixture windows')
        def focus_other():
            move(1100,600)
            window=next(w for w in json.loads(ctl('-j','clients')) if w['class']=='capture-other')
            dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
        def active():
            return json.loads(ctl('-j','activewindow')).get('class','')
        focus_other(); open_capture(); click(*button('Open','capture-app-button'))
        wait(lambda:active()=='capture-test','Open app did not focus matching window')
        focus_other(); close(); move(620,40); time.sleep(.7); move(1100,600); time.sleep(.8)
        msg('island-focus'); time.sleep(.7)
        # Settings is the first control, followed by the sole app button.
        send(keyboard,'15'); send(keyboard,'28')
        wait(lambda:active()=='capture-test','Keyboard Open app did not release the Island grab')
        close(); open_capture('TEST-2')
        assert 'Active' in text('capture-scaled','TEST-2')
        click(*button('Open','capture-scaled-button','TEST-2'))
        wait(lambda:active()=='capture-test','Scaled Open app failed')
        move(1100,1320); time.sleep(.8); close()

        # The media shader remains animated beneath the Screen activity.
        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log'); time.sleep(1.5)
        open_capture(); click(*button('Screen','capture-media-tab')); move(818,120)
        assert 'Active' in text('capture-media')
        a=shot('capture-art-before'); time.sleep(.5); b=shot('capture-art-after'); strip=(580,14,690,20)
        assert sum(max(p)>12 for p in b.crop(strip).getdata())>300, 'Capture card hid artwork'
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Capture card froze artwork'
        run(['notify-send','-u','critical','Urgent alert','Capture continues underneath'])
        time.sleep(.7); assert 'Urgent' in text('capture-urgent')
        clear_notifications(); assert 'Active' in text('capture-restored')
        click(798,91); time.sleep(.6)
        assert json.loads(msg('status'))['panelOpen'], 'Privacy link failed'
        close()

        # A shell recording coexists with sharing and owns the only Stop action.
        start_recording(settle=0); recording_intro('recording-sharing-intro',sharing=True)
        assert 'recording' in text('recording-compact',height=100)
        open_capture(); content=text('capture-recording-and-sharing')
        assert 'Sharing' in content and recording(),content
        assert recording(), 'Opening the recording card stopped it'
        run(['notify-send','-u','critical','Recording alert','Recording must continue'])
        time.sleep(.7); assert 'alert' in text('recording-urgent') and recording()
        clear_notifications()
        move(*button('Stop','recording-stop-button')); send(pointer,'press'); time.sleep(1.3)
        assert recording(), 'Press alone stopped recording'
        send(pointer,'release'); wait(lambda:msg('record-status')=='idle','Live Activity Stop did not finish recording')
        clear_notifications(); move(818,120)
        content=text('sharing-after-recording'); assert 'Active' in content and 'Stop' not in content,content
        videos=list((out/'Recordings').glob('*.mp4')); assert videos, 'No recording saved'
        streams=json.loads(run(['ffprobe','-v','error','-show_entries','stream=codec_type','-of','json',str(videos[-1])]))['streams']
        assert {s['codec_type'] for s in streams}=={'audio','video'},streams

        # Filtering removes the live session; reload alone preserves it. Reduced motion remains usable.
        config=cfg/'config.toml'; original=config.read_text()
        open_capture(); click(*button('Screen','capture-before-reload')); before=elapsed('capture-reload-before')
        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload'); time.sleep(.8)
        open_capture(); click(*button('Screen','capture-reduced-tab'))
        after=elapsed('capture-reduced'); assert before and after and after[0]>=before[0],(before,after)
        config.write_text(original+'\n[shell.privacy]\nscreen_filter_regex="Sharing Test"\n'); msg('config-reload'); time.sleep(.8)
        assert 'Active for' not in text('capture-filtered')
        config.write_text(original); msg('config-reload'); time.sleep(.8)
        open_capture(); click(*button('Screen','capture-filter-cleared')); assert 'Active' in text('capture-unfiltered')
        stop(second); assert 'Active for' not in text('capture-ended')

        # Recording alone supports keyboard focus and returns to media when it ends.
        start_recording(settle=0); recording_intro('recording-alone-intro')
        quiet=shot('recording-steady')
        assert sum(r>150 and r>g+60 and r>b+60 for r,g,b in quiet.crop((350,15,930,73)).getdata())>20, 'Steady recording icon missing'
        assert 'recording' in text('recording-steady-timer',height=100)
        msg('config-reload'); time.sleep(1)
        for i in range(5):
            assert glow(shot(f'recording-reload-quiet-{i}'))[0]<8, 'Reload replayed the recording pulse'
            assert glow(shot(f'recording-scaled-quiet-{i}','TEST-2'))[0]<8, 'Scaled output retained the recording pulse'
            time.sleep(.5)
        a=shot('recording-art-before'); time.sleep(.5); b=shot('recording-art-after')
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Recording froze media artwork'
        open_capture(); assert 'Noctalia' in text('recording-alone')
        close(); move(620,40); time.sleep(.7); move(1100,600); time.sleep(.8); msg('island-focus'); time.sleep(.8)
        assert 'Stop' in text('recording-keyboard')
        send(keyboard,'1'); time.sleep(.8); assert recording()
        msg('record-stop'); wait(lambda:msg('record-status')=='idle','IPC Stop failed'); clear_notifications()

        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload'); time.sleep(.8)
        start_recording(settle=0); recording_intro('recording-reduced-intro',reduced=True)
        msg('record-stop'); wait(lambda:msg('record-status')=='idle','Reduced-motion Stop failed'); clear_notifications()
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: screen capture app timers, overlapping streams, app focus, keyboard, scaled output, media artwork, urgent alerts, two startup pulses, steady recording icon, sharing restoration, recording Stop, saved audio/video, filters, reload and reduced motion',flush=True)
    finally:
        if shell.poll() is None:
            msg('record-stop')
        for proc in captures:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
        keyboard.terminate(); keyboard.wait(timeout=5)
