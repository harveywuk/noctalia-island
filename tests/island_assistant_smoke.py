"""Native assistant checks with a loopback AI and an isolated compositor/audio session."""
import csv
import http.server
import io
import json
import math
import struct
import wave
import os
import pathlib
import subprocess
import sys
import threading
import time

from PIL import Image, ImageChops, ImageStat

requests = []
server = None
strata_mode = ""
strata_started = 0.0
status_requests = []


class Backend(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        status_requests.append(time.monotonic())
        if strata_mode == 'unavailable':
            self.send_response(404); self.end_headers(); return
        if strata_mode == 'slow-status':
            time.sleep(1.5)
        data = {'service': 'strata', 'loaded': strata_mode != 'loading' or bool(strata_started and time.monotonic()-strata_started > 2),
                'activity': {'in_flight': 1 if strata_mode == 'waiting' else 0}, 'concurrency': {'serving': 1}}
        self.send_response(200); self.end_headers()
        try: self.wfile.write(json.dumps(data).encode())
        except (BrokenPipeError, ConnectionResetError): pass

    def do_POST(self):
        global strata_started
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        requests.append(payload)
        question = payload['messages'][-1]['content']
        if self.path == '/v1/chat/completions':
            strata_started = time.monotonic()
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
            try:
                self.wfile.write(b'data: {"choices":[{"delta":{"role":"assistant","content":""}}]}\n\n');self.wfile.flush()
                time.sleep(3)
                self.wfile.write(b'data: {"choices":[{"delta":{"reasoning_content":"Fixture reasoning"}}]}\n\n');self.wfile.flush()
                time.sleep(2)
                self.wfile.write(b'data: {"choices":[{"delta":{"content":"A quiet answer from Strata."},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n');self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError): pass
            return
        self.send_response(503 if question == 'fail' else 200)
        self.send_header('Content-Type', 'application/x-ndjson')
        self.end_headers()
        try:
            if question == 'fail':
                self.wfile.write(b'{"error":"Fixture unavailable"}\n')
                return
            time.sleep(4 if question == 'slow' else .65)
            if question == 'long':
                answer = '\n\n'.join(f'{n}. A readable paragraph that wraps naturally inside the Island.' for n in range(1, 45))
            elif question == 'follow':
                answer = 'Context preserved.' if len(payload['messages']) == 3 else 'Context missing.'
            elif question == 'empty':
                answer = ''
            else:
                answer = 'A quiet answer from the local fixture.'
            for part in [answer[:len(answer)//2], answer[len(answer)//2:]]:
                self.wfile.write((json.dumps({'message': {'content': part}, 'done': False})+'\n').encode())
                self.wfile.flush()
                time.sleep(.2)
            self.wfile.write(b'{"message":{"content":""},"done":true}\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


def prepare(base, cfg, env):
    global server
    if os.environ.get('NOCTALIA_ASSISTANT_PROFILE_ONLY'):
        env['NOCTALIA_IDLE_PROFILE'] = '1'
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Backend)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    path = cfg/'config.toml'
    hypr = base/'hyprland.lua'
    command = str(base/'noctalia-under-test')+' msg assistant-voice '
    hypr.write_text(hypr.read_text() + '\nhl.bind("CTRL + ALT + Space", hl.dsp.exec_cmd('+json.dumps(command+'start')+'), {dont_inhibit=true})\n'
                   'hl.bind("CTRL + ALT + Space", hl.dsp.exec_cmd('+json.dumps(command+'finish')+'), {release=true,dont_inhibit=true,ignore_mods=true,non_consuming=true})\n')
    path.write_text(path.read_text().replace('hover_widgets=["workspaces","taskbar"]',
        'hover_widgets=[]\ntrack_preview_seconds=0') + '\n[shell.launcher.ai]\nprovider="ollama"\nmodel="fixture"\nurl="http://127.0.0.1:' + str(server.server_port) + '"\n[shell.animation]\nenabled=false\n')
    plugin = base/'plugins/timer'
    plugin.mkdir(parents=True)
    repo = pathlib.Path(__file__).resolve().parents[1]
    (plugin/'plugin.toml').write_text('id="noctalia/timer"\nname="Assistant timer fixture"\nversion="1.0.0"\nplugin_api=3\n[[service]]\nid="timer"\nentry="service.luau"\n')
    (plugin/'service.luau').write_text((repo/'tests/fixtures/assistant_timer.luau').read_text())
    path.write_text(path.read_text().replace('auto_update="none"', 'auto_update="none"\nenabled=["noctalia/timer"]').replace(
        'location="/nonexistent"\nenabled=false', 'location='+json.dumps(str(plugin.parent))+'\nenabled=true'))
    fixtures = base/'voice-fixtures'
    fixtures.mkdir()
    (fixtures/'model.bin').write_text('fixture')
    (fixtures/'voice.onnx').write_text('fixture')
    (fixtures/'voice.onnx.json').write_text('{}')
    whisper = fixtures/'whisper'
    whisper.write_text("""#!/usr/bin/env python3
import pathlib,sys,time,wave
args=sys.argv
with wave.open(args[args.index('-f')+1]) as audio:
    assert audio.getframerate()==16000 and audio.getnchannels()==1 and audio.getnframes()>1600
time.sleep(.8)
transcript=pathlib.Path(__file__).with_name('transcript.txt')
pathlib.Path(args[args.index('-of')+1]+'.txt').write_text(transcript.read_text() if transcript.exists() else 'hello')
""")
    piper = fixtures/'piper'
    piper.write_text("""#!/usr/bin/env python3
import math,pathlib,struct,sys,time,wave
args=sys.argv
text=pathlib.Path(args[args.index('--input-file')+1]).read_text()
assert 'quiet answer' in text or text in ('Work Focus on.', 'No timer is active.')
pathlib.Path(__file__).with_name('spoken.txt').write_text(text)
time.sleep(.6)
with wave.open(args[args.index('-f')+1],'wb') as audio:
    audio.setnchannels(1);audio.setsampwidth(2);audio.setframerate(24000)
    audio.writeframes(b''.join(struct.pack('<h', int(7000*math.sin(i*2*math.pi*220/24000))) if 1.8 <= i/24000 < 4.2 else b'\\0\\0' for i in range(24000*7)))
""")
    whisper.chmod(0o700);piper.chmod(0o700)
    path.write_text(path.read_text() + '\n[shell.launcher.ai.voice]\nenabled=true\nwhisper_command='+json.dumps(str(whisper))+'\nwhisper_model='+json.dumps(str(fixtures/'model.bin'))+'\npiper_command='+json.dumps(str(piper))+'\npiper_model='+json.dumps(str(fixtures/'voice.onnx'))+'\n')
    # Keep credentials out of the private test environment, including compatible APIs.
    for name in ('OPENAI_API_KEY', 'ANTHROPIC_API_KEY', 'OLLAMA_API_KEY'):
        env.pop(name, None)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, battery):
    global strata_mode, strata_started
    if os.environ.get('NOCTALIA_ASSISTANT_PROFILE_ONLY'):
        from assistant_performance_smoke import run_checks as profile
        try:
            return profile(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell)
        finally:
            server.shutdown();server.server_close()
    repo = pathlib.Path(__file__).resolve().parents[1]
    tessdata = os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata'))
    helpers = []
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
             *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
    pointer, keyboard = helpers
    original = (cfg/'config.toml').read_text()
    backgrounds = {}

    def send(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def key(value):
        send(keyboard, value); time.sleep(.15)

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        send(pointer, 'relative 1 0'); send(pointer, 'relative -1 0'); time.sleep(.15)

    def click(x, y):
        move(x, y); send(pointer, 'press'); time.sleep(.08); send(pointer, 'release'); time.sleep(.3)

    def shot(name, output='TEST-1'):
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)]); return Image.open(path).convert('RGB')

    def words(name, output='TEST-1'):
        im = shot(name, output)
        output_scale = im.width / 1280
        inset = round(im.width / 2 - 300 * output_scale)
        area = (inset, 0, round(im.width / 2 + 300 * output_scale), im.height)
        difference = ImageChops.difference(im, backgrounds[output]).crop(area)
        bounds = difference.point(lambda p: 255 if p > 24 else 0).getbbox()
        assert bounds, 'Panel did not change the output'
        left, top, right, bottom = bounds
        left += inset; right += inset
        cropped = im.crop((left, top, right, bottom))
        factor = 3
        path = out/(name+'-ocr.png'); cropped.resize((cropped.width*factor, cropped.height*factor)).save(path)
        result = run(['tesseract', str(path), 'stdout', '--tessdata-dir', tessdata, '--psm', '11', '-c', 'tessedit_create_tsv=1'])
        rows = csv.DictReader(io.StringIO(result[result.index('level\t'):]), delimiter='\t', quoting=csv.QUOTE_NONE)
        return [(r['text'].strip(), left+(int(r['left'])+int(r['width'])/2)/factor,
                 top+(int(r['top'])+int(r['height'])/2)/factor) for r in rows if (r.get('text') or '').strip()]

    def text(name, output='TEST-1'):
        return ' '.join(w for w, x, y in words(name, output))

    def open_question(question=''):
        msg('panel-open', 'assistant', question); time.sleep(.35)

    def submit(question):
        open_question(question); key(28)
        wait(lambda: requests and requests[-1]['messages'][-1]['content'] == question, 'Question was not submitted')

    def new():
        key('chord 2 49')  # Ctrl+N
        time.sleep(.2)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 600)
        backgrounds['TEST-1'] = shot('baseline')
        backgrounds['TEST-2'] = shot('baseline-second', 'TEST-2')
        run(['pactl', 'set-default-source', 'hyprland-test.monitor'])
        ctl('reload');time.sleep(.3)
        open_question();new();key(1)
        before = len(requests)
        key('hold 6 57')
        wait(lambda: json.loads(msg('assistant-voice', 'status'))['voice']=='recording', 'Hold shortcut did not record')
        msg('assistant-voice', 'start')  # Repeated press must not stop or restart recording.
        time.sleep(.5)
        key('release 0 57')  # Modifiers can be released before Space.
        wait(lambda: json.loads(msg('assistant-voice', 'status'))['voice']=='idle', 'Release did not finish transcription')
        assert 'hello' in text('hold-review') and len(requests)==before
        assert not json.loads(msg('assistant-voice', 'status'))['held']
        key(1)
        msg('assistant-voice', 'finish');time.sleep(.2)
        assert not json.loads(msg('status'))['panelOpen'], 'Late release reopened the assistant'
        key('hold 6 57');time.sleep(.4);key(1);key('release 0 57');time.sleep(.3)
        assert not json.loads(msg('status'))['panelOpen'], 'Release after Escape reopened the card'
        assert json.loads(msg('assistant-voice', 'status'))['voice']=='idle'
        msg('assistant-voice', 'finish');msg('assistant-voice', 'start');time.sleep(.3)
        assert json.loads(msg('assistant-voice', 'status'))['voice']=='idle', 'Out-of-order quick tap began capture'
        key(1)
        open_question();new();key(1)
        open_question()
        assert not requests, 'Opening the assistant sent a request'
        assert 'Noctalia' not in text('ready'), 'Idle card kept a redundant title'
        assert json.loads(msg('status'))['activePanelId'] == 'assistant'
        open_question('draft'); key(1)
        assert not json.loads(msg('status'))['panelOpen'], 'Escape did not release the panel'
        open_question()
        assert 'draft' in text('draft-restored') and not requests
        submit('hello')
        time.sleep(1.3)
        assert 'quiet answer' in text('answer'), text('answer-failure')
        key('chord 3 46')  # Ctrl+Shift+C copies the answer without selecting its text.
        wait(lambda: 'quiet answer' in msg('clipboard-text'), 'Copy did not reach the clipboard')
        click(860, 48)
        assert not json.loads(msg('status'))['panelOpen'], 'Close button did not dismiss the answer'
        move(1100, 600)
        def local_action(question, expected, name):
            count = len(requests)
            open_question(question); key(28); time.sleep(.3)
            assert expected.lower() in text(name).lower(), text(name+'-failure')
            assert len(requests) == count, 'Local action reached the model'

        local_action('Set volume to thirty percent.', 'Volume set to 30%', 'action-volume')
        sink = json.loads(run(['pactl', '--format=json', 'list', 'sinks']))[0]
        assert all(abs(float(channel['value_percent'].rstrip('%'))-30)<2 for channel in sink['volume'].values())
        # Restore the private monitor's gain before the existing audio-reactivity checks.
        run(['pactl', 'set-sink-volume', 'hyprland-test', '100%'])
        local_action('Set volume to 130%', '0 to 100', 'action-volume-invalid')
        local_action('Enable Focus', 'Work Focus on', 'action-focus')
        assert json.loads(msg('focus-status'))['mode'] == 'work'
        local_action('Which Focus is active?', 'Work Focus on', 'query-focus-work')
        local_action('Disable Focus', 'Focus off', 'action-focus-off')
        assert json.loads(msg('focus-status'))['mode'] == 'off'
        local_action('Which Focus is active?', 'Focus off', 'query-focus-off')
        local_action("What's my battery level?", 'No system battery', 'query-battery-none')
        send(battery, json.dumps({'IsPresent':True, 'Percentage':65., 'State':2}));time.sleep(.3)
        local_action("What's my battery level?", 'Battery at 65%', 'query-battery')
        send(battery, json.dumps({'Percentage':42., 'State':1}));time.sleep(.3)
        local_action('How much battery is left?', '42%, charging', 'query-battery-charging')
        send(battery, json.dumps({'IsPresent':False}));time.sleep(.3)
        local_action("What's playing?", 'No media player', 'query-media-none')
        local_action('Pause the music', 'No media player', 'action-no-player')
        local_action('How long is left?', 'No timer is active', 'query-timer-none')
        local_action('Resume the timer', 'No timer is active', 'action-resume-no-timer')
        local_action('Set a timer for ten minutes.', 'Timer set for 10 minutes', 'action-timer')
        state_path = base/'plugins/timer/test-state.json'
        wait(lambda: state_path.exists() and json.loads(state_path.read_text())['state']=='RUNNING', 'Timer did not start')
        timer = json.loads(state_path.read_text())
        assert timer['duration'] == 600 and timer['starts'] == 1
        wait(lambda: not json.loads(msg('status'))['panelOpen'], 'Timer did not hand back to the Island')
        shot('action-timer-handoff')
        local_action('How long is left?', '10 minutes remaining', 'query-timer-running')
        local_action('Pause the timer', 'Timer paused', 'action-timer-pause')
        wait(lambda: json.loads(state_path.read_text())['state']=='PAUSED', 'Timer did not pause')
        paused = json.loads(state_path.read_text())
        local_action('Pause the timer', 'Timer paused', 'action-timer-pause-again')
        assert json.loads(state_path.read_text())['commands']==paused['commands'], 'Repeated pause sent another command'
        local_action('How much time is left?', 'paused with 10 minutes remaining', 'query-timer-paused')
        msg('plugin', 'noctalia/timer:timer', 'all', 'remaining', '95');time.sleep(.2)
        local_action('How long is left?', '1 minute 35 seconds remaining', 'query-timer-live')
        local_action('Resume the timer', 'Timer resumed', 'action-timer-resume')
        wait(lambda: json.loads(state_path.read_text())['state']=='RUNNING', 'Timer did not resume')
        resumed = json.loads(state_path.read_text())
        assert resumed['duration']==600 and resumed['remaining']==95 and resumed['starts']==1
        local_action('Resume the timer', 'Timer resumed', 'action-timer-resume-again')
        assert json.loads(state_path.read_text())['commands']==resumed['commands'], 'Repeated resume restarted the timer'
        local_action('Set a timer for five minutes', 'already active', 'action-timer-preserved')
        assert json.loads(state_path.read_text())['duration'] == 600
        local_action('Set a timer for ten minutes and pause the music', 'timer duration', 'action-compound-rejected')
        assert json.loads(state_path.read_text())['starts'] == 1
        local_action('Cancel the timer', 'Timer cancelled', 'action-timer-cancel')
        assert json.loads(state_path.read_text())['state']=='IDLE'
        local_action('Cancel the timer', 'No timer is active', 'action-timer-cancel-again')
        open_question('Start a five minute timer'); key(28)
        wait(lambda: json.loads(state_path.read_text())['starts']==2, 'Second timer did not start')
        key(30)  # Begin a follow-up while the confirmation is showing.
        time.sleep(1.5)
        assert json.loads(msg('status'))['activePanelId']=='assistant', 'Timer dismissed a new draft'
        msg('plugin', 'noctalia/timer:timer', 'all', 'finish');time.sleep(.25)
        local_action('How long is left?', 'timer has finished', 'query-timer-finished')
        ended = json.loads(state_path.read_text())
        local_action('Resume the timer', 'timer has finished', 'action-timer-resume-finished')
        assert json.loads(state_path.read_text())['commands']==ended['commands'], 'Resume changed a completed timer'
        local_action('Cancel the timer', 'Timer cancelled', 'action-timer-clear-finished')
        submit('follow'); time.sleep(1.3)
        assert len(requests[-1]['messages']) == 3, requests[-1]
        assert 'Context preserved' in text('follow-up')
        new(); submit('slow'); key('chord 2 52')  # Ctrl+period
        assert 'Stopped' in text('stopped')
        # A cancelled stream must never overwrite a newer reply.
        submit('hello'); time.sleep(4.2)
        assert requests[-1]['messages'] == [{'role': 'user', 'content': 'hello'}]
        assert 'quiet answer' in text('cancelled-request-stays-cancelled')
        new(); submit('slow'); key(1); time.sleep(.3)
        open_question(); assert 'Stopped' in text('dismiss-stops-request')
        new(); submit('fail'); time.sleep(.5)
        move(1100, 600)
        error_words = words('unavailable')
        assert 'fail' in ' '.join(w for w, x, y in error_words)
        assert 'Settings' in ' '.join(w for w, x, y in error_words)
        retry = next((x, y) for w, x, y in error_words if w == 'Try')
        before = len(requests); click(*retry)
        wait(lambda: len(requests) == before + 1, 'Retry did not submit')
        assert requests[-1]['messages'] == [{'role': 'user', 'content': 'fail'}]
        new(); submit('empty'); time.sleep(1.3)
        assert 'empty answer' in text('empty-response')
        new(); submit('long'); time.sleep(1.3)
        long_words = words('long-answer')
        assert 'paragraph' in ' '.join(w for w, x, y in long_words)
        # Follow-up field and close affordance remain inside the output for long answers.
        assert any('follow-up' in w and y < 680 for w, x, y in long_words)
        move(650, 200); send(pointer, 'scroll 15'); time.sleep(.4)
        shot('long-answer-scrolled')
        key(1)

        # Every capture/playback client uses the private PipeWire server and null sink.
        run(['pactl', 'set-default-source', 'hyprland-test.monitor'])
        open_question(); new()
        before = len(requests)
        msg('panel-open', 'assistant', '/dictate'); time.sleep(.7)
        assert 'Listening' in text('voice-listening')
        key('chord 2 32')  # Ctrl+D finishes capture, never sends the text.
        assert 'Transcribing' in text('voice-transcribing')
        time.sleep(1)
        assert 'hello' in text('voice-review') and len(requests) == before
        key(28)
        wait(lambda: len(requests) == before+1, 'Dictated text was not submitted')
        wait(lambda: (base/'voice-fixtures/spoken.txt').exists(), 'Dictated reply did not reach Piper')
        time.sleep(.8)
        assert 'Speaking' in text('voice-speaking')
        key(1)  # Escape dismisses the card and cancels spoken playback.
        assert not json.loads(msg('status'))['panelOpen'], 'Escape did not dismiss spoken playback'
        wait(lambda: not list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*')), 'Voice temporary files survived Escape')
        open_question(); new(); key('chord 2 32'); time.sleep(.6); key(1)
        wait(lambda: not list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*')), 'Voice capture survived dismiss')
        open_question(); new(); key('chord 2 32'); time.sleep(.6)
        key('chord 2 32'); key('chord 2 52'); time.sleep(1.2)
        assert 'hello' not in text('voice-cancelled-transcription'), 'Cancelled transcription changed the draft'
        key(1)

        # Dictated actions stay drafts until Enter and speak their local confirmation.
        transcript = base/'voice-fixtures/transcript.txt'
        spoken = base/'voice-fixtures/spoken.txt'
        transcript.write_text('Enable Focus'); spoken.unlink(missing_ok=True)
        before = len(requests)
        open_question(); new(); key('chord 2 32'); time.sleep(.6)
        key('chord 2 32'); time.sleep(1.2)
        assert 'Enable Focus' in text('action-voice-review')
        assert json.loads(msg('focus-status'))['mode']=='off', 'Dictation executed before Enter'
        assert len(requests)==before
        key(28)
        wait(lambda: spoken.exists(), 'Action confirmation did not reach Piper')
        assert spoken.read_text()=='Work Focus on.'
        assert json.loads(msg('focus-status'))['mode']=='work' and len(requests)==before
        key(1)
        wait(lambda: not list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*')), 'Action speech survived Escape')
        transcript.write_text('How long is left?'); spoken.unlink()
        open_question(); new(); key('chord 2 32'); time.sleep(.6)
        key('chord 2 32'); time.sleep(1.2)
        assert len(requests)==before and not spoken.exists(), 'Status dictation ran before Enter'
        key(28)
        wait(lambda: spoken.exists(), 'Timer status did not reach Piper')
        assert spoken.read_text()=='No timer is active.' and len(requests)==before
        key(1)
        wait(lambda: not list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*')), 'Status speech survived Escape')
        transcript.unlink(); msg('focus-set', 'off')

        # Exercise the orb with real private PipeWire capture and playback, never host audio.
        animated = original.replace('[shell.animation]\nenabled=false', '[shell.animation]\nenabled=true')
        (cfg/'config.toml').write_text(animated); msg('config-reload'); time.sleep(.4)
        open_question(); new()
        orb_area = (576, 24, 704, 156)
        edge_area = (460, 2, 820, 10)
        idle_a = shot('orb-ambient-a'); time.sleep(1.2)
        idle_b = shot('orb-ambient-b')
        assert sum(ImageStat.Stat(ImageChops.difference(idle_a.crop(orb_area), idle_b.crop(orb_area))).mean) > .5, 'Idle smoke did not move'
        assert sum(ImageStat.Stat(ImageChops.difference(idle_a.crop(edge_area), idle_b.crop(edge_area))).mean) > .5, 'Island gradient did not move'
        # Outside the glass, the halo follows only audio amplitude while smoke moves inside.
        def speech_halo(size):
            mask = Image.new('L', (orb_area[2]-orb_area[0], orb_area[3]-orb_area[1]))
            for y in range(mask.height):
                for x in range(mask.width):
                    radius2 = (x + orb_area[0] + .5 - 640)**2 + (y + orb_area[1] + .5 - (32+size/2))**2
                    if (size*21/48)**2 <= radius2 <= (size*23/48)**2:
                        mask.putpixel((x, y), 255)
            return mask
        halo_mask = speech_halo(112)
        key('chord 2 32'); time.sleep(.7)
        quiet_orb = shot('orb-input-silent').crop(orb_area)
        tone_file = base/'input-tone.wav'
        with wave.open(str(tone_file), 'wb') as audio:
            audio.setnchannels(1);audio.setsampwidth(2);audio.setframerate(16000)
            audio.writeframes(b''.join(struct.pack('<h', int(7000*math.sin(i*2*math.pi*220/16000))) for i in range(16000*4)))
        tone = start(['pw-play', str(tone_file)], 'orb-input-tone.log')
        time.sleep(.7)
        loud_orb = shot('orb-input-active').crop(orb_area)
        assert sum(ImageStat.Stat(ImageChops.difference(quiet_orb, loud_orb), halo_mask).mean) > .75, 'Orb did not react to microphone samples'
        assert len(json.loads(run(['pactl', '--format=json', 'list', 'source-outputs']))) == 1, 'Orb opened an extra capture stream'
        tone.terminate();tone.wait(timeout=3);time.sleep(1.5)
        still_a = shot('orb-input-settled').crop(orb_area);time.sleep(.3)
        still_b = shot('orb-input-still').crop(orb_area)
        assert sum(ImageStat.Stat(ImageChops.difference(still_a, still_b), halo_mask).mean) < 1.0, 'Speech halo kept pulsing during silence'
        key('chord 2 32'); time.sleep(1.2); key(28)
        wait(lambda: bool(list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*/reply.wav'))), 'Reply audio missing')
        time.sleep(.6)
        halo_mask = speech_halo(96)
        silent_output = shot('orb-output-silent').crop(orb_area)
        time.sleep(1.7)
        speaking_output = shot('orb-output-active').crop(orb_area)
        assert sum(ImageStat.Stat(ImageChops.difference(silent_output, speaking_output), halo_mask).mean) > .75, 'Orb did not react to spoken reply'
        key('chord 2 52');time.sleep(1.5)
        wait(lambda: not list(pathlib.Path(env['XDG_RUNTIME_DIR']).glob('noctalia-voice-*')), 'Reply audio survived Stop')
        stopped_a = shot('orb-output-stopped').crop(orb_area);time.sleep(.7)
        stopped_b = shot('orb-output-ambient').crop(orb_area)
        assert sum(ImageStat.Stat(ImageChops.difference(stopped_a, stopped_b)).mean) > .25, 'Ambient smoke stopped with speech'
        key(1)

        (cfg/'config.toml').write_text(original);msg('config-reload');time.sleep(.3)
        open_question();new();key('chord 2 32')
        tone = start(['pw-play', str(tone_file)], 'orb-reduced-motion-tone.log');time.sleep(.7)
        reduced_a=shot('orb-reduced-motion-a');time.sleep(.35)
        reduced_b=shot('orb-reduced-motion-b')
        assert sum(ImageStat.Stat(ImageChops.difference(reduced_a.crop(orb_area),reduced_b.crop(orb_area))).mean) < .1, 'Reduced-motion orb animated'
        assert sum(ImageStat.Stat(ImageChops.difference(reduced_a.crop(edge_area),reduced_b.crop(edge_area))).mean) < .1, 'Reduced-motion Island glow animated'
        assert 'Listening' in text('orb-reduced-motion-listening')
        tone.terminate();tone.wait(timeout=3);key(1)

        # Strata status is advisory, bounded, and never probes while the panel is idle.
        local = original.replace('provider="ollama"', 'provider="openai"\napi_key="fixture"').replace(
            f'url="http://127.0.0.1:{server.server_port}"', f'url="http://127.0.0.1:{server.server_port}/v1"')
        (cfg/'config.toml').write_text(local);msg('config-reload');time.sleep(.3)
        count = len(status_requests);open_question();time.sleep(.4)
        assert len(status_requests) == count, 'Idle panel polled the model'
        for mode, expected in [('loading', 'Loading model'), ('waiting', 'Waiting for Strata'), ('unavailable', 'Thinking'), ('slow-status', 'Thinking')]:
            strata_mode=mode;strata_started=0.0;new();submit('local status '+mode);time.sleep(.3)
            assert expected.lower() in text('strata-'+mode).lower(), text('strata-'+mode+'-failure')
            if mode in ('loading', 'waiting'):
                time.sleep(3)
                assert 'thinking' in text('strata-'+mode+'-thinking').replace(' ', '').lower(), 'Own reasoning did not clear waiting status'
            time.sleep(5 if mode in ('unavailable','slow-status') else 2.3)
            assert 'quiet answer from Strata' in text('strata-'+mode+'-answer')
            count=len(status_requests);time.sleep(.9)
            assert len(status_requests)==count, 'Status polling survived completion'
        strata_mode='slow-status';new();before=len(requests)
        open_question('cancel before submission');key(28);key('chord 2 52');time.sleep(1.7)
        assert len(requests)==before, 'Cancelled status preflight submitted the question'
        assert 'Stopped' in text('strata-cancelled-preflight')
        key(1);strata_mode='';(cfg/'config.toml').write_text(original);msg('config-reload');time.sleep(.3)

        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Media stays with you'
        env['ISLAND_TEST_EVENTS'] = str(out/'media-events.log')
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        time.sleep(1)
        local_action("What's playing?", 'Playing Media stays with you by Orbit', 'query-track-playing')
        local_action('Pause the music', 'Media paused', 'action-pause')
        wait(lambda: (out/'media-events.log').exists() and 'Pause' in (out/'media-events.log').read_text(), 'Pause did not reach the player')
        local_action("What's playing?", 'Paused: Media stays with you', 'query-track-paused')
        local_action('Resume the music', 'Media resumed', 'action-resume')
        wait(lambda: 'Play' in (out/'media-events.log').read_text(), 'Play did not reach the player')
        # A real private media stream is ducked, while master volume and manual adjustments survive.
        duck_config = original.replace('whisper_command=', 'duck_media=true\nwhisper_command=')
        (cfg/'config.toml').write_text(duck_config);msg('config-reload');time.sleep(.3)
        music_path = base/'duck-music.wav'
        with wave.open(str(tone_file), 'rb') as source, wave.open(str(music_path), 'wb') as dest:
            dest.setparams(source.getparams());dest.writeframes(source.readframes(source.getnframes())*12)
        music = start(['pw-play', '--properties', 'application.id=test application.name=MusicFixture', str(music_path)], 'duck-music.log')
        def music_stream():
            return next((s for s in json.loads(run(['pactl','--format=json','list','sink-inputs']))
                         if s['properties'].get('application.id')=='test'), None)
        def music_level():
            stream = music_stream()
            return float(next(iter(stream['volume'].values()))['value_percent'].rstrip('%')) if stream else -1
        wait(lambda: music_stream() is not None, 'Private music stream did not appear')
        open_question();new();msg('assistant-voice','start')
        wait(lambda: 43 <= music_level() <= 47, 'Music did not duck')
        sink = json.loads(run(['pactl','--format=json','list','sinks']))[0]
        assert all(channel['value_percent']=='100%' for channel in sink['volume'].values()), 'Ducking changed master volume'
        key(1)
        wait(lambda: music_level()>=99, 'Escape did not restore music')
        open_question();msg('assistant-voice','start')
        wait(lambda: 43 <= music_level() <= 47, 'Second capture did not duck')
        time.sleep(.6)
        run(['pactl','set-sink-input-volume',str(music_stream()['index']),'65%']);time.sleep(.6)
        key(1);time.sleep(.5)
        assert abs(music_level()-65)<2, 'Restore overwrote a manual volume change'
        music.terminate();music.wait(timeout=3)
        (cfg/'config.toml').write_text(original);msg('config-reload');time.sleep(.3)
        for compact in (False, True):
            (cfg/'config.toml').write_text(original.replace('track_preview_seconds=0',
                'track_preview_seconds=0\ncompact_layout='+str(compact).lower()).replace('[shell.animation]\nenabled=false', '[shell.animation]\nenabled=true'))
            msg('config-reload'); time.sleep(.4)
            for mode in ('dark', 'light'):
                msg('theme-mode-set', mode)
                open_question(); new()
                name=('compact' if compact else 'comfortable')+'-'+mode
                assert 'Media stays with you' in text(name)
                if mode == 'dark':
                    a=shot(name+'-flow-a').crop((470, 8, 810, 40)); time.sleep(.5)
                    b=shot(name+'-flow-b').crop((470, 8, 810, 40))
                    assert sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) > .05, 'Media artwork stopped moving'
                key(1)
        local_action('Skip this track', 'Skipped to the next track', 'action-next-track')
        assert 'Next' in (out/'media-events.log').read_text(), 'Next did not reach the player'
        local_action("What's playing?", 'Playing Another orbit', 'query-next-track')
        key(1)
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(1100, 1200)
        open_question('hello'); key(28); time.sleep(1.3)
        assert 'quiet answer' in text('fractional-output', 'TEST-2')
        key(1)
        assert shell.poll() is None and not ctl('configerrors').strip()
        (out/'assistant-requests.json').write_text(json.dumps(requests, indent=2))
        print('PASS: local timer lifecycle/status, live battery/Focus/track status and next-track confirmation without model requests, volume/playback actions, timer handoff, no request on open, draft restoration, native submit, streaming answer, copy, follow-up context, stop/dismiss, stale request isolation, retry, failure/empty states, long answer scroll, media continuity, densities, themes, fractional monitor, local dictation review, spoken status/actions, audio-reactive orb, Strata loading/queue/fallback/cancellation states and voice cleanup', flush=True)
    finally:
        for helper in helpers:
            helper.terminate(); helper.wait(timeout=5)
        server.shutdown(); server.server_close()
