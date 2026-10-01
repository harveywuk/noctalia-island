"""Private two-monitor activity ordering, cycling and interruption checks."""
import json
import os
import pathlib
import subprocess
import sys
import time
from PIL import Image, ImageChops, ImageStat


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base / 'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base / 'pointer-code.c')])
    run(['cc', '-I' + str(base), str(repo / 'tests/fixtures/island_pointer.c'),
         str(base / 'pointer-code.c'), '-lwayland-client', '-o', str(base / 'pointer')])
    pointer = subprocess.Popen([str(base / 'pointer')], env=env, stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, text=True)
    publisher = subprocess.Popen([sys.executable, str(repo / 'tests/fixtures/island_downloads.py')],
                                 env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    try:
        pointer.stdin.write('move 1100 600\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'
        _checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, repo, publisher)
    finally:
        for proc in (pointer, publisher):
            proc.terminate(); proc.wait(timeout=5)


def _checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, repo, publisher):
    hypr = base / 'hyprland.lua'
    hypr.write_text(hypr.read_text().replace(
        'mode="1920x1080@60",position="0x720",scale=1.5,transform=2',
        'mode="1280x720@60",position="0x720",scale=1,transform=0'))
    ctl('reload'); time.sleep(1)
    plugin = base / 'plugins/timer'; plugin.mkdir(parents=True)
    (plugin / 'plugin.toml').write_text('id="noctalia/timer"\nname="Cycle test timer"\nversion="1.0.0"\nplugin_api=3\n[[service]]\nid="timer"\nentry="timer.luau"\n')
    (plugin / 'timer.luau').write_text((repo / 'tests/fixtures/island_progress_timer.luau').read_text())
    baseline = (cfg / 'config.toml').read_text().replace('[island]\nenabled=true', '[island]\nenabled=false')
    baseline = baseline.split('[plugins]')[0] + '\n[plugins]\nauto_update="none"\nenabled=["noctalia/timer"]\n[[plugins.source]]\nname="test"\nkind="path"\nlocation=' + json.dumps(str(plugin.parent)) + '\nenabled=true\n'
    baseline += '\n[shell.animation]\nenabled=false\n[bar]\norder=["capsule","default"]\n[bar.capsule]\npresentation="island"\nreserve_space=false\n'

    def move(x, y): dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
    def timer(state='PAUSED'): msg('plugin', 'noctalia/timer:timer', 'all', state, '75')
    def configure(order, cycle=False, seconds=3):
        move(1100, 600)
        (cfg / 'config.toml').write_text(baseline + '\n[bar.capsule.island]\ntrack_preview_seconds=0\nactivity_priority=' + json.dumps(order) + '\ncycle_activities=' + str(cycle).lower() + '\nactivity_cycle_seconds=' + str(seconds) + '\n[bar.capsule.monitor.TEST-2.island]\nactivity_priority="media-downloads-timers"\ncycle_activities=false\n')
        msg('config-reload'); time.sleep(.6); timer(); time.sleep(.3)
    def download(visible=True, progress=.25):
        publisher.stdin.write(json.dumps({'uri': 'application://cycle-test.desktop',
                                         'properties': {'progress': progress, 'progress-visible': visible}}) + '\n')
        publisher.stdin.flush(); assert publisher.stdout.readline().strip() == 'ok'
    def shot(name, output='TEST-1'):
        path = out / (name + '-' + output + '.png')
        run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')
    def icon(picture): return picture.crop((480, 20, 555, 62))
    templates = {}
    def check(name, expected, output='TEST-1'):
        picture = icon(shot(name, output))
        scores = {key: sum(ImageStat.Stat(ImageChops.difference(value, picture)).mean)
                  for key, value in templates.items()}
        actual = min(scores, key=scores.get)
        assert actual == expected and scores[actual] < 3, (name, expected, scores)

    msg('theme-mode-set', 'dark')
    env['ISLAND_TEST_ART'] = (repo / 'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_EVENTS'] = str(out / 'cycle-player-actions.log')
    player = start([sys.executable, str(repo / 'tests/fixtures/island_player.py')], 'cycle-player.log')
    time.sleep(.7); download()
    for activity, order in [('media', 'media-downloads-timers'), ('downloads', 'downloads-media-timers'),
                            ('timers', 'timers-downloads-media')]:
        configure(order)
        templates[activity] = icon(shot('priority-' + activity))
    assert all(sum(ImageStat.Stat(ImageChops.difference(a, b)).mean) > 5
               for i, a in enumerate(templates.values()) for b in list(templates.values())[i + 1:])

    configure('timers-downloads-media', True, 3)
    check('cycle-first', 'timers'); check('monitor-override', 'media', 'TEST-2')
    time.sleep(3); check('cycle-second', 'downloads')
    time.sleep(3); check('cycle-third', 'media')
    time.sleep(3); check('cycle-wrap', 'timers')
    check('monitor-still-fixed', 'media', 'TEST-2')
    # Hover selection stays stable for longer than a cycle, and closing restores it.
    move(640, 40); time.sleep(.5)
    before = shot('hover-before').crop((460, 80, 820, 125))
    time.sleep(4)
    after = shot('hover-after').crop((460, 80, 820, 125))
    assert ImageChops.difference(before, after).getbbox() is None, 'Cycling changed hover controls'
    move(1100, 600); time.sleep(.5); check('hover-return', 'timers')
    # A critical alert interrupts and pauses cycling until dismissed.
    notification = run(['notify-send', '-p', '-u', 'critical', '-t', '0', 'Cycle alert', 'Keep activity timing paused']).strip()
    time.sleep(4)
    alert = shot('critical-alert')
    assert max(alert.getpixel((440, 60))) < 45, 'Alert did not take priority'
    run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
         '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.CloseNotification', notification])
    time.sleep(.4); check('after-alert', 'timers')
    # Disable a current activity and skip it immediately.
    timer('IDLE'); time.sleep(1.2); check('timer-ended', 'downloads')
    time.sleep(3); check('skip-inactive', 'media')
    download(False); time.sleep(.5); check('only-media', 'media')
    time.sleep(3); check('no-rotation-with-one', 'media')
    assert shell.poll() is None and not ctl('configerrors').strip()
    move(640, 40); time.sleep(.15); move(1100, 600)
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('settings-open', 'bar'); time.sleep(.8)
    shot('activity-settings')
    (out / 'inspect-env.json').write_text(json.dumps(env))
    if os.environ.get('NOCTALIA_TEST_CYCLE_INSPECT'):
        print('INSPECT: activity settings ready', flush=True)
        deadline = time.monotonic() + int(os.environ['NOCTALIA_TEST_CYCLE_INSPECT'])
        while time.monotonic() < deadline and not (out / 'inspect-done').exists(): time.sleep(.25)
    print('PASS: three activity priorities, ordered cycling, independent monitor override, '
          'hover stability, critical alert priority, resume and inactive activity removal', flush=True)
