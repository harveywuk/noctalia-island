"""Controlled GPU/RAM/CPU scenarios and real local speech/Strata latency.

Runs inside the private compositor and PipeWire server. The input is a synthesized
test sentence. No microphone from the user's desktop is accessed.
"""
import json
import os
import pathlib
import subprocess
import sys
import time
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
from shell_profile import measure, snapshot


def run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    source = pathlib.Path(os.environ['NOCTALIA_PROFILE_ASSISTANT_CONFIG'])
    ai = tomllib.loads(source.read_text())['shell']['launcher']['ai']
    assert ai['url'].startswith('http://127.0.0.1:'), 'Latency benchmark requires a local endpoint'
    voice = ai['voice']
    original = (cfg/'config.toml').read_text()
    config = original.split('\n[shell.launcher.ai]')[0] + '\n[shell.animation]\nenabled=true\n[shell.launcher.ai]\n'
    config += '\n'.join(k+'='+json.dumps(ai[k]) for k in ('provider','url','model','api_key') if k in ai)
    config += '\n[shell.launcher.ai.voice]\n' + '\n'.join(k+'='+json.dumps(v) for k,v in voice.items())+'\n'
    (cfg/'config.toml').write_text(config);msg('config-reload')
    run(['pactl','set-default-source','hyprland-test.monitor'])
    run(['wayland-scanner','client-header',str(repo/'protocols/virtual-keyboard-unstable-v1.xml'),str(base/'keyboard-client.h')])
    run(['wayland-scanner','private-code',str(repo/'protocols/virtual-keyboard-unstable-v1.xml'),str(base/'keyboard-code.c')])
    run(['cc','-I'+str(base),str(repo/'tests/fixtures/island_keyboard.c'),str(base/'keyboard-code.c'),'-lwayland-client','-lxkbcommon','-o',str(base/'keyboard')])
    keyboard = subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    results = {'conditions': {'seconds_per_phase':15,'voice_input':'synthetic greeting','ai_endpoint':'local Strata',
                              'cpu_basis':'100% = one logical core','gpu_basis':'DRM engine busy time for shell only',
                              'monitors':json.loads(ctl('-j','monitors'))},'phases':[], 'latency':[]}
    def save(): (out/'assistant-performance.json').write_text(json.dumps(results,indent=2))
    def record(label):
        time.sleep(2)
        row={'phase':label, **measure({'shell':shell.pid},15)}
        results['phases'].append(row);save();print(json.dumps(row),flush=True)
    def key(value):
        keyboard.stdin.write(str(value)+'\n');keyboard.stdin.flush()
        assert keyboard.stdout.readline().strip()=='ok'
    def status(): return json.loads(msg('assistant-voice','status'))
    def family_pids(pid):
        found={pid};todo=[pid]
        while todo:
            current=todo.pop()
            for task in pathlib.Path(f'/proc/{current}/task').glob('*/children'):
                try: children=[int(v) for v in task.read_text().split()]
                except OSError: continue
                for child in children:
                    if child not in found: found.add(child);todo.append(child)
        return found
    def until(done, timeout=180):
        begin=time.monotonic();peak_rss=peak_pss=strata_pss=None
        while not done():
            assert shell.poll() is None
            rss=pss=0
            for pid in family_pids(shell.pid):
                try:
                    data=snapshot(pid);rss+=data['rss_mib'];pss+=data['pss_mib']
                except (OSError,ProcessLookupError): pass
            peak_rss=max(peak_rss or 0,rss);peak_pss=max(peak_pss or 0,pss)
            if os.environ.get('NOCTALIA_PROFILE_STRATA_PID'):
                total=0
                for pid in family_pids(int(os.environ['NOCTALIA_PROFILE_STRATA_PID'])):
                    try: total+=snapshot(pid)['pss_mib']
                    except (OSError,ProcessLookupError): pass
                strata_pss=max(strata_pss or 0,total)
            assert time.monotonic()-begin<timeout,'Voice/model benchmark timed out'
            time.sleep(.08)
        # A stage can finish between status polls, before any memory sample exists.
        return {'seconds':round(time.monotonic()-begin,3),
                'shell_and_helpers_peak_rss_mib':round(peak_rss,1) if peak_rss is not None else None,
                'shell_and_helpers_peak_pss_mib':round(peak_pss,1) if peak_pss is not None else None,
                'strata_peak_pss_mib':round(strata_pss,1) if strata_pss is not None else None}
    try:
        ctl('dismissnotify');dispatch('hl.dsp.focus({monitor="TEST-1"})');dispatch('hl.dsp.cursor.move({x=1100,y=600})')
        record('idle-closed')
        msg('panel-open','assistant');record('assistant-animated-orb');msg('panel-close')
        msg('panel-open','control-center','audio');record('control-centre');msg('panel-close')
        env['ISLAND_TEST_ART']=(repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_EVENTS']=str(out/'media-events.log')
        player=start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'player.log')
        time.sleep(1);record('media-artwork')
        msg('panel-open','assistant');record('assistant-with-media');msg('panel-close')
        player.terminate();player.wait(timeout=3)
        for batch in range(2):
            for _ in range(10):
                for panel in ('assistant','control-center','launcher'):
                    msg('panel-open',panel);time.sleep(.15);msg('panel-close');time.sleep(.15)
            record('closed-after-ui-cycles-'+str(batch+1))

        # Generate a known sentence with the configured real Piper voice.
        sentence=base/'sentence.txt';sentence.write_text('Hello. Please give me a short friendly greeting.')
        wav=base/'sentence.wav'
        run([voice['piper_command'],'-m',voice['piper_model'],'--input-file',str(sentence),'-f',str(wav)])
        for turn in range(2):
            msg('panel-open','assistant');key('chord 2 49');time.sleep(.2)
            msg('assistant-voice','start');time.sleep(.4)
            playback=start(['pw-play',str(wav)],f'input-{turn}.log');playback.wait(timeout=30);time.sleep(.15)
            msg('assistant-voice','finish')
            transcription=until(lambda:status()['voice']=='idle')
            assert not status()['error'] and status()['has_draft'],status()
            key(28)
            first_text=until(lambda:status()['has_answer'])
            answer=until(lambda:status()['voice'] in ('preparing','speaking'))
            tts=until(lambda:status()['voice']=='speaking')
            assert not status()['error'],status()
            results['latency'].append({'turn':turn+1,'transcription':transcription,'first_text':first_text,
                                      'answer_after_first_text':answer,'speech_startup':tts})
            save();print(json.dumps(results['latency'][-1]),flush=True)
            key(1);time.sleep(1)
        record('settled-after-real-voice')
        results['renderer'] = next((line for line in (out/'noctalia.log').read_text().splitlines() if 'OpenGL ES vendor' in line), '')
        save();print('PASS: shell resource and local voice benchmark',flush=True)
    finally:
        keyboard.terminate();keyboard.wait(timeout=3)
