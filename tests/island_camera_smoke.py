"""Camera Live Activity, using direct-device fixtures and real private PipeWire links."""
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

    proc=base/'fakeproc'; proc.mkdir()
    env['NOCTALIA_PRIVACY_PROC_ROOT']=str(proc)

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
    def capture(index, linked=True, app="camtest", binary="camtest", kind="Camera"):
        # The classifier uses the node classes and source metadata. Audio DSP ports provide a
        # deterministic transport for these video-class nodes without a real camera or portal.
        source, consumer = f'sharing-output-{index}', f'sharing-input-{index}'
        proc = start(['pw-loopback', '-n', f'sharing-{index}', '-c', '1', '-m', 'MONO',
                      '--capture-props', f'node.name={consumer} media.class=Stream/Input/Video application.name="{app}" application.process.binary={binary}',
                      '--playback-props', f'node.name={source} media.class=Video/Source media.name="{kind} capture"'],
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



    def direct_camera(index, app='camtest'):
        proc=base/'fakeproc'/str(4242+index)
        (proc/'fd').mkdir(parents=True)
        (proc/'comm').write_text(app+'\n'); (proc/'cmdline').write_bytes(app.encode()+b'\0')
        (proc/'exe').symlink_to('/usr/bin/'+app)
        fd=proc/'fd'/'3'; fd.symlink_to('/dev/video0')
        # Allow the two-second device scan and its brief Camera Active OSD to finish.
        time.sleep(4.2)
        return fd

    def close():
        msg('panel-close'); move(1100,600); time.sleep(.9)

    def open_camera(output='TEST-1'):
        move(1100,600 if output=='TEST-1' else 1320); time.sleep(.8)
        move(620,40 if output=='TEST-1' else 760); time.sleep(.9)
        move(818,85 if output=='TEST-1' else 805)

    def green(p):
        return p[1]>150 and p[1]>p[0]+70 and p[1]>p[2]+50

    def elapsed(name):
        import re
        # Isolate the coloured elapsed label. Alternate thresholds handle antialiased
        # edges over the live shader; reject impossible second values from OCR.
        crop=shot(name).crop((450,120,665,215))
        for threshold in (50,35,70):
            mask=Image.new('L',crop.size)
            mask.putdata([0 if (g>80 and g>r+threshold and g>b+20)
                          or (b>120 and r>100 and b>g+40 and r>g+30) else 255 for r,g,b in crop.getdata()])
            path=out/(name+f'-elapsed-{threshold}.png'); mask.resize((crop.width*5,crop.height*5)).save(path)
            content=run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','6'])
            values=[int(m)*60+int(s) for m,s in re.findall(r'(\d+):([0-5]\d)',content)]
            if values: return values[0]
        raise AssertionError('Elapsed time missing: '+name+' '+content)

    def active():
        return json.loads(ctl('-j','activewindow')).get('class','')

    def focus_other():
        move(1100,600)
        window=next(w for w in json.loads(ctl('-j','clients')) if w['class']=='other')
        dispatch('hl.dsp.focus({window='+json.dumps('address:'+window['address'])+'})')
        wait(lambda:active()=='other','Other window did not receive focus')

    def clear_notes():
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.7)

    mic=None
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark'); time.sleep(1)
        if '--island-privacy-rotation-only' in sys.argv:
            direct=direct_camera(9)
            run(['pactl','load-module','module-remap-source','source_name=privacy-mic','master=hyprland-test.monitor'])
            mic=subprocess.Popen(['parec','--device=privacy-mic','--client-name=Privacy test'],env=env,
                                 stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
            screen,_=capture(10,kind='Screen')
            run(['notify-send','-t','2000','Unread test','Waits in history unseen.']); time.sleep(4)
            slot=(708,26,740,62)
            seen=set()
            for index in range(23):
                frame=shot(f'privacy-rotation-{index}')
                pixels=list(frame.crop(slot).getdata())
                if sum(green(p) for p in pixels)>5: seen.add('camera')
                if sum(r>200 and 110<g<195 and b<80 for r,g,b in pixels)>5: seen.add('microphone')
                if sum(r>140 and b>170 and g<125 for r,g,b in pixels)>5: seen.add('screen')
                if sum(min(p)>205 for p in pixels)>5: seen.add('unread')
                time.sleep(1)
            assert seen=={'camera','microphone','screen','unread'},seen
            # Enter after a rotation so the outgoing visual still exists in the scene.
            # A coloured glyph proves the slot stayed compact; two expanded frames can
            # otherwise compare equal without exercising the indicator's hover target.
            for index in range(21):
                if sum(green(p) for p in shot(f'privacy-camera-wait-{index}').crop(slot).getdata())>5:
                    time.sleep(.5)
                    break
                time.sleep(1)
            else:
                raise AssertionError('Camera did not rotate into the compact slot')
            # Hover holds the current glyph instead of rotating its click target away.
            move(724,43); time.sleep(.8); held=shot('privacy-rotation-held').crop(slot)
            assert sum(green(p) for p in held.getdata())>5, 'Hover missed the compact camera indicator'
            time.sleep(6)
            assert not ImageChops.difference(held,shot('privacy-rotation-held-after').crop(slot)).getbbox(), 'Hover did not hold the indicator'
            move(1100,600)
            mic.terminate(); mic.wait(timeout=5); mic=None; direct.unlink(); stop(screen); time.sleep(2)
            # The bell on its own must use the same small glyph as the rotating slot.
            for output in ('TEST-1','TEST-2'):
                frame=shot('privacy-bell-alone-'+output,output).resize((1280,720))
                pixels=[(x,y) for x in range(686,724) for y in range(25,65) if min(frame.getpixel((x,y)))>205]
                assert pixels, (output,'Unread bell missing')
                width=max(x for x,y in pixels)-min(x for x,y in pixels)+1
                height=max(y for x,y in pixels)-min(y for x,y in pixels)+1
                assert width<=19 and height<=19,(output,'Unread bell is larger than the privacy icons',width,height)
            assert shell.poll() is None and not ctl('configerrors').strip()
            print('PASS: microphone/camera/screen/unread rotation, hover hold and matching compact bell size on both outputs',flush=True)
            return
        unlinked,_=capture(0,linked=False)
        open_camera(); assert 'Active for' not in text('camera-unlinked')
        stop(unlinked); close()
        direct=direct_camera(0)
        compact=shot('camera-compact')
        for i in range(4):
            frame=shot(f'camera-quiet-{i}')
            red=max(r-max(g,b) for r,g,b in frame.crop((620,80,660,84)).getdata())
            assert red<8, ('Camera still triggers a red outer glow',red)
            time.sleep(.6)
        pixels=[(x,y) for x in range(640,880) for y in range(25,65) if green(compact.getpixel((x,y)))]
        assert pixels, 'Green camera indicator missing'
        cx,cy=sum(x for x,y in pixels)/len(pixels),sum(y for x,y in pixels)/len(pixels)
        click(cx,cy); move(818,85)
        content=text('camera-expanded')
        assert 'Camera' in content and 'camtest' in content and 'Active' in content,content
        assert not json.loads(msg('status'))['panelOpen'], 'Camera opened a panel instead of its activity'
        before=elapsed('camera-before'); time.sleep(2); assert elapsed('camera-after')>=before+2
        move(744,129); send(pointer,'press'); time.sleep(1.3); send(pointer,'release'); move(818,85); time.sleep(.6)
        assert 'window' in text('camera-background-app')
        print('PASS: direct camera indicator, live elapsed time and background app feedback',flush=True)

        kitty=['kitty','--config','NONE','-o','confirm_os_window_close=0']
        start(kitty+['--class','camtest','-e','sleep','300'],'camtest.log')
        start(kitty+['--class','other','-e','sleep','300'],'other.log')
        wait(lambda:len(json.loads(ctl('-j','clients')))==2,'Camera fixture windows')
        focus_other(); open_camera(); move(744,129); send(pointer,'press'); time.sleep(1.3); send(pointer,'release')
        wait(lambda:active()=='camtest','Held Open App click did not focus camera app')
        focus_other(); close(); move(620,40); time.sleep(.8); move(1100,600); time.sleep(.8)
        msg('island-focus'); time.sleep(.8); send(keyboard,'15'); send(keyboard,'28')
        wait(lambda:active()=='camtest','Keyboard Open App did not release focus grab')
        focus_other(); open_camera('TEST-2'); assert 'camtest' in text('camera-scaled','TEST-2')
        click(744,849); wait(lambda:active()=='camtest','Scaled Open App failed')
        move(1100,1320); time.sleep(.8); close(); open_camera()
        click(798,43); assert json.loads(msg('status'))['panelOpen'], 'Privacy settings link failed'
        close(); open_camera(); before=elapsed('camera-direct-time')

        # Direct and PipeWire routes for the same app must merge without resetting the timer.
        first,_=capture(1)
        assert text('camera-direct-and-pipewire').count('Active')==1
        direct.unlink(); time.sleep(2.7)
        assert elapsed('camera-pipewire-only')>=before
        second,_=capture(2)
        stop(first); assert text('camera-last-stream').count('Active')==1
        third,_=capture(3,app='Other Camera',binary='other')
        assert text('camera-two-apps').count('Active')==2
        stop(third); assert text('camera-other-ended').count('Active')==1
        print('PASS: held click, keyboard and scaled app focus; merged and overlapping camera streams',flush=True)

        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        player=start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log'); time.sleep(1.5)
        close(); open_camera()
        # The green camera icon in the expanded media footer opens the same activity.
        media=shot('camera-media-indicator')
        pixels=[(x,y) for x in range(600,680) for y in range(220,330) if green(media.getpixel((x,y)))]
        assert pixels, 'Expanded media camera icon missing'
        click(sum(x for x,y in pixels)/len(pixels),sum(y for x,y in pixels)/len(pixels)); move(818,120)
        assert 'camtest' in text('camera-media-card')
        a=shot('camera-art-before'); time.sleep(.5); b=shot('camera-art-after'); strip=(580,14,690,20)
        assert sum(max(p)>12 for p in b.crop(strip).getdata())>300, 'Camera card hid media artwork'
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Camera card froze media artwork'
        click(551,35); time.sleep(.5); click(728,35); move(818,120)
        assert 'camtest' in text('camera-tab')
        run(['notify-send','-u','critical','Urgent alert','Camera stays active'])
        time.sleep(.7); assert 'Urgent' in text('camera-urgent')
        clear_notes(); assert 'camtest' in text('camera-restored')
        close(); msg('volume-osd','43'); time.sleep(.35)
        assert not any(green(p) for p in shot('camera-volume-osd').crop((400,8,880,75)).getdata()), 'Camera icon appeared in volume OSD'
        time.sleep(2.5); open_camera(); click(728,35)

        # Camera and screen timers from the same app remain independent.
        camera_age=elapsed('camera-age-before-sharing')
        screen,_=capture(4,kind='Screen')
        click(760,35); move(818,120)
        assert elapsed('camera-with-sharing')>=camera_age
        click(640,35); move(818,120)
        assert elapsed('screen-with-camera')<camera_age
        stop(second); close(); open_camera(); click(728,35); move(818,120)
        assert 'Screen' in text('screen-after-camera') and 'camtest' in text('screen-still-active')
        stop(screen)
        print('PASS: expanded indicator, activity tabs, artwork, urgent alerts, OSD and independent camera/screen timers',flush=True)

        direct=direct_camera(9); close(); open_camera(); click(728,35); move(818,120)
        config=cfg/'config.toml'; original=config.read_text(); before=elapsed('camera-reload-before')
        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload'); time.sleep(.8)
        open_camera(); click(728,35); move(818,120)
        assert elapsed('camera-reduced')>=before, 'Reload reset camera elapsed time'
        config.write_text(original+'\n[shell.privacy]\ncam_filter_regex="camtest"\n'); msg('config-reload'); time.sleep(.8)
        assert 'camtest' not in text('camera-filtered')
        config.write_text(original); msg('config-reload'); time.sleep(.8)
        open_camera(); click(728,35); move(818,120); assert 'camtest' in text('camera-unfiltered')
        player.terminate(); player.wait(timeout=5); close(); time.sleep(1)

        # Preserve the single compact slot, cycling camera, microphone and unread history.
        run(['pactl','load-module','module-remap-source','source_name=privacy-mic','master=hyprland-test.monitor'])
        mic=subprocess.Popen(['parec','--device=privacy-mic','--client-name=Privacy test'],env=env,
                             stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        run(['notify-send','-t','2000','Unread test','Waits in history unseen.']); time.sleep(4)
        seen=set(); patterns=set()
        for index in range(17):
            frame=shot(f'camera-rotation-{index}')
            region=frame.crop((round(cx)-13,round(cy)-13,round(cx)+14,round(cy)+14))
            values=list(region.getdata())
            if sum(green(p) for p in values)>5: seen.add('camera')
            if sum(r>200 and 110<g<195 and b<80 for r,g,b in values)>5: seen.add('microphone')
            if sum(min(p)>205 for p in values)>5: seen.add('unread')
            patterns.add(tuple(max(p)>160 for p in values))
            time.sleep(1)
        assert seen=={'camera','microphone','unread'} and len(patterns)>=3,seen
        mic.terminate(); mic.wait(timeout=5); mic=None; clear_notes(); time.sleep(2.7)
        focus_other(); close(); move(620,40); time.sleep(.8); move(1100,600); time.sleep(.8)
        msg('island-focus'); time.sleep(.8); send(keyboard,'15')
        direct.unlink(); time.sleep(2.7)
        assert 'camtest' not in text('camera-ended')
        assert active()=='other' and not json.loads(msg('status'))['panelOpen'], 'Ending capture kept keyboard focus'
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: camera filters, reload, reduced motion, compact camera/mic/unread rotation and final capture cleanup',flush=True)
    finally:
        if mic is not None and mic.poll() is None:
            mic.terminate(); mic.wait(timeout=5)
        for proc in captures:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
        keyboard.terminate(); keyboard.wait(timeout=5)
