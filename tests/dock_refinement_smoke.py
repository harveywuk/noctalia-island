"""Dock keyboard, drag pin/unpin, name-before-preview and compact appearance checks."""
import json
import pathlib
import re
import subprocess
import time
import tomllib
from PIL import Image

from dock_motion_smoke import prepare as prepare_motion


def prepare(base, cfg, env):
    prepare_motion(base, cfg, env)
    config = cfg/'config.toml'
    config.write_text(config.read_text().replace('hide_delay_ms=400', 'hide_delay_ms=400\nwindow_previews=true\npreview_delay_ms=900'))
    apps = pathlib.Path(env['XDG_DATA_HOME'])/'applications'
    (apps/'dock-running-test.desktop').write_text('[Desktop Entry]\nType=Application\nName=Running App\nExec=kitty --class dock-running-test\nIcon=web-browser\nStartupWMClass=dock-running-test\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner','client-header',str(protocol),str(base/(kind+'-client.h'))])
        run(['wayland-scanner','private-code',str(protocol),str(base/(kind+'-code.c'))])
        run(['cc','-I'+str(base),str(repo/f'tests/fixtures/island_{kind}.c'),str(base/(kind+'-code.c')),*libs,'-o',str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True))
    pointer, keyboard = helpers
    original_pins = ['dock-motion-test','dock-second-test']
    state = pathlib.Path(env['XDG_STATE_HOME'])/'noctalia/settings.toml'
    def pins():
        settings = tomllib.loads(state.read_text()) if state.exists() else {}
        return settings.get('dock',{}).get('pinned',original_pins)
    def restore_pins():
        if state.exists():
            state.write_text(re.sub(r'(?ms)^\[dock\]\n.*?(?=^\[|\Z)','',state.read_text()))
        msg('config-reload');time.sleep(.5)
        assert pins() == original_pins
    def command(proc, value):
        proc.stdin.write(str(value)+'\n');proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'
    def key(value): command(keyboard,value);time.sleep(.12)
    def move(x,y,delay=.35):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer,'relative 1 0');command(pointer,'relative -1 0');time.sleep(delay)
    def shot(name,output='TEST-1'):
        path=out/(name+'.png');run(['grim','-o',output,str(path)]);return path
    def words(name):
        path = shot(name)
        image = Image.open(path).convert('L').point(lambda value: 0 if value > 150 else 255)
        processed = out/(name+'-ocr.png')
        image.resize((image.width*3, image.height*3), Image.Resampling.LANCZOS).save(processed)
        return run(['tesseract',str(processed),'stdout','--tessdata-dir',str(repo/'build-rishot/test-data/tessdata'),'--psm','11']).lower()
    def clients(): return [c for c in json.loads(ctl('-j','clients')) if c['class'].startswith('dock-')]
    def reveal(): move(640,719,.4)
    def drag(x,y,dx,dy):
        reveal();move(x,y,.15);command(pointer,'press');time.sleep(.4)
        command(pointer,f'relative {dx} {dy}');time.sleep(.3)
    def release(): command(pointer,'release');time.sleep(.6)
    try:
        ctl('dismissnotify');dispatch('hl.dsp.focus({monitor="TEST-1"})');move(1100,400)
        msg('color-scheme-set','community','macOS');msg('theme-mode-set','dark');time.sleep(.5)
        shot('dock-comfortable')
        msg('dock-focus');time.sleep(.6);shot('dock-keyboard-launcher')
        key(106);key(28)
        wait(lambda:len(clients())==1,'Keyboard Enter did not launch the selected app')
        msg('dock-focus');time.sleep(.4);key(106);key('chord 1 68');time.sleep(.4)
        text=words('dock-keyboard-menu');assert 'unpin' in text,text
        key(1);time.sleep(.2);shot('dock-keyboard-menu-dismissed');key(1)
        move(1100,400);time.sleep(.6)
        msg('dock-focus');time.sleep(.4);key(28);time.sleep(.5);shot('launcher-from-keyboard-dock');msg('panel-close')
        print('PASS: dock keyboard activation, app menu and Escape',flush=True)

        # A name appears before the configured preview delay; a later preview replaces it.
        reveal();move(1100,600,.05);move(640,670,.70)
        name_text=words('dock-name-before-preview')
        time.sleep(.6);preview_text=words('dock-deliberate-preview')
        move(1100,400,.7)

        start(['kitty','--config','NONE','--class','dock-running-test','--title','Running App','/bin/sh'],'running-app.log')
        wait(lambda:len(clients())==2,'Running fixture did not open')
        move(1100,400,.6)
        # Four items, including the launcher; the last app is in the unpinned group.
        drag(738,670,-150,0);shot('dock-drag-to-pin');release()
        shot('dock-after-pin')
        wait(lambda:pins()[0]=='dock-running-test','Dragging a running app into the pinned group did not pin it')
        print('PASS: drag running app into pinned group',flush=True)
        # Leaving and returning cancels removal. Escape also leaves pins unchanged.
        drag(609,670,0,-110);shot('dock-drag-remove-cue');key(1);release()
        assert pins()[0]=='dock-running-test','Escape committed a drag removal'
        drag(609,670,0,-110);command(pointer,'relative 0 110');time.sleep(.15);release()
        assert pins()[0]=='dock-running-test','Returning to the dock removed a pin'
        drag(609,670,0,-110);shot('dock-drag-unpin');release()
        wait(lambda:'dock-running-test' not in pins(),'Dragging away did not unpin')
        assert any(c['class']=='dock-running-test' for c in clients()),'Unpin closed the app'
        print('PASS: drag-out removal, return and Escape cancellation',flush=True)

        # Reorder pins in both directions and ensure a cancelled drag never launches an app.
        drag(602,670,82,0);release()
        wait(lambda:pins()[:2]==list(reversed(original_pins)),'Reorder to end failed')
        drag(664,670,-82,0);release()
        wait(lambda:pins()[:2]==original_pins,'Reorder to beginning failed')
        restore_pins()
        assert len(clients())==2

        # The same menu treatment works with the pointer and keyboard.
        reveal();move(602,670,.2);command(pointer,'right-press');command(pointer,'right-release');time.sleep(.4)
        shot('dock-pointer-menu');key(1);move(1100,400,.5)
        config=cfg/'config.toml';config.write_text(config.read_text().replace('icon_size=48','icon_size=36').replace('[shell.animation]\nenabled=true','[shell.animation]\nenabled=false'))
        msg('config-reload');msg('theme-mode-set','light');time.sleep(.6)
        reveal();shot('dock-compact-light')
        dispatch('hl.dsp.focus({monitor="TEST-2"})');move(1100,1100)
        msg('dock-focus');time.sleep(.6);shot('dock-keyboard-fractional','TEST-2');key(106);key(1)
        assert shell.poll() is None and not ctl('configerrors').strip()
        assert 'motion' in name_text,name_text
        assert 'motion' in preview_text,preview_text
        print('PASS: dock reorder, pointer-menu Escape, compact/reduced motion and fractional output',flush=True)
    finally:
        for proc in helpers:
            if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
