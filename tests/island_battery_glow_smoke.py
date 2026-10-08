"""Battery connection halos on the private GPU compositor and D-Bus fixtures."""
import json
import pathlib
import sys
import time

from PIL import Image, ImageChops


def prepare(base, cfg, env):
    path = cfg/'config.toml'
    text = path.read_text().replace('hover_widgets=["workspaces","taskbar"]', '''hover_widgets=[]
media_gradient=true
track_preview_seconds=0
bluetooth_preview_seconds=5
bluetooth_preview_monitor="all"
split_activities=false''')
    path.write_text(text.replace('[shell]\n', '[shell]\noffline_mode=true\n')
                    +'\n[battery]\nwarning_threshold=0\n')


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell, bluetooth, battery):
    repo = pathlib.Path(__file__).resolve().parents[1]
    original = (cfg/'config.toml').read_text()
    halo_box = (590, 73, 690, 80)

    def properties(proc, **values):
        proc.stdin.write(json.dumps(values)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'

    def at(began, seconds):
        time.sleep(max(0, began+seconds-time.monotonic()))

    def shot(name, output='TEST-1'):
        path = out/(name+'.png')
        run(['grim', '-o', output, str(path)])
        return Image.open(path).convert('RGB')

    def count(image, color, box=halo_box):
        tests = {
            'green': lambda r, g, b: g > 25 and g > r+15 and g > b,
            # At reduced-motion strength, amber blends with the blue wallpaper.
            'amber': lambda r, g, b: r > 35 and r > g+15 and g > b+5,
            'red': lambda r, g, b: r > 35 and r > g*1.8 and r > b+15,
        }
        return sum(tests[color](*pixel) for pixel in image.crop(box).getdata())

    def present(name, color, box=halo_box):
        image = shot(name)
        assert count(image, color, box) > 40, (name, color, 'Missing battery halo')
        return image

    def absent(name):
        image = shot(name)
        assert max(count(image, color) for color in ('green', 'amber', 'red')) < 10, (name, 'Unexpected halo')

    def connect(percentage):
        properties(bluetooth, Connected=False)
        time.sleep(.15)
        began = time.monotonic()
        properties(bluetooth, Connected=True, Percentage=percentage)
        return began

    def configure(reduced=False, outer=True):
        (cfg/'config.toml').write_text(original.replace('split_activities=false',
                                     'split_activities=false\nouter_progress_ring='+str(outer).lower())
                                     +'\n[shell.animation]\nenabled='+str(not reduced).lower()+'\n')
        msg('config-reload'); time.sleep(.6)

    ctl('dismissnotify')
    dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    msg('color-scheme-set', 'community', 'macOS'); msg('theme-mode-set', 'dark')
    env['ISLAND_TEST_ART'] = (repo/'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_TITLE'] = 'Battery glow fixture'
    env['ISLAND_TEST_EVENTS'] = str(out/'player-events.txt')
    start([sys.executable, str(repo/'tests/fixtures/island_player.py')], 'player.log')
    time.sleep(2)
    absent('before-connection')

    began = connect(75)
    samples = []
    for index, offset in enumerate((.6, 1.2, 1.8, 2.4)):
        at(began, offset)
        samples.append(shot('green-pulse-'+str(index)))
    shot('green-pulse-other-output', 'TEST-2')
    assert max(count(image, 'green') for image in samples) > 40
    assert ImageChops.difference(samples[0].crop(halo_box), samples[1].crop(halo_box)).getbbox(), 'Halo did not pulse'
    # Exclude labels, icons and halo so they cannot mask missing artwork animation.
    interior = (590, 16, 690, 24)
    assert sum(max(pixel) > 12 for pixel in samples[1].crop(interior).getdata()) > 400
    assert ImageChops.difference(samples[0].crop(interior), samples[1].crop(interior)).getbbox(), 'Artwork froze'
    at(began, 6)
    present('glow-outlives-compact-preview', 'green')
    properties(bluetooth, Percentage=76)
    at(began, 8.9)
    # Probe several pulse phases before expiry; a valley alone cannot prove absence.
    late = [shot('late-pulse-0')]
    time.sleep(.25); late.append(shot('late-pulse-1'))
    assert max(count(image, 'green') for image in late) > 40
    at(began, 10.6); absent('expired-after-ten-seconds')
    properties(bluetooth, Percentage=77); time.sleep(.5)
    absent('percentage-does-not-restart')

    configure(reduced=True)
    began = connect(60); at(began, .5)
    first = present('sixty-is-amber', 'amber'); time.sleep(.5)
    second = present('reduced-motion-steady', 'amber')
    assert not ImageChops.difference(first.crop(halo_box), second.crop(halo_box)).getbbox(), 'Reduced-motion halo pulsed'
    for percentage, color in ((20, 'amber'), (19, 'red'), (0, 'red'), (61, 'green')):
        properties(bluetooth, Percentage=percentage); time.sleep(.35)
        present('boundary-'+str(percentage), color)
    properties(bluetooth, Connected=False); time.sleep(.4)
    absent('disconnect-clears-halo')

    # UPower peripheral arrival is a connection even without BlueZ.
    properties(battery, add='mouse_USB', properties={'Type': 5, 'IsPresent': True, 'Percentage': 40., 'State': 2})
    time.sleep(.5); present('wired-device-amber', 'amber')
    properties(battery, remove='mouse_USB'); time.sleep(.4)
    absent('wired-device-removed')
    properties(battery, IsPresent=True, State=2, Percentage=80., Serial='')
    time.sleep(.4); absent('system-discharging')
    properties(battery, State=1); time.sleep(.4)
    present('charger-connected', 'green')
    properties(battery, State=4); time.sleep(.4)
    present('fully-charged-retains-event', 'green')
    properties(battery, State=2); time.sleep(.4)
    absent('charger-unplugged')

    # A notification hides the halo without extending the original connection deadline.
    began = connect(40); at(began, .4)
    run(['gdbus', 'call', '--session', '--dest', 'org.freedesktop.Notifications', '--object-path',
         '/org/freedesktop/Notifications', '--method', 'org.freedesktop.Notifications.Notify',
         'Mail', '0', '', 'Priority notice', 'Battery connection expires behind this alert.',
         '[]', "{'urgency': <byte 2>}", '0'])
    time.sleep(.5)
    # The notification grows downwards; its top edge remains at the same position.
    present('critical-alert-keeps-red', 'red', (590, 1, 690, 7))
    at(began, 10.6)
    msg('notification-clear-active'); msg('notification-clear-history'); time.sleep(.5)
    absent('interrupted-connection-does-not-replay')

    # A completed transfer keeps its green halo while the battery pulse is amber.
    began = connect(40); at(began, .4)
    msg('island-activity-start', 'battery-test', 'Copying files')
    msg('island-activity-update', 'battery-test', '100')
    msg('island-activity-end', 'battery-test'); time.sleep(.5)
    present('completion-takes-priority', 'green')
    at(began, 6); present('battery-remaining-lifetime', 'amber')
    at(began, 10.6); absent('battery-expires-after-completion')

    # All-output routing also renders at fractional scale, with the same event age.
    configure(reduced=True)
    began = connect(75); at(began, .4)
    present('all-output-green', 'green')
    fractional = shot('fractional-green', 'TEST-2')
    fractional.resize((1280, 720)).save(out/'fractional-green-logical.png')
    assert count(fractional.resize((1280, 720)), 'green') > 40
    configure(reduced=True, outer=False)
    absent('outer-ring-disabled')
    assert shell.poll() is None and not ctl('configerrors').strip()
    print('PASS: pulsing and ten-second expiry, threshold boundaries, percentage updates, media artwork, '
          'reduced motion, disconnect, wired devices, charger transitions, notification and transfer priority, '
          'fractional scale and disabled outer ring', flush=True)
