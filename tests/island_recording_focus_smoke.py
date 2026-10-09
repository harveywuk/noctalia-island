"""Temporary recording Focus with real recordings on a private GPU desktop."""
import csv
import io
import json
import shutil
import subprocess
import sys
import time
import tomllib

from PIL import Image, ImageChops
from island_capture_menu_smoke import prepare as prepare_menu, REPO, TESSDATA


def prepare(base, cfg, env):
    prepare_menu(base, cfg, env)
    with (cfg/'config.toml').open('a') as config:
        config.write('\n[notification.focus.work]\nallowed_apps=["Retained App"]\nallow_critical=false\n'
                     'start_minute=1335\nend_minute=405\ndays=65\n')
    fixtures=base/'focus-bin'; fixtures.mkdir()
    (fixtures/'wf-recorder').write_text(
        '#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\n'
        'if Path('+repr(str(base/'fail-recording'))+').exists(): sys.exit(7)\n'
        'os.execv('+repr(shutil.which('wf-recorder'))+', ["wf-recorder", *sys.argv[1:]])\n')
    (fixtures/'wf-recorder').chmod(0o700)
    env['PATH']=str(fixtures)+':'+env['PATH']


def run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell):
    for kind,proto,libs in (
        ('pointer',REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml',['-lwayland-client']),
        ('keyboard',REPO/'protocols/virtual-keyboard-unstable-v1.xml',['-lwayland-client','-lxkbcommon']),
    ):
        run(['wayland-scanner','client-header',str(proto),str(base/(kind+'-client.h'))])
        run(['wayland-scanner','private-code',str(proto),str(base/(kind+'-code.c'))])
        source=base/(kind+'.c')
        source.write_text((REPO/f'tests/fixtures/island_{kind}.c').read_text().replace('x,y,1280,720','x,y,1280,1440'))
        run(['cc','-I'+str(base),str(source),str(base/(kind+'-code.c')),*libs,'-o',str(base/kind)])
    pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    keyboard=subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)

    def send(proc,command):
        proc.stdin.write(command+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip()=='ok'

    def move(x,y):
        send(pointer,f'move {round(x)} {round(y)}'); time.sleep(.15)

    def click(x,y):
        move(x,y); send(pointer,'press'); send(pointer,'release'); time.sleep(.5)

    def key(code):
        send(keyboard,str(code)); time.sleep(.25)

    def shot(name,output='TEST-1'):
        path=out/(name+'.png'); run(['grim','-o',output,str(path)])
        return Image.open(path).convert('RGB').resize((1280,720))

    def words(name,output='TEST-1'):
        im=shot(name,output).resize((2560,1440)); path=out/(name+'-text.png'); im.save(path)
        result=run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','11','-c','tessedit_create_tsv=1'])
        rows=csv.DictReader(io.StringIO(result[result.index('level\t'):]),delimiter='\t')
        return [(r['text'].strip(),(int(r['left'])+int(r['width'])/2)/2,
                 (int(r['top'])+int(r['height'])/2)/2) for r in rows if (r.get('text') or '').strip()]

    def text(name,output='TEST-1'):
        return ' '.join(w for w,x,y in words(name,output))

    def setting(label, name, scroll=False):
        import ocr
        for _ in range(12 if scroll else 1):
            shot(name)
            point=ocr.find(out/(name+'.png'), label, min_x=300)
            if point and point[1]<650:
                return point
            move(1100,580); send(pointer,'scroll 2'); time.sleep(.3)
        raise AssertionError('Setting not found: '+label)

    def focus():
        return json.loads(msg('focus-status'))

    def clear():
        msg('notification-clear-active'); move(1100,600); time.sleep(.4)

    def selection(output='TEST-1'):
        dispatch('hl.dsp.focus({monitor='+json.dumps(output)+'})')
        offset=720 if output=='TEST-2' else 0
        move(1100,600+offset); msg('record-region'); time.sleep(.5)
        return offset

    def region(offset=0):
        move(100,250+offset); send(pointer,'press'); move(500,500+offset); send(pointer,'release')

    def record(expected='recording',output='TEST-1'):
        clear(); region(selection(output))
        wait(lambda:msg('record-status').startswith('REC'),'Recorder did not start')
        assert focus()['mode']==expected,focus()
        move(1100,600+(720 if output=='TEST-2' else 0)); time.sleep(1)

    def stop(expected):
        msg('record-stop'); wait(lambda:msg('record-status')=='idle','Recorder did not stop')
        assert focus()['mode']==expected,focus()

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
        assert focus()=={'mode':'off','automatic':True}
        record('off'); stop('off'); clear() # Opt-in, no change to existing recording behavior.

        msg('panel-open','control-center','focus'); time.sleep(1)
        quick=text('focus-quick-controls')
        assert all(word in quick for word in ('Work','Gaming','Sleep')),quick
        assert 'Configure' not in quick and 'while recording' not in quick,quick
        msg('panel-close'); msg('settings-open','notifications'); time.sleep(1)
        click(*setting('Focus','focus-settings-overview'))
        assert 'Retained App' in text('focus-settings-profile'), 'Existing Focus profile was not loaded'
        settings=base/'state/noctalia/settings.toml'
        click(*setting('Retained App','focus-settings-apps')); key(107); key(57); key(3)
        click(*setting('Save Focus','focus-settings-save',scroll=True))
        wait(lambda:settings.exists() and tomllib.loads(settings.read_text()).get('notification',{}).get('focus',{}).get('work'),
             'Focus profile was not saved from Settings')
        saved=tomllib.loads(settings.read_text())['notification']['focus']['work']
        # Unchanged fields inherit the original profile instead of redundant overrides.
        assert saved=={'allowed_apps':['Retained App 2']},saved
        move(1100,300); send(pointer,'scroll -30'); time.sleep(.5)
        row=setting('Focus while recording','focus-settings-recording')
        click(1132,row[1]+10) # Toggle is centred beside the title and its one-line description.
        wait(lambda:settings.exists() and tomllib.loads(settings.read_text()).get('notification',{}).get('focus',{}).get('while_recording'),
             'Focus while recording toggle was not saved')
        shot('recording-focus-setting-enabled'); msg('settings-close'); time.sleep(.6)

        # Neither opening a selector nor cancelling the pre-record countdown changes Focus.
        selection(); assert focus()['mode']=='off'; key(1)
        msg('capture-menu'); time.sleep(.7)
        click(735,78); click(645,166); click(690,255); time.sleep(.5); region()
        assert focus()['mode']=='off'; msg('record-stop'); time.sleep(3.5)
        assert msg('record-status')=='idle' and focus()['mode']=='off'

        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'focus-player.log'); time.sleep(1)
        record(); assert focus()['automatic']
        a=shot('recording-focus-active'); time.sleep(.5); b=shot('recording-focus-active-after')
        strip=(580,14,690,20)
        assert ImageChops.difference(a.crop(strip),b.crop(strip)).getbbox(),'Recording Focus froze media artwork'
        run(['notify-send','-a','QuietApp','-t','1500','Quiet notice','Retained in history'])
        time.sleep(.5); assert 'Quiet notice' not in text('recording-focus-quiet')
        history=base/'state/noctalia/notification_history.json'
        wait(lambda:history.exists() and 'Quiet notice' in history.read_text(),'Suppressed notification missing from history')
        run(['notify-send','-u','critical','Urgent alert','Recording continues'])
        time.sleep(.6); assert 'Urgent' in text('recording-focus-urgent')
        assert msg('record-status').startswith('REC') and focus()['mode']=='recording'
        clear(); stop('off'); assert focus()['automatic']; time.sleep(.6)
        content=text('recording-focus-restored-preview'); assert 'Recording' in content and 'saved' in content,content
        time.sleep(5.5); assert 'Quiet notice' not in text('recording-focus-no-replay'); clear()

        # Preserve manual presets, plain DND and user changes, including across reloads.
        msg('focus-set','work'); record(); stop('work'); clear()
        record(); msg('focus-set','sleep'); assert focus()['mode']=='sleep'
        msg('config-reload'); time.sleep(.6); assert focus()['mode']=='sleep'; stop('sleep'); clear()
        msg('focus-set','off'); record(); msg('notification-dnd-set','on')
        assert focus()['mode']=='dnd'; stop('dnd'); clear()
        record(); stop('dnd'); msg('notification-dnd-set','off'); clear()

        # A recording failure restores Focus before its failure notification is delivered.
        (base/'fail-recording').touch(); region(selection())
        wait(lambda:msg('record-status')=='idle','Failed recorder did not exit'); time.sleep(.5)
        assert focus()['mode']=='off'; assert 'failed' in text('recording-focus-failure')
        (base/'fail-recording').unlink(); clear()

        record(output='TEST-2'); shot('recording-focus-scaled','TEST-2')
        msg('focus-set','off'); msg('config-reload'); time.sleep(.6)
        assert focus()['mode']=='off'; stop('off'); clear()
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: quick Control Centre, Settings profile round-trip and recording toggle persistence, selector/countdown cancellation, recording Focus, live artwork, quiet history, urgent alerts, saved preview, no replay, preset/DND restoration, manual override, reload, recorder failure and scaled output',flush=True)
    except Exception:
        shot('focus-settings-failure')
        settings=base/'state/noctalia/settings.toml'
        if settings.exists():
            (out/'failed-settings.toml').write_text(settings.read_text())
        raise
    finally:
        if shell.poll() is None:
            msg('record-stop')
        for proc in (pointer,keyboard):
            proc.terminate(); proc.wait(timeout=5)
