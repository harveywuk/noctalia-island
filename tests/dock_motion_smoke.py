"""Dock behaviour and shell transitions in a private, two-output Hyprland session."""
import json
import pathlib
import subprocess
import time

from PIL import Image, ImageChops, ImageStat


def prepare(base, cfg, env):
    apps = pathlib.Path(env['XDG_DATA_HOME'])/'applications'
    apps.mkdir(parents=True, exist_ok=True)
    launch = base/'launch-test-app'
    launch.write_text('#!/bin/sh\nsleep 0.9\nexec kitty --config NONE --class dock-motion-test --title "Dock motion test" /bin/sh\n')
    launch.chmod(0o755)
    for app, name, icon, command in (
        ('dock-motion-test', 'Motion Test', 'utilities-terminal', str(launch)),
        ('dock-second-test', 'Another application with a deliberately long display name', 'system-file-manager', '/usr/bin/true'),
    ):
        (apps/(app+'.desktop')).write_text(f'[Desktop Entry]\nType=Application\nName={name}\nExec={command}\nIcon={icon}\nStartupWMClass={app}\n')
    config = (cfg/'config.toml').read_text().replace('[shell]\n', '[shell]\noffline_mode=true\n')
    config = config.replace('[dock]\nenabled=false', '''[dock]
enabled=true
position="bottom"
smart_auto_hide=true
reserve_space=false
icon_size=48
active_scale=1
inactive_scale=1
active_opacity=1
inactive_opacity=1
margin_edge=12
radius=18
concave_edge_corners=false
show_dots=true
show_instance_count=false
launcher_position="start"
hide_delay_ms=400
pinned=["dock-motion-test", "dock-second-test"]''')
    config += '\n[shell.animation]\nenabled=true\nspeed=0.75\n'
    (cfg/'config.toml').write_text(config)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
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
    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'
    def move(x, y, delay=.6):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})'); time.sleep(delay)
    def click(x, y, right=False):
        move(x, y)
        command(pointer, 'right-press' if right else 'press'); time.sleep(.05)
        command(pointer, 'right-release' if right else 'release')
    def shot(name, output='TEST-1'):
        dest = out/(name+'.png'); run(['grim', '-o', output, str(dest)])
        return Image.open(dest).convert('RGB')
    def clients(): return [c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'dock-motion-test']
    def score(a, b, rect): return sum(ImageStat.Stat(ImageChops.difference(a.crop(rect), b.crop(rect))).mean)
    def config_change(old, new):
        config = cfg/'config.toml'; current = config.read_text(); assert old in current
        config.write_text(current.replace(old, new)); msg('config-reload'); time.sleep(.7)
    def dock_count(output):
        return sum(x['namespace'] == 'noctalia-dock' for level in json.loads(ctl('-j', 'layers'))[output]['levels'].values() for x in level)
    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        move(1100, 400); time.sleep(.8)
        assert dock_count('TEST-1') == dock_count('TEST-2') == 1
        rest = shot('dock-dark-rest')
        move(640, 670); magnified = shot('dock-dark-hover')
        assert score(rest, magnified, (610, 605, 674, 697)) > 3, 'Dock did not magnify'
        command(pointer, 'press'); pressed = shot('dock-dark-pressed'); command(pointer, 'release')
        assert score(magnified, pressed, (610, 605, 674, 697)) > 3, 'No press feedback'
        time.sleep(.2); shot('dock-launch-bounce')
        wait(lambda: len(clients()) == 1, 'Dock did not launch the pinned app')
        move(1100, 400); time.sleep(.6); hidden = shot('dock-smart-hidden')
        move(640, 719); shown = shot('dock-edge-reveal')
        # The slab remains stable while magnification settles during the grace period.
        move(1100, 400, .01); grace = shot('dock-hide-grace')
        slab = (549, 695, 560, 700)
        assert score(shown, hidden, slab) > 5, 'Smart hide did not retract the dock'
        assert score(shown, grace, slab) < 3, 'Dock hid before its grace period elapsed'
        move(640, 670); time.sleep(.5); returned = shot('dock-hide-cancelled')
        assert score(shown, returned, slab) < 3, 'Re-entry did not cancel hide'

        click(640, 670, right=True); time.sleep(.3); shot('dock-context-menu')
        move(1000, 350); time.sleep(.6); menu_held = shot('dock-menu-keeps-visible')
        assert score(shown, menu_held, slab) < 3, 'Dock hid while its app menu was open'
        click(1000, 350); time.sleep(.8)

        second = start(['kitty', '--config', 'NONE', '--class', 'dock-motion-test', '--title',
                        'Second window with a very long title for the dock menu', '/bin/sh'], 'dock-second-window.log')
        wait(lambda: len(clients()) == 2, 'Second app window did not appear')
        move(640, 719); click(640, 670); time.sleep(.4)
        first_active = json.loads(ctl('-j', 'activewindow'))['address']
        click(640, 670); time.sleep(.4)
        assert json.loads(ctl('-j', 'activewindow'))['address'] != first_active, 'Dock did not cycle app windows'
        move(1100, 400); move(640, 719)
        click(640, 670, right=True); time.sleep(.3); shot('dock-long-window-menu'); click(1050, 400)

        # Changing motion while the pointer is over the dock must remove magnification.
        move(1100, 400); move(640, 719); move(640, 670); before = shot('dock-motion-on')
        config_change('[shell.animation]\nenabled=true', '[shell.animation]\nenabled=false')
        reduced = shot('dock-reduced-motion')
        assert score(before, reduced, (610, 605, 674, 697)) > 3, 'Live reduced-motion change left magnification active'
        msg('panel-open', 'control-center', 'home'); time.sleep(.2); shot('panel-reduced-motion')
        msg('panel-close'); time.sleep(.2)
        config_change('[shell.animation]\nenabled=false', '[shell.animation]\nenabled=true')

        # Interrupt open/close transitions repeatedly, then verify a usable final surface.
        move(1100, 400)
        for _ in range(3):
            msg('panel-open', 'control-center', 'home'); time.sleep(.06)
            msg('panel-close'); time.sleep(.06)
        time.sleep(.6); move(640, 40, .12); shot('island-morph'); time.sleep(.5); shot('island-expanded')
        msg('panel-open', 'control-center', 'home'); time.sleep(.6); shot('panel-motion-open')
        msg('panel-close'); time.sleep(.6); move(1100, 400)

        move(640, 719); click(574, 670); time.sleep(.6); shot('launcher-from-dock')
        msg('panel-close'); time.sleep(.4)

        msg('theme-mode-set', 'light'); time.sleep(.6)
        move(640, 719); move(706, 670); shot('dock-light-hover-long-label')
        dispatch('hl.dsp.focus({monitor="TEST-2"})'); move(640, 1439); move(640, 1390)
        shot('dock-fractional-monitor', 'TEST-2')
        assert dock_count('TEST-1') == dock_count('TEST-2') == 1
        assert shell.poll() is None and not ctl('configerrors').strip()
        print('PASS: dock launch, press, magnification, smart hide, grace/cancel, app menus, window cycling, '
              'live reduced motion, interrupted Island/panel transitions, long labels and fractional output', flush=True)
    finally:
        for client in clients():
            dispatch('hl.dsp.window.close({window="address:'+client['address']+'"})')
        for proc in helpers:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
