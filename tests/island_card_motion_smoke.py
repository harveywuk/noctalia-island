"""Sample real status-card fades, including replacement during an unfinished fade."""
import json
import os
import pathlib
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
media_gradient=true
volume_show_percentage=true
track_preview_seconds=0
bluetooth_preview_seconds=12
network_preview_seconds=12
outer_progress_ring=false
split_activities=false''').replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    +'\n[shell.animation]\nenabled=true\nspeed=0.25\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, bluetooth, network):
    repo = pathlib.Path(__file__).resolve().parents[1]
    original = (cfg/'config.toml').read_text()

    def change(proc, **values):
        proc.stdin.write(json.dumps(values)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def at(began, offset):
        time.sleep(max(0, began+offset-time.monotonic()))

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim','-o',output,str(path)])
        image = Image.open(path).convert('RGB')
        return image.resize((1280,720)) if output == 'TEST-2' else image

    def text(name, expected=None, output='TEST-1', region=(490,20,795,64)):
        image = shot(name, output).crop(region)
        image = image.resize((image.width*4,image.height*4))
        words = []
        for index, sample in enumerate((image, image.convert('L').point(lambda p:255 if p>110 else 0))):
            path = out/f'{name}-text-{index}.png'; sample.save(path)
            words.append(run(['tesseract',str(path),'stdout','--tessdata-dir',
                              os.environ.get('NOCTALIA_TEST_TESSDATA',str(repo/'build-rishot/test-data/tessdata')),
                              '--psm','6']).lower())
        words = ' '.join(words)
        if expected:
            assert expected.lower() in words, (name, words)
        return words

    def artwork(images):
        strips = [im.crop((590,14,690,20)) for im in images]
        assert all(sum(max(p)>12 for p in im.getdata())>300 for im in strips), 'Artwork flashed black'
        assert any(ImageChops.difference(strips[0],im).getbbox() for im in strips[1:]), 'Artwork froze'

    def capture(name, trigger, updates=(), output='TEST-1'):
        before = shot(name+'-before',output)
        began = time.monotonic(); trigger()
        updates = iter(updates); pending = next(updates,None)
        frames = []
        for index, offset in enumerate((.02,.08,.16,.26,.4,.6,.85,1.2)):
            while pending and pending[0]<=offset:
                at(began,pending[0]); pending[1]()
                pending = next(updates,None)
            at(began,offset); frames.append(shot(f'{name}-{index}',output))
        artwork(frames)
        return before,frames

    def fades(name, before, frames, region=(515,20,790,44)):
        # Isolate pixels unique to each title. Animated artwork stays below the
        # white text threshold, so it cannot make an abrupt text swap pass.
        pixels = [list(im.crop(region).getdata()) for im in [before,*frames]]
        old, new = pixels[0],pixels[-1]
        old_mask = [i for i,(a,b) in enumerate(zip(old,new)) if min(a)>215 and max(b)<130]
        new_mask = [i for i,(a,b) in enumerate(zip(old,new)) if min(b)>215 and max(a)<130]
        assert len(old_mask)>8 and len(new_mask)>8, (name,len(old_mask),len(new_mask))

        def fraction(sample, mask, foreground, background):
            mean = lambda data: sum(sum(data[i])/3 for i in mask)/len(mask)
            return (mean(sample)-mean(background))/(mean(foreground)-mean(background))

        old_values = [fraction(p,old_mask,old,new) for p in pixels[1:]]
        new_values = [fraction(p,new_mask,new,old) for p in pixels[1:]]
        assert any(.15<v<.85 for v in old_values), (name,'Outgoing title snapped',old_values)
        assert any(.15<v<.85 for v in new_values), (name,'Incoming title snapped',new_values)
        assert old_values[-2]<.15 and new_values[-2]>.85, (name,'Fade did not settle',old_values,new_values)
        (out/(name+'-fade.json')).write_text(json.dumps(dict(outgoing=old_values,incoming=new_values),indent=2))

    def steady(name, before, frames, region=(585,20,780,44)):
        # A percentage, focus or duplicate result update must not dim the title.
        baseline = list(before.crop(region).getdata())
        mask = [i for i,p in enumerate(baseline) if min(p)>215]
        assert len(mask)>20, (name,'No settled title')
        for index, frame in enumerate(frames):
            values = list(frame.crop(region).getdata())
            ratio = sum(min(values[i]) for i in mask)/sum(min(baseline[i]) for i in mask)
            assert ratio>.9, (name,index,'Unchanged title faded',ratio)

    def activity(identity,status):
        if status=='running':
            msg('island-activity-start',identity,'Fixture transfer','download')
        elif status=='completed':
            msg('island-activity-update',identity,'100'); msg('island-activity-end',identity)
        else:
            msg('island-activity-update',json.dumps(dict(id=identity,status=status)))

    def configure(reduced=False,seconds=12,speed=.25):
        (cfg/'config.toml').write_text(original.replace('enabled=true\nspeed=0.25',
                                                       'enabled='+str(not reduced).lower()+'\nspeed=0.25')
                                     .replace('bluetooth_preview_seconds=12','bluetooth_preview_seconds='+str(seconds))
                                     .replace('speed=0.25','speed='+str(speed)))
        msg('config-reload'); time.sleep(3)

    ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
    env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_TITLE'] = 'Motion fixture'
    env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
    start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'player.log')
    time.sleep(3)
    change(bluetooth,Connected=True,Alias='Office headphones'); change(bluetooth,Percentage=75)
    time.sleep(3); text('initial-card','Office headphones')

    before,frames = capture('device-name',lambda:change(bluetooth,Alias='Studio headphones'))
    fades('device-name',before,frames); text('device-settled','Studio headphones')
    before,frames = capture('battery-update',lambda:change(bluetooth,Percentage=76))
    steady('battery-update',before,frames)

    before,frames = capture('burst',lambda:change(bluetooth,Alias='Kitchen headphones'),
                           ((.04,lambda:change(bluetooth,Alias='Library headphones')),
                            (.09,lambda:change(bluetooth,Percentage=77)),
                            (.13,lambda:change(bluetooth,Alias='Garden headphones'))))
    fades('burst',before,frames); text('burst-latest','Garden headphones')
    time.sleep(.4); text('burst-no-replay','Garden headphones')
    change(bluetooth,Connected=False); time.sleep(3)

    # An initially unnamed Wi-Fi link acquires its SSID without becoming a new
    # event. Its visible card should transition while preserving the deadline.
    change(network,kind='wired'); time.sleep(3)
    change(network,kind='wifi',ssid=''); time.sleep(1.8)
    before,frames = capture('network-name',lambda:change(network,ssid='Garden'))
    fades('network-name',before,frames); text('network-settled','Connected to')
    before,frames = capture('signal-update',lambda:change(network,strength=42))
    steady('signal-update',before,frames)

    activity('first','running'); activity('first','completed'); time.sleep(3)
    activity('second','running')
    before,frames = capture('duplicate-finish',lambda:activity('second','completed'))
    steady('duplicate-finish',before,frames)
    activity('third','running')
    before,frames = capture('transfer-result',lambda:activity('third','failed'))
    fades('transfer-result',before,frames,region=(560,27,790,55))
    text('transfer-settled','Transfer failed')

    # Force several different views to replace each other before their incoming
    # fade finishes. The final OSD must not inherit another fade's completion.
    msg('volume-osd','20'); time.sleep(.08)
    run(['gdbus','call','--session','--dest','org.freedesktop.Notifications','--object-path',
         '/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.Notify',
         'Mail','0','','Priority notice','Motion interruption','[]',"{'urgency': <byte 2>}",'0'])
    time.sleep(.05); msg('notification-clear-active'); msg('notification-clear-history')
    msg('volume-osd','65'); time.sleep(1.1)
    text('interruption-settled','65',region=(735,30,780,52))
    msg('island-activity-end','third')
    time.sleep(6)

    # Short previews can expire while a slow animation is still running.
    configure(seconds=1)
    change(bluetooth,Connected=True,Alias='Short card'); time.sleep(.3)
    change(bluetooth,Alias='Latest short card'); time.sleep(4)
    words = text('expiry-settled')
    assert 'short' not in words and any(c.isdigit() for c in words), ('Expired card did not return to the clock',words)
    assert not json.loads(msg('status'))['panelOpen']
    change(bluetooth,Connected=False)

    configure(reduced=True); msg('theme-mode-set','light')
    change(bluetooth,Connected=True,Alias='Office headphones'); time.sleep(.4)
    change(bluetooth,Alias='Studio headphones'); time.sleep(.08)
    first = shot('reduced-immediate'); time.sleep(.4)
    final = shot('reduced-settled')
    assert ImageChops.difference(first.crop((490,20,790,44)),final.crop((490,20,790,44))).getbbox() is None
    text('reduced-title','Studio headphones')
    text('fractional-title','Studio headphones','TEST-2')

    # Toggle reduced motion during an active fade and rebuild both surfaces.
    configure(); change(bluetooth,Alias='Library headphones'); time.sleep(.08)
    configure(reduced=True); text('reduced-mid-fade','Library headphones')
    # Large screencopies take longer; leave enough time between opacity levels
    # to sample the 150% output on slower GPUs too.
    change(bluetooth,Connected=False); configure(speed=.125)
    change(bluetooth,Connected=True,Alias='Office headphones'); time.sleep(6)
    before,frames = capture('fractional-fade',lambda:change(bluetooth,Alias='Studio headphones'),output='TEST-2')
    fades('fractional-fade',before,frames); text('fractional-settled','Studio headphones','TEST-2')
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: same-view device/network/result fades, coalesced bursts, steady metadata and identical results, '
          'live artwork, interrupted fades, expiry, reduced motion, reload and fractional scaling',flush=True)
