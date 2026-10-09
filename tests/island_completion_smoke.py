"""Transfer notices, pause/resume, interruptions and return to live activities.

Opt-in GPU test; run through hyprland_smoke.py on its private compositor/bus.
"""
import json
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
media_gradient=true
volume_show_percentage=true
track_preview_seconds=0
split_activities=false
activity_priority="downloads-media-timers"''')
    path.write_text(text.replace('[shell]\n', '[shell]\noffline_mode=true\n'))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    original = (cfg/'config.toml').read_text()
    for kind, protocol, libs in (
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
             *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True))
    pointer, keyboard = helpers
    publisher = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_downloads.py')], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    helpers.append(publisher)

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def move(x=1100, y=600):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0')
        time.sleep(.6)

    def progress(name, value, visible=True):
        command(publisher, json.dumps({'uri': 'application://'+name+'.desktop',
                                       'properties': {'progress': value, 'progress-visible': visible}}))

    def finish(name='short'):
        progress(name, .25)
        time.sleep(.15)
        began = time.monotonic()
        progress(name, 1.0, False)
        return began

    def at(began, seconds):
        time.sleep(max(0, began+seconds-time.monotonic()))

    def shot(name, output='TEST-1'):
        path = out/(name+'.png')
        run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def text(name, region=(420, 0, 860, 310), psm='11'):
        crop = shot(name).crop(region)
        crop = crop.resize((crop.width*3, crop.height*3))
        # Read muted percentages in colour, then separate brighter labels from
        # the moving artwork. One threshold cannot preserve both text weights.
        words = []
        for suffix, sample in (('text', crop), ('labels', crop.convert('L').point(lambda p: 255 if p > 110 else 0))):
            path = out/(name+'-'+suffix+'.png'); sample.save(path)
            words.append(run(['tesseract', str(path), 'stdout', '--tessdata-dir',
                              os.environ.get('NOCTALIA_TEST_TESSDATA', str(repo/'build-rishot/test-data/tessdata')),
                              '--psm', psm, '-c', 'user_defined_dpi=288']).lower())
        return '\n'.join(words)

    def transition(name, trigger):
        event_time = trigger()
        began = time.monotonic()
        for index, offset in enumerate((0, .07, .14, .24, .4, .6, .85)):
            at(began, offset)
            image = shot(f'{name}-{index}')
            interior = image.crop((560, 16, 720, 24))
            assert sum(max(pixel) > 12 for pixel in interior.getdata()) > 640, (name, index, 'Artwork flashed black')
        return event_time

    # The centred icon moves with the measured width of each result label.
    def green(image, crop=(515, 22, 615, 60), threshold=70):
        return sum(g > threshold and g > r+15 and g > b+15
                   for r, g, b in image.crop(crop).getdata())

    def present(name):
        image = shot(name)
        assert green(image) > 40, (name, 'Green completion checkmark is missing')
        return image

    def absent(name):
        assert green(shot(name)) < 10, (name, 'Completion checkmark remained or replayed')

    def red(image, crop=(515, 22, 615, 60), threshold=70):
        # The translucent halo blends with the blue desktop, so allow the same
        # channel separation as the green halo instead of requiring opaque red.
        return sum(r > threshold and r > g+15 and r > b+15
                   for r, g, b in image.crop(crop).getdata())

    def failure(name):
        image = shot(name)
        assert red(image) > 40, (name, 'Red failure cross is missing')
        return image

    def activity(status, **fields):
        msg('island-activity-update', json.dumps({'id': 'backup', 'status': status, **fields}))

    def media_ready(name):
        # Stay above the labels and to the right of the artwork sample strip;
        # this nested compositor can include its software cursor in a capture.
        move(735, 12)
        assert 'completion fixture' in text(name), 'Did not return to the current media player'
        move()

    def media(method):
        run(['gdbus', 'call', '--session', '--dest', 'org.mpris.MediaPlayer2.islandtest', '--object-path',
             '/org/mpris/MediaPlayer2', '--method', 'org.mpris.MediaPlayer2.Player.'+method])

    def artwork(name):
        # Interior above the label: excludes the green checkmark, halo and text,
        # so their animation cannot hide a missing or frozen artwork background.
        crop = (560, 16, 720, 24)
        before = shot(name+'-before').crop(crop)
        time.sleep(.45)
        after = shot(name+'-after').crop(crop)
        assert sum(max(pixel) > 12 for pixel in after.getdata()) > 640, (name, 'Artwork became black')
        assert ImageChops.difference(before, after).getbbox(), (name, 'Artwork stopped animating')

    def events():
        path = out/'player-events.txt'
        return path.read_text() if path.exists() else ''

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move()
        msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
        env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
        env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
        env['ISLAND_TEST_TITLE'] = 'Completion fixture'
        start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
        time.sleep(2.5)
        media_ready('initial-media')

        # Sample the spring resize itself, including an interruption while the
        # media card is expanded, rather than checking only its settled frames.
        transition('media-to-transfer', lambda: progress('handoff', .32))
        transition('transfer-to-media', lambda: progress('handoff', .32, False))
        move(735, 12)
        began = transition('expanded-media-to-notice', lambda: finish('morph'))
        at(began, 4.85)
        transition('notice-to-expanded-media', lambda: None)
        assert 'completion fixture' in text('morph-return-media')
        move()

        # Check timing against the event, not against OCR or screenshot duration.
        began = finish(); at(began, .8)
        image = present('completion-green')
        assert green(image, (565, 73, 715, 80), threshold=25) > 40, 'Green halo is missing outside the capsule'
        assert 'download finished' in text('completion-label')
        artwork('completion-artwork')
        at(began, 3)
        progress('short', 1.0, False)  # Duplicate success must not restart the timer.
        at(began, 4.2); present('completion-before-expiry')
        assert time.monotonic() < began+5, 'Timing probe ran too slowly to check the five-second boundary'
        at(began, 5.7); absent('completion-expired'); media_ready('expired-media')

        # Each distinct completion refreshes the same notice, without a queue of old cards.
        began = finish('first'); at(began, 3)
        latest = finish('second')
        at(began, 5.7); present('overlapping-finishes')
        artwork('overlapping-artwork')
        at(latest, 5.7); absent('overlapping-expired')

        progress('remaining', .37)
        began = finish('alongside'); at(began, 5.7)
        absent('remaining-download-return'); move(735, 12)
        remaining = text('remaining-download')
        assert 'remaining' in remaining and '37' in remaining, remaining
        artwork('remaining-download-artwork')
        move(); progress('remaining', .37, False); time.sleep(.6)
        absent('cancelled-download'); media_ready('cancelled-media')

        began = finish('volume'); at(began, .7)
        msg('volume-osd', '65'); time.sleep(.4)
        absent('volume-interrupts-completion')
        assert '65' in text('volume-level')
        artwork('volume-artwork')
        at(began, 3.4); present('completion-after-volume')
        at(began, 5.7); absent('volume-return-expired')

        began = finish('notification'); at(began, .7)
        run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
             '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
             'Mail', '0', '', 'Priority notice', 'Completion must expire behind this alert.',
             '[]', "{'urgency': <byte 2>}", '0'])
        time.sleep(.6)
        assert 'priority notice' in text('notification-interrupts-completion')
        absent('notification-hides-completion')
        artwork('notification-artwork')
        at(began, 5.7)
        msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.6)
        absent('notification-does-not-replay-completion'); media_ready('notification-return-media')

        msg('panel-open', 'control-center', 'audio'); time.sleep(.7)
        began = finish('panel'); at(began, .7)
        assert json.loads(msg('status'))['activePanelId'] == 'control-center'
        assert 'download finished' not in text('panel-retains-control')
        at(began, 5.7); msg('panel-close'); time.sleep(.8)
        absent('panel-does-not-replay-completion'); media_ready('panel-return-media')

        msg('island-focus'); time.sleep(.6); command(keyboard, 15)
        before = events().count('PlayPause')
        began = finish('keyboard'); at(began, .6)
        assert 'download finished' not in text('keyboard-retains-control')
        command(keyboard, 28); time.sleep(.4)
        assert events().count('PlayPause') == before+1
        command(keyboard, 28); time.sleep(.4)
        assert events().count('PlayPause') == before+2
        at(began, 5.7); command(keyboard, 1); move()
        absent('keyboard-does-not-replay-completion')

        # Playback updates must reach a retained completion card too. Neither
        # pause nor resume changes its text, so its foreground signature is stable.
        began = finish('playback-changes'); at(began, .6)
        media('Pause'); time.sleep(.3)
        paused = shot('completion-paused').crop((560, 16, 720, 24))
        assert all(max(pixel) < 12 for pixel in paused.getdata()), 'Paused artwork stayed active'
        media('Play'); time.sleep(.3)
        artwork('completion-resumed-artwork')
        assert 'download finished' in text('playback-update-retains-completion')
        at(began, 5.7); absent('playback-change-completion-expired')

        # Paused jobs keep their percentage and identity; progress changes alone
        # cannot imply a resume. Their unknown-size ring must be stationary too.
        msg('island-activity-start', json.dumps({'id': 'backup', 'title': 'Backing up', 'progress': 42}))
        activity('paused'); time.sleep(.6)
        assert 'paused' in text('paused-compact')
        move(735, 12)
        paused_text = text('paused-expanded')
        assert 'paused' in paused_text and '42' in paused_text, paused_text
        assert 'backing up' in text('paused-title', (518, 108, 700, 133), '7')
        artwork('paused-download-artwork')
        msg('island-activity-update', json.dumps({'id': 'backup', 'progress': 43}))
        time.sleep(.4)
        paused_text = text('progress-keeps-pause')
        assert 'paused' in paused_text and '43' in paused_text, paused_text
        activity('running'); time.sleep(.4)
        resumed = text('resumed-expanded')
        assert '43' in resumed and 'paused' not in resumed, resumed
        assert 'backing up' in text('resumed-title', (518, 108, 700, 133), '7')
        move(); activity('paused', progress=None); time.sleep(.6)
        amber = shot('paused-unknown').crop((511, 22, 549, 58))
        assert sum(r > 160 and 80 < g < r-35 and b < 80 for r, g, b in amber.getdata()) > 20
        # Compare the amber symbol/ring mask so moving background artwork is excluded.
        def amber_mask():
            image = shot('paused-ring-sample').crop((488, 8, 792, 74))
            return [r > 230 and 135 < g < 180 and b < 35 for r, g, b in image.getdata()]
        before = amber_mask(); time.sleep(.5)
        assert sum(a != b for a, b in zip(before, amber_mask())) < 8, 'Unknown-size paused transfer still spins'

        # Failure overrides even 100% progress. A duplicate error must not reset
        # its lifetime, and retry must reuse the existing activity.
        activity('running', progress=43)
        began = time.monotonic(); activity('failed', progress=100); at(began, .7)
        image = failure('transfer-failed')
        assert red(image, (565, 73, 715, 80), threshold=25) > 40, 'Red failure halo is missing'
        assert 'transfer failed' in text('failure-label')
        artwork('failure-artwork')
        at(began, 3); activity('failed')
        at(began, 4.2); failure('failure-before-expiry')
        assert time.monotonic() < began+5, 'Failure timing probe ran too slowly'
        at(began, 5.7)
        assert red(shot('failure-expired')) < 10, 'Duplicate error prolonged the notice'
        absent('failure-is-not-success'); media_ready('failure-return-media')
        activity('running', progress=44); time.sleep(.6); move(735, 12)
        retried = text('retry-existing-transfer')
        assert '44' in retried, retried
        assert 'backing up' in text('retry-title', (518, 108, 700, 133), '7')
        move()
        began = time.monotonic(); activity('failed', progress=100)
        msg('island-activity-end', 'backup'); time.sleep(.6)
        failure('ending-failed-retains-error'); absent('ending-failed-never-succeeds')
        at(began, 5.7)
        assert red(shot('ended-failure-expired')) < 10

        # Reduced motion keeps a steady halo. Rotation must pause behind the
        # notice, preserving its selected activity and remaining dwell time.
        path = cfg/'config.toml'
        path.write_text(original.replace('downloads-media-timers', 'media-downloads-timers')
                        .replace('media_gradient=true', 'media_gradient=false')
                        .replace('split_activities=false',
                                 'split_activities=false\ncycle_activities=true\nactivity_cycle_seconds=3')
                        +'\n[shell.animation]\nenabled=false\n')
        msg('config-reload'); time.sleep(.8)
        baseline = shot('rotation-media-before').crop((511, 22, 549, 58))
        progress('rotating', .37)
        began = finish('rotation'); at(began, .6)
        first = present('steady-green-before'); time.sleep(.6)
        second = present('steady-green-after')
        assert not ImageChops.difference(first.crop((560, 73, 720, 80)),
                                        second.crop((560, 73, 720, 80))).getbbox(), 'Reduced-motion halo pulsed'
        at(began, 5.6)
        returned = shot('rotation-media-return').crop((511, 22, 549, 58))
        assert not ImageChops.difference(baseline, returned).getbbox(), 'Activity rotated behind completion notice'
        progress('rotating', .37, False)

        # Match the script transfer wording and save a fractional-scale reference too.
        msg('theme-mode-set', 'light')
        msg('island-activity-start', 'backup', 'Copying files')
        msg('island-activity-update', 'backup', '100')
        began = time.monotonic(); msg('island-activity-end', 'backup'); at(began, .6)
        assert 'transfer finished' in text('light-transfer-finished')
        present('light-green-completion'); shot('fractional-completion', 'TEST-2')
        at(began, 5.7); absent('transfer-expired')
        assert shell.poll() is None
        print('PASS: five-second completion and halo, duplicates, overlapping finishes, remaining downloads, '
              'media return, artwork continuity and playback changes, OSD/notification/panel/keyboard '
              'interruptions, pause/resume, explicit failure and retry, paused rotation and reduced motion',
              flush=True)
    finally:
        for process in helpers:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=5)
