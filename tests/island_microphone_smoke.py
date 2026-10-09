"""Microphone Live Activity against real private PipeWire routes and two GPU outputs."""
import csv
import io
import json
import math
import os
import pathlib
import struct
import subprocess
import sys
import time
import wave

from PIL import Image, ImageChops

REPO = pathlib.Path(__file__).resolve().parents[1]
TESSDATA = os.environ.get('NOCTALIA_TEST_TESSDATA', str(REPO/'build-rishot/test-data/tessdata'))


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    path.write_text(path.read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
                    .replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    .replace('[osd.kinds]\n', '[osd.kinds]\nprivacy=false\nkeyboard_layout=false\n')+'''
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
    captures = []

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
        im = im.crop((0, 0, im.width, round(420*factor)))
        zoom = out/(name+'-text.png')
        im.resize((im.width*3, im.height*3)).getchannel('R').point(lambda p: 0 if p>90 else 255).save(zoom)
        result = run(['tesseract', str(zoom), 'stdout', '--tessdata-dir', TESSDATA, '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        rows = csv.DictReader(io.StringIO(result[result.index('level\t'):]), delimiter='\t')
        return [(r['text'].strip(), (int(r['left'])+int(r['width'])/2)/(3*factor),
                 (int(r['top'])+int(r['height'])/2)/(3*factor)) for r in rows if (r.get('text') or '').strip()]

    def text(name, output='TEST-1'):
        return ' '.join(w for w, x, y in words(name, output))

    def meters():
        return [node for node in json.loads(run(['pw-dump']))
                if node.get('info', {}).get('props', {}).get('application.name') == 'Noctalia Input Meter'
                and node['type'] == 'PipeWire:Interface:Node']

    def muted(name):
        return next(s['mute'] for s in json.loads(run(['pactl', '--format=json', 'list', 'sources'])) if s['name'] == name)

    def capture(name, app):
        proc = subprocess.Popen(['parec', '--device='+name, '--client-name='+app], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        captures.append(proc); time.sleep(.8)
        return proc

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100,600)
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark'); time.sleep(1)
        for name, label in (('live-mic', 'Studio'), ('other-mic', 'Headset')):
            run(['pactl', 'load-module', 'module-remap-source', 'source_name='+name,
                 'master=hyprland-test.monitor', 'source_properties=device.description='+label])
        run(['pactl', 'set-default-source', 'other-mic'])
        assert not meters(), 'Idle Island opened an input monitor'
        first = capture('live-mic', 'VoiceRoom')
        compact = shot('microphone-compact')
        orange = [(x,y) for x in range(450,830) for y in range(20,75)
                  if (lambda p: p[0]>160 and 75<p[1]<210 and p[2]<90)(compact.getpixel((x,y)))]
        assert orange, 'Orange microphone indicator missing'
        click(sum(x for x,y in orange)/len(orange), sum(y for x,y in orange)/len(orange))
        time.sleep(.5)
        content = text('microphone-open')
        assert all(s in content for s in ('Microphone', 'VoiceRoom', 'Studio', 'Live')), content
        wait(lambda:len(meters())==1, 'Input meter did not attach')
        assert meters()[0]['info']['props']['target.object'] == 'live-mic', meters()
        assert 'Headset' not in content and 'Noctalia' not in content, content

        # Feed an actual tone into the captured route. A silent meter must rise, then fall when muted.
        wav = base/'tone.wav'
        block = b''.join(struct.pack('<h', int(10000*math.sin(2*math.pi*440*n/48000))) for n in range(48000))
        with wave.open(str(wav), 'wb') as sound:
            sound.setparams((1,2,48000,0,'NONE','not compressed')); sound.writeframes(block*90)
        silent = shot('microphone-silent')
        tone = start(['paplay', '--device=hyprland-test', str(wav)], 'tone.log'); time.sleep(1)
        active = shot('microphone-level')
        meter_box = (465,145,730,162)
        assert ImageChops.difference(silent.crop(meter_box),active.crop(meter_box)).getbbox(), 'Input meter stayed silent'
        click(787,130)
        wait(lambda:muted('live-mic'), 'Mute button did not mute the actual captured source')
        assert not muted('other-mic'), 'Mute changed an unrelated default source'
        assert 'Muted' in text('microphone-muted')
        wait(lambda:not meters(), 'Muted input kept monitoring')
        click(787,130); wait(lambda:not muted('live-mic'), 'Unmute button failed')
        assert 'Live' in text('microphone-unmuted')

        # External mute updates in place, without a temporary volume card stealing the controls.
        run(['pactl','set-source-mute','live-mic','1']); time.sleep(.6)
        assert 'Muted' in text('microphone-external-mute')
        run(['pactl','set-source-mute','live-mic','0']); time.sleep(.6)

        # Two captures with the same app name keep both routed microphones and their controls.
        second = capture('other-mic', 'VoiceRoom')
        content = text('microphone-two-inputs')
        assert 'Studio' in content and 'Headset' in content, content
        wait(lambda:len(meters())==2, 'Second input meter missing')
        second.terminate(); second.wait(timeout=5); time.sleep(.7)
        assert 'Headset' not in text('microphone-input-ended')
        wait(lambda:len(meters())==1, 'Ended source remained monitored')

        stream = next(s['index'] for s in json.loads(run(['pactl','--format=json','list','source-outputs']))
                      if s['properties'].get('application.name') == 'VoiceRoom')
        run(['pactl','move-source-output',str(stream),'other-mic']); time.sleep(.8)
        content = text('microphone-rerouted')
        assert 'Headset' in content and 'Studio' not in content, content
        run(['pactl','set-source-mute','other-mic','1']); time.sleep(.6)
        assert 'Muted' in text('microphone-default-muted')
        run(['pactl','set-source-mute','other-mic','0'])
        run(['pactl','move-source-output',str(stream),'live-mic']); time.sleep(.8)
        assert 'Studio' in text('microphone-route-returned')

        click(798,43)
        assert 'Output' in text('microphone-audio-panel')
        wait(lambda:not meters(), 'Hosted panel kept the hidden input meter running')
        msg('panel-close'); move(1100,600); time.sleep(.8); move(610,40); time.sleep(.8)
        assert 'VoiceRoom' in text('microphone-panel-return')

        # Closing the view releases passive monitors, while the external capture continues.
        move(1100,600); time.sleep(1)
        wait(lambda:not meters(), 'Collapsed Island kept monitoring')
        move(610,40); time.sleep(1)
        assert 'VoiceRoom' in text('microphone-hover-open')

        # On the scaled/rotated output the same card, route, mute and escape actions work.
        move(1100,600); time.sleep(.8); move(610,760); time.sleep(1)
        assert 'VoiceRoom' in text('microphone-scaled', 'TEST-2')
        click(787,850); wait(lambda:muted('live-mic'), 'Scaled mute failed')
        assert 'Muted' in text('microphone-scaled-muted', 'TEST-2')
        click(787,850); wait(lambda:not muted('live-mic'), 'Scaled unmute failed')
        move(1100,1320); time.sleep(.8)
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(610,40); time.sleep(.7)
        move(1100,600); time.sleep(.8)
        msg('island-focus'); time.sleep(.7)
        assert 'VoiceRoom' in text('microphone-keyboard')
        send(keyboard, '1'); time.sleep(.7)
        assert 'VoiceRoom' not in text('microphone-escaped')
        wait(lambda:not meters(), 'Escape kept monitoring')

        # Media keeps its moving artwork while the microphone card is selected.
        env['ISLAND_TEST_ART'] = (REPO/'assets/noctalia-wallpaper.png').as_uri()
        player = start([sys.executable, str(REPO/'tests/fixtures/island_player.py')], 'player.log'); time.sleep(1.5)
        move(610,40); time.sleep(1); click(728,35)
        assert 'VoiceRoom' in text('microphone-with-media')
        a = shot('microphone-art-before'); time.sleep(.4); b = shot('microphone-art-after')
        strip = (580,14,690,20)
        assert sum(max(p)>12 for p in b.crop(strip).getdata()) > 300, 'Microphone covered media artwork'
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(), 'Microphone froze media artwork'
        run(['notify-send','-u','critical','Urgent alert','Microphone activity yields to urgent notifications'])
        time.sleep(.5); assert 'Urgent' in text('microphone-urgent')
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.7)
        assert 'VoiceRoom' in text('microphone-restored')
        config = cfg/'config.toml'; original = config.read_text()
        config.write_text(original+'\n[shell.animation]\nenabled=false\n'); msg('config-reload'); time.sleep(.8)
        move(1100,600); time.sleep(.8); move(610,40); time.sleep(.8); click(728,35)
        assert 'VoiceRoom' in text('microphone-reduced-motion')
        config.write_text(original+'\n[shell.privacy]\nmic_filter_regex="VoiceRoom"\n')
        msg('config-reload'); time.sleep(.9)
        assert 'VoiceRoom' not in text('microphone-filtered')
        wait(lambda:not meters(), 'Filtered capture kept monitoring')
        config.write_text(original); msg('config-reload'); time.sleep(.8)
        move(1100,600); time.sleep(.8); move(610,40); time.sleep(.8); click(728,35)
        assert 'VoiceRoom' in text('microphone-unfiltered')
        first.terminate(); first.wait(timeout=5); time.sleep(1)
        content = text('microphone-ended')
        assert 'VoiceRoom' not in content and 'Microphone' not in content, content
        wait(lambda:not meters(), 'Activity kept itself alive after capture stopped')
        assert shell.poll() is None
        print('PASS: microphone Live Activity, routed mute, real levels, multiple inputs, scaled output, keyboard, media and teardown', flush=True)
    finally:
        for proc in captures:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
        pointer.terminate(); pointer.wait(timeout=5)
        keyboard.terminate(); keyboard.wait(timeout=5)
