"""Two-output preview routing and hotplug checks for the private Hyprland harness."""
import json
import pathlib
import subprocess
import sys
import time
from PIL import Image


def run_checks(base,cfg,out,env,run,ctl,dispatch,msg,wait,start,shell,bluetooth):
    repo=pathlib.Path(__file__).resolve().parents[1]
    # Use equal logical/physical dimensions so preview widths compare directly.
    hypr=base/'hyprland.lua'
    hypr.write_text(hypr.read_text().replace('mode="1920x1080@60",position="0x720",scale=1.5,transform=2',
                                           'mode="1280x720@60",position="0x720",scale=1,transform=0'))
    ctl('reload');time.sleep(1)
    baseline=(cfg/'config.toml').read_text().replace('[island]\nenabled=true','[island]\nenabled=false')
    def configure(target='focused',seconds=5):
        (cfg/'config.toml').write_text(baseline+'\n[bar]\norder=["capsule","default"]\n[bar.capsule]\npresentation="island"\nreserve_space=false\n[bar.capsule.island]\ntrack_preview_monitor='+json.dumps(target)+'\nbluetooth_preview_monitor='+json.dumps(target)+'\ntrack_preview_seconds='+str(seconds)+'\nbluetooth_preview_seconds='+str(seconds)+'\n')
        msg('config-reload');time.sleep(.6)
    def focus(output):
        y=600 if output=='TEST-1' else 1320
        dispatch(f'hl.dsp.cursor.move({{x=1100,y={y}}})')
        dispatch('hl.dsp.focus({monitor='+json.dumps(output)+'})');time.sleep(.35)
    def image(name,output):
        path=out/(name+'-'+output+'.png');run(['grim','-o',output,str(path)])
        return Image.open(path).convert('RGB')
    def width(name,output):
        picture=image(name,output);left=640
        while left>200 and max(picture.getpixel((left-1,12)))<45:left-=1
        # Hyprland's nested-session warning covers the upper-right corner.
        return 2*(640-left)
    def media(method):
        run(['gdbus','call','--session','--dest','org.mpris.MediaPlayer2.islandtest','--object-path','/org/mpris/MediaPlayer2','--method','org.mpris.MediaPlayer2.Player.'+method])
    def pair(name,selected):
        for output in ('TEST-1','TEST-2'):
            result=width(name,output)
            assert (result>380)==(output in selected),(name,output,result,selected)
    def until(deadline):time.sleep(max(0,deadline-time.monotonic()))
    def unplug():
        ctl('output','remove','TEST-2')
        wait(lambda:'TEST-2' not in [m['name'] for m in json.loads(ctl('-j','monitors'))],'Output removed')
        time.sleep(.4)
        assert shell.poll() is None,'Shell exited on output removal'
    def reconnect():
        ctl('output','create','headless','TEST-2')
        wait(lambda:'TEST-2' in json.loads(ctl('-j','layers')),'Output reconnected')
        time.sleep(.8)
        layers=json.loads(ctl('-j','layers'))
        for output in ('TEST-1','TEST-2'):
            count=sum(1 for entries in layers[output]['levels'].values() for entry in entries if entry.get('namespace')=='noctalia-island')
            assert count==1,(output,count)
    configure();msg('theme-mode-set','dark');focus('TEST-1')
    env['ISLAND_TEST_ART']=(repo/'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_EVENTS']=str(out/'routing-player-actions.log')
    player=start([sys.executable,str(repo/'tests/fixtures/island_player.py')],'routing-player.log')
    time.sleep(.8);pair('focused-first',{'TEST-1'})
    focus('TEST-2');pair('focus-moved',{'TEST-1'})
    media('Next');time.sleep(.7);pair('focused-next',{'TEST-2'})
    configure('focused',8);media('Previous');began=time.monotonic();time.sleep(.7)
    unplug();assert width('focused-unplug','TEST-1')>380,'Remaining focused preview did not fall back'
    reconnect();pair('focused-reconnected',{'TEST-1'})
    until(began+8.7);pair('focused-expired',set())
    configure('TEST-2',8);focus('TEST-1');media('Next');began=time.monotonic();time.sleep(.7)
    pair('specific',{'TEST-2'});unplug()
    assert width('specific-unplug','TEST-1')<310,'Specific display preview leaked to another monitor'
    reconnect();pair('specific-reconnected',{'TEST-2'})
    until(began+8.7);pair('specific-expired',set())
    configure('all');media('Previous');time.sleep(.7);pair('all',{'TEST-1','TEST-2'})
    configure('missing-output');media('Next');time.sleep(.7);pair('unavailable-specific',set())
    # Keep a media card keyboard-focused on TEST-1 while interacting with TEST-2.
    configure('all')
    proto=repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner','client-header',str(proto),str(base/'pointer-client.h')])
    run(['wayland-scanner','private-code',str(proto),str(base/'pointer-code.c')])
    run(['cc','-I'+str(base),str(repo/'tests/fixtures/island_pointer.c'),str(base/'pointer-code.c'),'-lwayland-client','-o',str(base/'pointer')])
    pointer=subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    publisher=subprocess.Popen([sys.executable,str(repo/'tests/fixtures/island_downloads.py')],env=env,
                               stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
    def click(x,y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})');time.sleep(.2)
        for command in ('press','release'):
            pointer.stdin.write(command+'\n');pointer.stdin.flush();assert pointer.stdout.readline().strip()=='ok'
        time.sleep(.5)
    try:
        publisher.stdin.write(json.dumps({'uri':'application://routing-download.desktop','properties':{'progress':.25,'progress-visible':True}})+'\n')
        publisher.stdin.flush();assert publisher.stdout.readline().strip()=='ok';time.sleep(.5)
        focus('TEST-1');dispatch('hl.dsp.cursor.move({x=640,y=40})');time.sleep(.6)
        msg('island-focus');time.sleep(.5);click(556,31)
        dispatch('hl.dsp.cursor.move({x=640,y=760})');time.sleep(.7);click(722,751)
        image('independent-media','TEST-1');image('independent-downloads','TEST-2')
        actions=out/'routing-player-actions.log';before=actions.read_text().count('PlayPause')
        click(640,207)
        assert actions.read_text().count('PlayPause')==before+1,'Other monitor changed the selected media card'
        msg('panel-open','launcher');time.sleep(.3);msg('panel-close')
    finally:
        pointer.terminate();pointer.wait(timeout=5);publisher.terminate();publisher.wait(timeout=5)
    configure('focused',4);media('Stop');time.sleep(.5);focus('TEST-2')
    try:
        def bt(**values):
            bluetooth.stdin.write(json.dumps(values)+'\n');bluetooth.stdin.flush()
            assert bluetooth.stdout.readline().strip()=='ok';time.sleep(.3)
        bt(Connected=True,Percentage=75);began=time.monotonic();time.sleep(.4)
        assert width('bluetooth-focused','TEST-2')>210 and width('bluetooth-other','TEST-1')<200
        focus('TEST-1')
        assert width('bluetooth-focus-moved','TEST-2')>210 and width('bluetooth-stays','TEST-1')<200
        until(began+4.7)
        assert width('bluetooth-expired','TEST-1')<200 and width('bluetooth-expired','TEST-2')<200
        bt(Percentage=5)
        assert width('low-battery','TEST-1')>210 and width('low-battery','TEST-2')>210
        bt(Connected=False);bt(Connected=True,Percentage=75)
        assert width('bluetooth-reconnect','TEST-1')>210 and width('bluetooth-reconnect','TEST-2')<200
    finally:
        bluetooth.terminate();bluetooth.wait(timeout=5)
    player.terminate();player.wait(timeout=5)
    configure('missing-output');focus('TEST-1');msg('settings-open','bar');time.sleep(.7)
    image('routing-settings','TEST-1')
    (out/'inspect-env.json').write_text(json.dumps(env))
    if __import__('os').environ.get('NOCTALIA_TEST_ROUTING_INSPECT'):
        print('INSPECT: routing settings ready',flush=True)
        deadline=time.monotonic()+int(__import__('os').environ['NOCTALIA_TEST_ROUTING_INSPECT'])
        while time.monotonic()<deadline and not (out/'inspect-done').exists():time.sleep(.25)
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: focused/all/specific routing, stable event focus, unplug fallback, reconnect without timer restart, unavailable display, Bluetooth targeting, low-battery visibility and independent activity selection',flush=True)
