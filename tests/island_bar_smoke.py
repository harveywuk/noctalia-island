"""Managed Island checks, run via hyprland_smoke.py --island-bars-only."""
import csv
import io
import json
import os
import pathlib
import subprocess
import time
import tomllib
from PIL import Image, ImageChops

def run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell):
    repo=pathlib.Path(__file__).resolve().parents[1]
    baseline=(cfg/'config.toml').read_text().replace('[island]\nenabled=true','[island]\nenabled=false')
    managed='''
[bar.capsule]
presentation="island"
reserve_space=false
smart_auto_hide=true
show_on_workspace_switch=false
[bar.capsule.island]
hover_widgets=["workspaces"]
hover_widgets_center=["clock"]
[bar.capsule.monitor.TEST-2]
scale=1.25
smart_auto_hide=false
[bar.capsule.monitor.TEST-2.island]
clock_seconds=true
height=52
hover_widgets=[]
'''
    def configure(text):
        (cfg/'config.toml').write_text(baseline+text)
        msg('config-reload')
        time.sleep(.7)
        assert shell.poll() is None,'Noctalia exited on bar reload'
    def layers(output,namespace):
        data=json.loads(ctl('-j','layers'))[output]['levels']
        return [layer for level in data.values() for layer in level if layer['namespace'].startswith(namespace)]
    def capture(name,output='TEST-1'):
        path=out/(name+'.png');run(['grim','-o',output,str(path)])
        return Image.open(path).convert('RGB')
    def island_pixels(image):
        return image.crop((550,15,730,60))
    def differs(first,second):
        return ImageChops.difference(island_pixels(first),island_pixels(second)).getbbox() is not None
    def point(x,y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        time.sleep(.15)
    configure(managed)
    point(1100,600)
    for output in ('TEST-1','TEST-2'):
        assert len(layers(output,'noctalia-island'))==1
        assert not layers(output,'noctalia-bar')
    capture('island-empty')
    capture('island-monitor-override','TEST-2')
    # A solid client makes a hidden Island observable independently of the wallpaper.
    point(1100,600)
    client=start(['kitty','--config','NONE','--class','island-hide-test','--override','remember_window_size=no',
                  '--override','background=#235623','--override','initial_window_width=900','--override','initial_window_height=600'],'island-client.log')
    wait(lambda:any(w['class']=='island-hide-test' for w in json.loads(ctl('-j','clients'))),'test client')
    time.sleep(1)
    hidden=capture('island-smart-hidden')
    configure(managed.replace('smart_auto_hide=true','smart_auto_hide=false'))
    shown=capture('island-forced-shown')
    assert differs(hidden,shown),'Smart hide did not change the occupied workspace rendering'
    configure(managed)
    assert not differs(hidden,capture('island-hidden-again')),'Smart hide did not settle back to the hidden state'
    point(640,1);time.sleep(1)
    assert differs(hidden,capture('island-pointer-reveal')),'Top edge did not reveal the Island'
    point(1100,600);time.sleep(1)
    assert not differs(hidden,capture('island-pointer-left')),'Island stayed visible after pointer leave'
    # OSD and panels must remain usable while the Island normally hides.
    run(['pactl','set-sink-volume','@DEFAULT_SINK@','43%']);time.sleep(.4)
    assert differs(hidden,capture('island-osd-reveal')),'OSD failed to reveal the Island'
    time.sleep(3)
    msg('panel-open','launcher');time.sleep(.7)
    capture('island-hosted-launcher')
    msg('panel-close');time.sleep(1)
    assert not differs(hidden,capture('island-panel-return')),'Panel return left the Island visible'
    client.terminate();client.wait(timeout=5);time.sleep(1)
    empty=capture('island-empty-again')
    configure(managed.replace('smart_auto_hide=true','auto_hide=true'))
    assert differs(empty,capture('island-auto-hide-empty')),'Ordinary auto hide should also hide on empty workspaces'
    configure(managed.replace('smart_auto_hide=true','auto_hide=true').replace('show_on_workspace_switch=false','show_on_workspace_switch=true'))
    dispatch('hl.dsp.focus({workspace="9"})')
    time.sleep(.2)
    peek=capture('island-workspace-peek')
    time.sleep(1)
    assert differs(peek,capture('island-peek-finished')),'Workspace switch did not briefly reveal the hidden Island'
    configure(managed.replace('smart_auto_hide=true','smart_auto_hide=false').replace('reserve_space=false','reserve_space=true'))
    monitors=json.loads(ctl('-j','monitors'))
    assert all(m['reserved'][1]>0 for m in monitors),monitors
    configure(managed.replace('[bar.capsule.monitor.TEST-2]','[bar.capsule.monitor.TEST-2]\npresentation="bar"'))
    assert len(layers('TEST-1','noctalia-island'))==1 and not layers('TEST-2','noctalia-island')
    assert layers('TEST-2','noctalia-bar'),'Monitor presentation override did not create a standard bar'
    capture('standard-bar-monitor','TEST-2')
    configure(managed.replace('[bar.capsule.monitor.TEST-2]','[bar.capsule.monitor.TEST-2]\nenabled=false'))
    assert not layers('TEST-2','noctalia-island'),'Disabled monitor still has an Island'
    previous=baseline
    baseline=baseline.replace('[bar.default]\nenabled=false','[bar.default]\nenabled=true\nposition="bottom"')
    baseline=baseline.replace('[shell]','[shell]\npanel_anchor_bar="default"')
    configure(managed)
    msg('panel-open','launcher');time.sleep(.7)
    assert layers('TEST-1','noctalia-panel') or layers('TEST-1','noctalia-attached-panel'),'Explicit standard-bar panel anchor was ignored'
    capture('island-standard-panel-anchor')
    msg('panel-close');time.sleep(.5)
    baseline=previous
    configure(managed)
    msg('settings-open','bar');time.sleep(.7)
    capture('island-bar-settings')
    proto=repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner','client-header',str(proto),str(base/'pointer-client.h')])
    run(['wayland-scanner','private-code',str(proto),str(base/'pointer-code.c')])
    run(['cc','-I'+str(base),str(repo/'tests/fixtures/island_pointer.c'),str(base/'pointer-code.c'),'-lwayland-client','-o',str(base/'pointer')])
    pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def mouse(command):
        pointer.stdin.write(command+'\n');pointer.stdin.flush()
        assert pointer.stdout.readline().strip()=='ok'
        time.sleep(.12)
    def click(x,y):
        point(x,y);mouse('press');mouse('release');time.sleep(.3)
    def overrides():
        return tomllib.loads((base/'state/noctalia/settings.toml').read_text()).get('bar',{})
    def find_text(word,name,min_x=300):
        """Centre of the first OCR word containing `word` (right of the sidebar) on a fresh screenshot."""
        capture(name)
        data=run(['tesseract',str(out/(name+'.png')),'stdout','--tessdata-dir',
                  os.environ.get('NOCTALIA_TEST_TESSDATA',str(repo/'build-rishot/test-data/tessdata')),
                  '--psm','11','-c','tessedit_create_tsv=1'])
        for row in csv.DictReader(io.StringIO(data),delimiter='\t',quoting=csv.QUOTE_NONE):
            if word in (row.get('text') or '') and int(row['left'])>=min_x:
                return int(row['left'])+int(row['width'])//2,int(row['top'])+int(row['height'])//2
        raise AssertionError(f'{word!r} not found in {name}')
    try:
        # The hover-widget editor is the bar page's Widgets group; its Media preset leaves only
        # Volume in the left lane, and Undo preset restores the inherited layout.
        msg('settings-open','bar/widgets');time.sleep(.8)
        click(*find_text('Media','island-widgets-editor'))
        assert overrides()['capsule']['island']['hover_widgets']==['volume'],overrides()
        click(*find_text('Undo','island-widget-preset'))
        assert not overrides(),'Undo did not restore the inherited bar layout'
        # The monitor override is an indented sidebar row under its bar, below the fold.
        msg('settings-open','bar');time.sleep(.8)
        point(150,500)
        for _ in range(20):mouse('scroll 1')
        click(*find_text('TEST-2','island-monitor-sidebar',min_x=0));time.sleep(.5)
        try: widgets=find_text('Widgets','island-monitor-groups')
        except AssertionError:
            point(760,500)
            for _ in range(10):mouse('scroll 1')
            widgets=find_text('Widgets','island-monitor-groups')
        click(*widgets);time.sleep(.5)
        click(*find_text('Media','island-monitor-editor'))
        saved=overrides()['capsule']
        assert saved['monitor']['TEST-2']['island']['hover_widgets']==['volume'],saved
        assert not saved.get('island'),'Monitor preset changed the parent bar'
        click(*find_text('Undo','island-monitor-preset'))
        assert not overrides(),'Monitor Undo did not restore inheritance'
    finally:
        pointer.terminate();pointer.wait(timeout=5)
    (out/'inspect-env.json').write_text(json.dumps(env))
    (out/'inspect-paths.json').write_text(json.dumps({'base':str(base),'config':str(cfg),'binary':str(repo/'build-rishot/noctalia')}))
    if os.environ.get('NOCTALIA_TEST_ISLAND_BARS_INSPECT'):
        print('INSPECT: Island bar settings ready',flush=True)
        until=time.monotonic()+int(os.environ['NOCTALIA_TEST_ISLAND_BARS_INSPECT'])
        while time.monotonic()<until:time.sleep(.25)
    msg('settings-close')
    configure('')
    assert all(not layers(output,'noctalia-island') for output in ('TEST-1','TEST-2')),'Removing managed bars left Island surfaces'
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: managed Islands, monitor content and presentation overrides, smart/ordinary hide, pointer reveal, OSD, panel return, reservation and removal',flush=True)
