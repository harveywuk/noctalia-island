"""Keyboard capture entry and window selection on two private GPU outputs."""
import json
import shlex
import subprocess
import sys
import time

from PIL import Image
from island_capture_menu_smoke import REPO, TESSDATA, prepare as prepare_menu


def prepare(base, cfg, env):
    prepare_menu(base,cfg,env)
    binary=env.get('NOCTALIA_TEST_BINARY',str(REPO/'build-release/noctalia'))
    path=base/'hyprland.lua'
    path.write_text(path.read_text()+'\nhl.bind("SHIFT + Print", hl.dsp.exec_cmd('
                    +json.dumps(shlex.quote(binary)+' msg capture-menu')+'))\n')


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

    def key(command):
        send(keyboard,str(command)); time.sleep(.18)

    def move(x,y):
        send(pointer,f'move {round(x)} {round(y)}'); time.sleep(.16)

    def click(x,y):
        move(x,y); send(pointer,'press'); send(pointer,'release'); time.sleep(.35)

    def shot(name,output='TEST-1'):
        path=out/(name+'.png'); run(['grim','-o',output,str(path)])
        return Image.open(path).convert('RGB')

    def text(name,output='TEST-1'):
        im=shot(name,output).resize((1280,720)).crop((430,0,850,300)).resize((1260,900))
        path=out/(name+'-text.png'); im.save(path)
        return run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','11'])

    def caption(name,output='TEST-1'):
        im=shot(name,output).resize((1280,720)).crop((320,600,960,720)).resize((1280,240))
        path=out/(name+'-caption.png'); im.save(path)
        return run(['tesseract',str(path),'stdout','--tessdata-dir',TESSDATA,'--psm','11'])

    def clients():
        return json.loads(ctl('-j','clients'))

    def window(app):
        return next(w for w in clients() if w['class']==app)

    def saved():
        return set(out.glob('screenshot*.png'))

    def clear():
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.4)

    def menu(output='TEST-1'):
        clear(); dispatch('hl.dsp.focus({monitor='+json.dumps(output)+'})')
        move(1100,600+(720 if output=='TEST-2' else 0)); key('chord 1 99'); time.sleep(.6)

    def select_window(output='TEST-1',delay=False):
        menu(output); offset=720 if output=='TEST-2' else 0
        click(680,124+offset)
        if delay: click(695,166+offset)
        click(690,213+offset); time.sleep(.7)

    def capture_click(app,output='TEST-1'):
        target=window(app); before=saved(); x,y=target['at']; w,h=target['size']
        move(x+w/2,y+h/2); shot('window-highlight-'+app,output)
        content=caption('window-caption-'+app,output)
        assert ('Capture Red' if app=='window-first' else 'Capture Green') in content,content
        assert 'Enter' in content and 'Tab' in content,content
        send(pointer,'press'); time.sleep(.25)
        assert saved()==before,'Window captured on press'
        send(pointer,'release')
        wait(lambda:bool(saved()-before),'Window screenshot was not saved')
        return Image.open(next(iter(saved()-before))).convert('RGB'),target

    try:
        ctl('reload'); ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('color-scheme-set','community','macOS'); msg('theme-mode-set','dark')
        menu(); assert 'Capture' in text('window-menu-keyboard-shortcut')
        # Arrows select only within the focused group, including its wrapping edge.
        key(106); assert 'Audio' in text('window-menu-arrow-record')
        key(105); assert 'Audio' not in text('window-menu-arrow-screenshot')
        key(15); key(15); key(105)
        assert 'Select Monitor' in text('window-menu-arrow-area-wrap')
        key(106); assert 'Select Region' in text('window-menu-arrow-area-wrap-back')
        for _ in range(3): key(15)
        key(106); shot('window-menu-arrow-delay'); key(105)
        click(735,78); click(570,208); key(105); shot('window-menu-arrow-audio-wrap')
        key(106); click(570,78); key(1); menu()
        # Keyboard selects Window and starts its selector without using the pointer.
        for _ in range(3): key(15)
        key(28); assert 'Select Window' in text('window-menu-keyboard-option')
        key(1)
        first=start(['kitty','--config','NONE','--class','window-first','--title','Capture Red','--override','background=#a02020','-e','sleep','300'],'first-window.log')
        second=start(['kitty','--config','NONE','--class','window-second','--title','Capture Green '+('long window title '*30),'--override','background=#20a040','-e','sleep','300'],'second-window.log')
        wait(lambda:len(clients())==2,'Window fixtures did not map'); time.sleep(.5)
        select_window(); result,target=capture_click('window-first')
        assert result.size==tuple(target['size']),(result.size,target['size'])
        # No Island clock, selector hint, or pointer may leak into the native window image.
        for x,y in ((result.width//2,result.height//2),(result.width//2,12),
                    (result.width//2,result.height-24),(result.width-20,result.height//2)):
            r,g,b=result.getpixel((x,y)); assert r>g*2 and r>b*2,(x,y,r,g,b)
        clear()

        # Enter captures the highlighted window; Tab and Shift+Tab cycle without pointer movement.
        select_window(); key(15); key('shift-tab'); before=saved(); key(28)
        wait(lambda:bool(saved()-before),'Keyboard window capture failed'); clear()

        # Re-query the same window after the countdown, including changed geometry.
        target=window('window-first'); address=target['address']
        dispatch('hl.dsp.window.float({window="address:'+address+'",action="set"})')
        select_window(delay=True); target=window('window-first'); x,y=target['at']; w,h=target['size']
        (out/'windows-before-delay.json').write_text(json.dumps(clients(),indent=2))
        move(x+w/2,y+h/2); shot('window-delayed-selection')
        before=saved(); click(x+w/2,y+h/2)
        assert 'Taking screenshot' in text('window-delayed')
        dispatch('hl.dsp.window.resize({x=500,y=400,window="address:'+address+'"})'); time.sleep(.4)
        target=window('window-first')
        wait(lambda:bool(saved()-before),'Delayed window capture failed')
        result=Image.open(next(iter(saved()-before)))
        assert result.size==tuple(target['size']),(result.size,target['size'])
        clear()

        # A selected window disappearing must not capture its replacement or the desktop.
        select_window(delay=True); target=window('window-first'); x,y=target['at']; w,h=target['size']
        before=saved(); click(x+w/2,y+h/2); first.terminate(); first.wait(timeout=5); time.sleep(5.5)
        assert saved()==before,'Closed window produced a screenshot'
        assert 'no longer available' in text('window-closed'); clear()

        # Window bounds are logical; the captured pixels follow the output's scale and transform.
        target=window('window-second'); address=target['address']
        dispatch('hl.dsp.window.move({window="address:'+address+'",monitor="TEST-2",follow=false})'); time.sleep(.5)
        select_window('TEST-2'); result,target=capture_click('window-second','TEST-2')
        assert all(abs(actual-round(logical*1.5))<=1 for actual,logical in zip(result.size,target['size'])),(result.size,target['size'])
        clear(); menu('TEST-2'); click(680,844); click(690,933); time.sleep(.5)
        before=saved(); key(1); time.sleep(.5); assert saved()==before

        # Window is screenshot-only; selecting Record switches its target to Region.
        menu(); click(680,124); click(735,78)
        content=text('window-record-mode'); assert 'Window' not in content and 'Audio' in content,content
        key(1)
        # Hovering media keeps the footer free of the capture entry and its empty row.
        env['ISLAND_TEST_ART']=(REPO/'assets/noctalia-wallpaper.png').as_uri()
        start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'window-player.log'); time.sleep(1)
        move(1100,600); time.sleep(.5); move(640,40); time.sleep(1)
        assert 'Island Test Player' in text('window-hover-clean')
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: Shift+Print, arrow groups and wrapping, keyboard window option, window captions and long titles, highlighted click/release, keyboard selection, delayed resize, closed window, scaled/rotated output, cancellation, Record-only audio and clean hover',flush=True)
    finally:
        for proc in (pointer,keyboard):
            proc.terminate(); proc.wait(timeout=5)
