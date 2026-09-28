#!/usr/bin/env python3
"""Exercise the native island on private Wayland and D-Bus sessions.

Requires Sway, grim, Python GObject bindings, and a built Noctalia binary.
No installed desktop services are stopped or reconfigured.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parent.parent


def wait_for(predicate, message, timeout=12):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.1)
    raise RuntimeError(message)


def worker(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    processes = []
    (out/'player-actions.log').unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix='ni-') as tmp:
        base = Path(tmp)
        runtime = base / 'run'
        runtime.mkdir(mode=0o700)
        config = base / 'config' / 'noctalia'
        config.mkdir(parents=True)
        profile = (REPO / 'examples/orbit-island.toml').read_text()
        profile = profile.replace('[wallpaper]\nenabled = false', '[wallpaper]\nenabled = true')
        (config / 'config.toml').write_text(profile)
        swayconfig = base / 'sway.conf'
        swayconfig.write_text('output HEADLESS-1 mode 1280x720\nseat seat0 fallback true\n')
        env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base/'config'),
                   XDG_STATE_HOME=str(base/'state'), XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'),
                   NOCTALIA_CONFIG_HOME=str(base/'config'), NOCTALIA_STATE_HOME=str(base/'state'),
                   NOCTALIA_DATA_HOME=str(base/'data'), GSETTINGS_BACKEND='memory',
                   WLR_BACKENDS='headless', WLR_RENDERER='pixman', WLR_LIBINPUT_NO_DEVICES='1',
                   LIBGL_ALWAYS_SOFTWARE='1', XDG_CURRENT_DESKTOP='sway',
                   ISLAND_TEST_EVENTS=str(out/'player-actions.log'),
                   ISLAND_TEST_ART=(REPO/'assets/noctalia-wallpaper.png').as_uri())
        env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        def start(command, name):
            with (out/name).open('w') as log:
                p = subprocess.Popen(command, env=dict(env, WAYLAND_DEBUG='1') if name == 'noctalia.log' else env, stdout=log, stderr=log)
            processes.append(p)
            return p
        def run(command):
            return subprocess.run(command, env=env, check=True, text=True, capture_output=True, timeout=8).stdout
        try:
            compositor = start([str(args.sway), '-c', str(swayconfig)], 'sway.log')
            wait_for(lambda: list(runtime.glob('wayland-*.lock')), 'Sway did not start')
            env['WAYLAND_DISPLAY'] = next(runtime.glob('wayland-*.lock')).name.removesuffix('.lock')
            env['SWAYSOCK'] = str(next(runtime.glob('sway-ipc.*.sock')))
            protocol = REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
            run(['wayland-scanner','client-header',str(protocol),str(base/'pointer-client.h')])
            run(['wayland-scanner','private-code',str(protocol),str(base/'pointer-code.c')])
            run(['cc','-I'+str(base),str(REPO/'tests/fixtures/island_pointer.c'),str(base/'pointer-code.c'),'-lwayland-client','-o',str(base/'pointer')])
            pointer = subprocess.Popen([str(base/'pointer')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(pointer)
            def pointer_command(command):
                pointer.stdin.write(command+'\n'); pointer.stdin.flush()
                assert pointer.stdout.readline().strip() == 'ok'
            keyboard_protocol = REPO/'protocols/virtual-keyboard-unstable-v1.xml'
            run(['wayland-scanner','client-header',str(keyboard_protocol),str(base/'keyboard-client.h')])
            run(['wayland-scanner','private-code',str(keyboard_protocol),str(base/'keyboard-code.c')])
            run(['cc','-I'+str(base),str(REPO/'tests/fixtures/island_keyboard.c'),str(base/'keyboard-code.c'),'-lwayland-client','-lxkbcommon','-o',str(base/'keyboard')])
            keyboard = subprocess.Popen([str(base/'keyboard')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
            processes.append(keyboard)
            def key(code):
                keyboard.stdin.write(str(code)+'\n'); keyboard.stdin.flush()
                assert keyboard.stdout.readline().strip() == 'ok'
            pointer_command('move 1000 600')
            shell = start([str(args.binary)], 'noctalia.log')
            wait_for(lambda: (runtime/f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(), 'Noctalia did not start')
            def msg(*words): return run([str(args.binary),'msg',*words])
            def sway(command): return run([str(args.sway.parent/'swaymsg'),command])
            def cursor(x,y): pointer_command(f'move {x} {y}')
            def click():
                pointer_command('press')
                pointer_command('release')
            def shot(name):
                time.sleep(.65)
                assert shell.poll() is None, 'Noctalia exited; see noctalia.log'
                run(['grim',str(out/f'{name}.png')])
            def events():
                path = out/'player-actions.log'
                return path.read_text() if path.exists() else ''
            time.sleep(1)
            cursor(1000,600)
            shot('01-rest')
            time.sleep(3)  # Let the initial keyboard-layout OSD expire before hover.
            cursor(640,42)
            shot('02-calendar')
            cursor(1000,600)
            # Open from the pill, dismiss through the real keyboard and pointer,
            # then switch panel types on the same layer surface.
            cursor(640,42)
            time.sleep(.3)
            click()
            shot('13-island-controls')
            key(1)  # Escape
            cursor(1000,600)
            shot('14-island-collapsed')
            for context in ('calendar', 'media', 'notifications', 'audio'):
                msg('panel-open','control-center',context)
                shot('15-panel-'+context)
            from PIL import Image
            def card_bottom(name):
                with Image.open(out/(name+'.png')) as capture:
                    return max(y for y in range(9,705) if max(capture.getpixel((640,y))[:3]) < 12)
            assert card_bottom('15-panel-notifications') + 80 < card_bottom('13-island-controls'), 'Empty page did not shrink'
            msg('panel-open','control-center','notifications')
            shot('21-fit-empty')
            empty_bottom = card_bottom('21-fit-empty')
            cursor(715, 55)  # Calendar, reached from the compact notifications page.
            click()
            shot('28-calendar-from-notifications')
            calendar_bottom = card_bottom('28-calendar-from-notifications')
            assert calendar_bottom > empty_bottom + 150, 'Calendar inherited the short notifications height'
            assert abs(calendar_bottom - card_bottom('15-panel-calendar')) <= 2, 'Calendar size depends on previous tab'
            cursor(755, 55)  # Notifications must shrink again.
            click()
            shot('29-notifications-from-calendar')
            assert abs(card_bottom('29-notifications-from-calendar') - empty_bottom) <= 2, 'Notifications did not shrink after calendar'
            cursor(675, 55)  # Weather must also measure independently of notifications.
            click()
            shot('40-weather-from-notifications')
            weather_bottom = card_bottom('40-weather-from-notifications')
            msg('panel-open', 'control-center', 'home')
            shot('41-home-before-weather')
            cursor(675, 55)
            click()
            shot('42-weather-from-home')
            assert abs(card_bottom('42-weather-from-home') - weather_bottom) <= 2, 'Weather size depends on previous tab'
            assert weather_bottom > empty_bottom + 100, 'Weather inherited the short notifications height'
            msg('panel-open', 'control-center', 'notifications')
            shot('43-notifications-after-weather')
            # Audio is the third icon in the top navigation. It must remain
            # clickable after the empty page has collapsed to its smaller size.
            cursor(437, 55)
            click()
            shot('22-top-navigation-audio')
            assert card_bottom('22-top-navigation-audio') > empty_bottom + 30, 'Top navigation did not switch to audio'
            msg('panel-open','control-center','notifications')
            for index in range(5):
                run(['gdbus','call','--session','--dest','org.freedesktop.Notifications',
                     '--object-path','/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.Notify',
                     'Fit test','0','',f'Content row {index}','The card should grow with its notification history.',
                     '[]','{}','10000'])
            shot('23-fit-populated')
            assert card_bottom('23-fit-populated') > empty_bottom + 100, 'New content did not grow the open card'
            msg('notification-clear-active')
            msg('notification-clear-history')
            shot('24-fit-cleared')
            assert abs(card_bottom('24-fit-cleared')-empty_bottom) <= 2, 'Clearing content did not restore the compact height'
            time.sleep(1.2)
            shot('25-fit-stable')
            assert abs(card_bottom('25-fit-stable')-empty_bottom) <= 2, 'Content fit height drifted while idle'
            cursor(1000,600)
            msg('panel-open','launcher')
            key(30)  # type a into the focused launcher search
            shot('16-island-launcher')
            msg('panel-open','launcher','__island_no_matching_application__')
            shot('26-empty-launcher')
            assert card_bottom('26-empty-launcher') + 100 < card_bottom('16-island-launcher'), 'Launcher did not shrink with its results'
            key(1)
            shot('17-launcher-dismissed')
            msg('panel-open','control-center')
            time.sleep(.6)
            cursor(1100,650)
            click()
            shot('18-outside-dismissed')
            log = (out/'noctalia.log').read_text()
            assert '"noctalia-panel"' not in log, 'Opened a separate panel surface instead of borrowing the island'
            assert '"noctalia-attached-panel"' not in log
            from PIL import Image, ImageChops
            with Image.open(out/'01-rest.png') as rest, Image.open(out/'13-island-controls.png') as controls:
                assert ImageChops.difference(rest.crop((400,250,880,550)),controls.crop((400,250,880,550))).getbbox() is not None, 'Control centre did not become visible'
            for name in ('14-island-collapsed', '17-launcher-dismissed', '18-outside-dismissed'):
                with Image.open(out/'01-rest.png') as rest, Image.open(out/(name+'.png')) as collapsed:
                    assert ImageChops.difference(rest.crop((300,250,980,590)),collapsed.crop((300,250,980,590))).getbbox() is None, name+' left panel content visible'
            cursor(1000,600)
            start([sys.executable,str(REPO/'tests/fixtures/island_player.py')],'player.log')
            time.sleep(2)
            shot('03-activity')
            msg('panel-open','control-center')
            time.sleep(.6)
            msg('panel-close')
            collapse_widths = []
            collapse_centers = []
            # Check every captured transition frame: the playing pill must never
            # shrink through the narrow idle-clock width before widening again.
            for frame in range(16):
                path = out/f'collapse-playing-{frame:02}.png'
                run(['grim',str(path)])
                with Image.open(path) as capture:
                    def dark(x): return max(capture.getpixel((x,20))[:3]) < 12
                    left = right = 640
                    while left > 200 and dark(left-1): left -= 1
                    while right < 1080 and dark(right+1): right += 1
                    collapse_widths.append(right-left+1)
                    collapse_centers.append((right+left)/2)
                time.sleep(.035)
            assert all(abs(center-639.5) <= 1 for center in collapse_centers), f'Capsule moved sideways: {collapse_centers}'
            assert min(collapse_widths) >= 250, f'Collapse undershot the playing pill: {collapse_widths}'
            assert collapse_widths[-1] < 330, f'Panel did not finish collapsing: {collapse_widths}'
            import re
            trace = (out/'noctalia.log').read_text()
            island_role = re.search(r'get_layer_surface\(new id zwlr_layer_surface_v1#(\d+),[^\n]*"noctalia-island"', trace)
            assert island_role, 'No island layer-surface in protocol trace'
            sizes = re.findall(r'zwlr_layer_surface_v1#'+island_role.group(1)+r'\.set_size\((\d+), (\d+)\)',trace)
            assert sizes and len(set(sizes)) == 1, f'Island viewport resized during transitions: {sizes}'
            msg('panel-open','control-center')
            time.sleep(.12)
            msg('panel-close')
            shot('20-interrupted-opening')
            cursor(640,42)
            shot('04-media')
            cursor(640,183)
            click()
            wait_for(lambda: 'PlayPause' in events(), 'Pause button did not invoke MPRIS')
            shot('05-paused-hover')
            click()
            cursor(550,125)
            pointer_command('press')
            cursor(700,125)
            pointer_command('release')
            wait_for(lambda: 'SetPosition' in events(), 'Seeking did not invoke MPRIS')
            cursor(1000,600)
            msg('brightness-osd','63')
            shot('06-osd')
            msg('brightness-osd','42')
            shot('07-brightness')
            run(['gdbus','call','--session','--dest','org.freedesktop.Notifications',
                 '--object-path','/org/freedesktop/Notifications','--method','org.freedesktop.Notifications.Notify',
                 'Orbit test','0','','Your island is ready','Native Noctalia services, familiar Orbit behaviour.',
                 '[]','{}','10000'])
            shot('08-notification')
            msg('brightness-osd','24')
            shot('09-notification-priority')
            from PIL import Image, ImageChops
            with Image.open(out/'08-notification.png') as first, Image.open(out/'09-notification-priority.png') as second:
                assert ImageChops.difference(first.crop((300,0,980,220)),second.crop((300,0,980,220))).getbbox() is None, 'Hidden OSD changed the visible notification'
            msg('notification-dnd-set','on')
            shot('10-dnd')
            msg('notification-dnd-set','off')
            msg('panel-open','control-center')
            time.sleep(.6)
            msg('panel-close')
            (config/'config.toml').write_text(profile.replace('[island]\nenabled = true','[island]\nenabled = false'))
            time.sleep(.4)
            shot('11-disabled')
            msg('panel-open','launcher')
            shot('19-disabled-panel-fallback')
            key(1)
            time.sleep(.5)
            assert '"noctalia-panel"' in (out/'noctalia.log').read_text(), 'Disabled island did not restore the normal panel host'
            (config/'config.toml').write_text(profile)
            time.sleep(.4)
            shot('12-reenabled')
            # Home preferences survive close/reopen and each layout fits its visible cards.
            def home_profile(name, cards, stacked=False):
                import json
                msg('panel-close')
                time.sleep(.5)
                (config/'config.toml').write_text(profile + '\n[control_center.home]\ncards = '
                    + json.dumps(cards) + '\nstacked = ' + str(stacked).lower() + '\n')
                time.sleep(.5)
                msg('panel-open', 'control-center')
                shot(name)
                return card_bottom(name)
            full_home = home_profile('30-home-default', ['profile', 'media', 'clock', 'shortcuts'])
            clock_home = home_profile('31-home-clock-only', ['clock'])
            assert clock_home + 150 < full_home, 'Hidden Home cards still reserve space'
            empty_home = home_profile('32-home-empty', [])
            assert empty_home < 250, 'Empty Home layout did not stay compact'
            stacked_home = home_profile('33-home-stacked', ['clock', 'media', 'shortcuts', 'profile'], True)
            assert stacked_home > clock_home + 150, 'Stacked Home lost its cards'
            two_cards = home_profile('34-home-two-cards', ['clock', 'media'], True)
            msg('panel-close')
            time.sleep(.5)
            msg('panel-open', 'control-center')
            shot('35-home-reopened')
            assert abs(card_bottom('35-home-reopened') - two_cards) <= 2, 'Home layout did not persist on reopen'
            duplicate_cards = home_profile('36-home-duplicate-cards', ['clock', 'media', 'clock', 'unknown'], True)
            assert abs(duplicate_cards - two_cards) <= 2, 'Unknown or repeated Home cards alter the layout'
            home_profile('37-home-reversed', ['media', 'clock'], True)
            assert home_profile('38-home-shortcuts-only', ['shortcuts']) < 400, 'Standalone shortcuts overflow their card'
            cursor(842, 55)  # Home settings action opens the customisation section.
            click()
            shot('39-home-settings')
            msg('settings-close')
            assert compositor.poll() is None
            print('PASS: embedded panels, keyboard/outside dismissal, switching, media pause/resume, seeking, OSD commands, notifications, DND, and config reload; captures in',out)
        finally:
            for p in reversed(processes):
                p.terminate()
            for p in reversed(processes):
                try: p.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    p.kill(); p.wait()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,default=REPO/'build-island/noctalia')
    parser.add_argument('--sway',type=Path,default=Path(shutil.which('sway') or REPO/'.deps/usr/bin/sway'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--worker',action='store_true')
    args=parser.parse_args()
    args.binary=args.binary.resolve();args.sway=args.sway.resolve()
    if args.worker: worker(args)
    else:
        raise SystemExit(subprocess.call(['dbus-run-session','--',sys.executable,__file__,*sys.argv[1:],'--worker']))

if __name__=='__main__':main()
