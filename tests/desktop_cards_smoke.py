#!/usr/bin/env python3
"""Render and exercise desktop cards in an isolated labwc session with fixture data."""
import datetime as dt
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import tomllib

from PIL import Image, ImageChops

REPO = Path(__file__).resolve().parents[1]
if '--worker' not in sys.argv:
    with tempfile.TemporaryDirectory(prefix='desktop-cards-bus-') as directory:
        bus = Path(directory) / 'bus.conf'
        bus.write_text('<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>'
                       '<auth>EXTERNAL</auth><policy context="default"><allow send_destination="*"/>'
                       '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>')
        raise SystemExit(subprocess.call(['dbus-run-session', '--config-file', str(bus), '--',
                                         sys.executable, __file__, '--worker', *sys.argv[1:]]))

media_suite = '--media' in sys.argv
coverage_suite = '--coverage' in sys.argv
out = REPO / ('build-rishot/desktop-coverage-smoke' if coverage_suite else 'build-rishot/desktop-media-cards-smoke' if media_suite else 'build-rishot/desktop-cards-smoke')
out.mkdir(parents=True, exist_ok=True)
binary = os.environ.get('NOCTALIA_TEST_BINARY', str(REPO / 'build-test/noctalia'))
with tempfile.TemporaryDirectory(prefix='desktop-cards-') as directory:
    base = Path(directory)
    runtime = base / 'runtime'
    runtime.mkdir(mode=0o700)
    config = base / 'config/noctalia'
    config.mkdir(parents=True)
    cache = base / 'cache/noctalia'
    cache.mkdir(parents=True)
    calendar = base / 'calendars/local'
    calendar.mkdir(parents=True)
    today = dt.datetime.now().astimezone()
    event_date = today.strftime('%Y%m%d')
    (calendar / 'review.ics').write_text(
        'BEGIN:VCALENDAR\nVERSION:2.0\nPRODID:-//Noctalia//Desktop Cards Test//EN\nBEGIN:VEVENT\n'
        f'UID:card-review\nDTSTART;VALUE=DATE:{event_date}\n'
        f'DTEND;VALUE=DATE:{(today + dt.timedelta(days=1)).strftime("%Y%m%d")}\n'
        'SUMMARY:Review desktop widgets\nEND:VEVENT\nEND:VCALENDAR\n')
    snapshot = dict(valid=True, location_name='London', source_label='Fixture', latitude=51.5,
                    longitude=-0.12, timezone='Europe/London', timezone_abbreviation='BST',
                    utc_offset_seconds=int(today.utcoffset().total_seconds()), fetched_at=int(time.time()),
                    current=dict(time_iso=today.isoformat(), temperature_c=18, weather_code=2, is_day=True),
                    forecast_days=[dict(date_iso=(today + dt.timedelta(days=i)).strftime('%Y-%m-%d'),
                                        temperature_min_c=10+i, temperature_max_c=19+i, weather_code=[2,3,61,0,2,3,0][i],
                                        sunrise_iso='', sunset_iso='') for i in range(7)])
    (cache / 'weather.json').write_text(json.dumps(dict(snapshot=snapshot)))
    header = f'''[shell]
setup_wizard_enabled=false
polkit_agent=false
[bar.default]
enabled=false
[dock]
enabled=false
[island]
enabled=false
[theme]
mode="dark"
[location]
latitude=51.5
longitude=-0.12
[weather]
enabled=true
[calendar]
enabled=true
[calendar.account.fixture]
type="vdir"
name="Fixture"
path={json.dumps(str(calendar.parent))}
[calendar.reminders]
enabled=false
[osd.kinds]
lock_keys=false
[desktop_widgets]
enabled=true
'''
    blocks = []
    for kind, offset in ([('clock', 0), ('media_player', 480)] if media_suite else [('weather', 0), ('calendar', 480)]):
        for size, x, y in [('small', 128, 144), ('medium', 464, 144), ('large', 928, 256)]:
            blocks.append(f'''[desktop_widgets.widget.{kind}_{size}]
type="{kind}"
output="HEADLESS-1"
cx={x}.0
cy={y+offset}.0
placement_width=1280.0
placement_height=1024.0
[desktop_widgets.widget.{kind}_{size}.settings]
card_size="{size}"
background_padding=14
background_radius=16
''')
    if media_suite:
        blocks = [block + ('clock_style="analog"\nshow_seconds=false\ncolor="primary"\n' if 'type="clock"' in block else '') for block in blocks]
    config_file = config / 'config.toml'
    config_file.write_text(header + ''.join(blocks))
    compositor_config = base / 'labwc'
    compositor_config.mkdir()
    (compositor_config / 'rc.xml').write_text('<labwc_config/>')
    (compositor_config / 'autostart').write_text('')
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(base/'config'),
               XDG_STATE_HOME=str(base/'state'), XDG_DATA_HOME=str(base/'data'), XDG_CACHE_HOME=str(base/'cache'),
               NOCTALIA_CONFIG_HOME=str(base/'config'), NOCTALIA_STATE_HOME=str(base/'state'),
               NOCTALIA_DATA_HOME=str(base/'data'), NOCTALIA_ASSETS_DIR=str(REPO/'assets'),
               WLR_BACKENDS='headless', WLR_HEADLESS_OUTPUTS='1', WLR_LIBINPUT_NO_DEVICES='1',
               LIBGL_ALWAYS_SOFTWARE='1', XDG_CURRENT_DESKTOP='labwc', XDG_SESSION_TYPE='wayland',
               GSETTINGS_BACKEND='keyfile', https_proxy='http://127.0.0.1:9', http_proxy='http://127.0.0.1:9')
    env['DBUS_SYSTEM_BUS_ADDRESS'] = env['DBUS_SESSION_BUS_ADDRESS']
    for key in ('WAYLAND_DISPLAY', 'DISPLAY', 'HYPRLAND_INSTANCE_SIGNATURE', 'SWAYSOCK', 'TRIAD_SOCKET', 'MANGO_INSTANCE_SIGNATURE'):
        env.pop(key, None)
    processes = []

    def run(args):
        return subprocess.check_output(args, env=env, text=True, stderr=subprocess.STDOUT, timeout=20).strip()

    def start(args, name):
        with (out/name).open('w') as log:
            process = subprocess.Popen(args, env=env, stdout=log, stderr=log)
        processes.append(process)
        return process

    def wait(check, reason):
        for _ in range(150):
            if check():
                return
            time.sleep(.1)
        raise AssertionError(reason)

    try:
        start(['labwc', '-C', str(compositor_config)], 'labwc.log')
        wait(lambda: list(runtime.glob('wayland-*.lock')), 'labwc did not start')
        env['WAYLAND_DISPLAY'] = next(runtime.glob('wayland-*.lock')).name.removesuffix('.lock')
        run(['wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '1280x1024@60'])
        protocol = REPO / 'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
        run(['wayland-scanner', 'client-header', str(protocol), str(base/'pointer-client.h')])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/'pointer-code.c')])
        (base/'pointer.c').write_text((REPO/'tests/fixtures/island_pointer.c').read_text().replace('x,y,1280,720', 'x,y,1280,1024'))
        run(['cc', '-I'+str(base), str(base/'pointer.c'), str(base/'pointer-code.c'), '-lwayland-client', '-o', str(base/'pointer')])
        pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        processes.append(pointer)
        protocol = REPO / 'protocols/virtual-keyboard-unstable-v1.xml'
        run(['wayland-scanner', 'client-header', str(protocol), str(base/'keyboard-client.h')])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/'keyboard-code.c')])
        run(['cc', '-I'+str(base), str(REPO/'tests/fixtures/island_keyboard.c'), str(base/'keyboard-code.c'),
             '-lwayland-client', '-lxkbcommon', '-o', str(base/'keyboard')])
        keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        processes.append(keyboard)
        def key(command):
            keyboard.stdin.write(str(command)+'\n')
            keyboard.stdin.flush()
            assert keyboard.stdout.readline().strip() == 'ok'
            time.sleep(.1)
        pointer_width, pointer_height = 1280, 1024
        def pointer_command(command):
            if command.startswith('move '):
                _, x, y = command.split()
                command = f'move {round(int(x)*1280/pointer_width)} {round(int(y)*1024/pointer_height)}'
            pointer.stdin.write(command+'\n')
            pointer.stdin.flush()
            assert pointer.stdout.readline().strip() == 'ok'
            time.sleep(.1)
        def click(x, y):
            pointer_command(f'move {x} {y}')
            pointer_command('press')
            pointer_command('release')
        def park_pointer():
            pointer_command('move 1220 990')
        park_pointer()
        if coverage_suite:
            from desktop_cards_coverage import Coverage
            coverage = Coverage(base, out, env, config_file, header, processes)
            coverage.prepare()
        run([binary, 'config', 'validate'])
        shell = start([binary], 'noctalia.log')
        wait(lambda: (runtime / f"noctalia-{env['WAYLAND_DISPLAY']}.sock").exists(), 'shell did not start')

        def msg(*args):
            return run([binary, 'msg', *args])

        def shot(name):
            time.sleep(.5)
            assert shell.poll() is None, 'shell exited'
            path = out / (name + '.png')
            run(['grim', str(path)])
            return Image.open(path).convert('RGB')

        if coverage_suite:
            coverage.exercise(binary, run, msg, shot, click, park_pointer, key)
        elif media_suite:
            import ocr
            time.sleep(3)
            env['ISLAND_TEST_ART'] = (REPO/'assets/noctalia-wallpaper.png').as_uri()
            env['ISLAND_TEST_EVENTS'] = str(out/'player-actions.log')
            (out/'player-actions.log').write_text('')
            env['ISLAND_TEST_TICK'] = '1'
            player = start([sys.executable, str(REPO/'tests/fixtures/island_player.py')], 'player.log')
            time.sleep(2)
            for mode in ('dark', 'light'):
                msg('color-scheme-set', 'community', 'macOS')
                msg('theme-mode-set', mode)
                park_pointer()
                shot(mode)
            # These controls operate the same MPRIS player as the shell's media controls.
            click(128, 694)
            assert 'PlayPause' in (out/'player-actions.log').read_text(), 'small card play/pause did not activate'
            click(514, 694)
            assert 'Next' in (out/'player-actions.log').read_text(), 'medium card next did not activate'
            # Track changes refresh the cached MPRIS position asynchronously.
            time.sleep(2)
            before = shot('paused')
            time.sleep(1.2)
            after = shot('paused-idle')
            assert ImageChops.difference(before.crop((712,520,1144,952)), after.crop((712,520,1144,952))).getbbox() is None, 'paused card did not settle'
            click(928, 868)
            assert (out/'player-actions.log').read_text().count('PlayPause') >= 2, 'large card play did not activate'
            shot('playing')
            # A real focused app drives blending; it occupies the center and leaves the small card visible.
            focus = start(['alacritty', '--class', 'desktop-blend-test', '--title', 'Blend Focus Test', '-e', 'sleep', '120'], 'focus-window.log')
            time.sleep(2)
            park_pointer()
            muted = shot('blended')
            sample = (40,570,108,630)
            def chroma(image):
                pixels = list(image.crop(sample).getdata())
                return sum(max(p)-min(p) for p in pixels)/len(pixels)
            assert chroma(muted) < chroma(before)*.65, 'focused app did not soften artwork colours'
            pointer_command('move 120 600')
            hovered = shot('hover-full-colour')
            assert chroma(hovered) > chroma(muted)*1.5, 'hover did not restore full colour'
            park_pointer()
            config_file.write_text((header + ''.join(blocks)).replace('[desktop_widgets]\nenabled=true', '[desktop_widgets]\nenabled=true\nalways_full_color=true'))
            time.sleep(1)
            full = shot('always-full-colour')
            assert chroma(full) > chroma(muted)*1.5, 'always full colour preference did not apply'
            config_file.write_text(header + ''.join(blocks))
            focus.terminate(); focus.wait(timeout=5)
            time.sleep(1)
            desktop = shot('desktop-full-colour')
            assert chroma(desktop) > chroma(muted)*1.5, 'closing the focused app did not restore colour'
            player.terminate(); player.wait(timeout=5)
            time.sleep(1)
            shot('no-player')
            assert ocr.find(out/'no-player.png', 'Nothing Playing', min_y=520), 'empty media state is missing'
            # Digital layouts and timezone changes reuse the same card sizes.
            config_file.write_text((header + ''.join(blocks)).replace('clock_style="analog"', 'clock_style="digital"\ntimezone="Asia/Tokyo"'))
            time.sleep(1)
            exported = subprocess.check_output([binary, 'config', 'export', 'full'], env=env, text=True, stderr=subprocess.DEVNULL)
            (out/'digital-config.toml').write_text(exported)
            assert tomllib.loads(exported)['desktop_widgets']['widget']['clock_small']['settings']['clock_style'] == 'digital', 'digital clock configuration was overridden'
            shot('digital-clocks')
            assert ocr.find(out/'digital-clocks.png', 'Tokyo', max_y=500), 'clock timezone was not applied'
            assert any(re.fullmatch(r'\d{1,2}:\d{2}', row['text']) and row['left'] < 230 and 85 < row['top'] < 200
                       for row in ocr.words(out/'digital-clocks.png')), 'small digital clock did not display its time'
            msg('desktop-widgets-edit')
            shot('editor')
            click(444, 92)
            shot('gallery')
            point = ocr.find(out/'gallery.png', 'Clock', min_y=250, max_y=650)
            assert point, 'Clock missing from gallery'
            click(*point)
            shot('gallery-clock')
            point = ocr.find(out/'gallery-clock.png', 'Large', min_x=450, min_y=280, max_y=330)
            assert point, 'Clock card sizes missing'
            click(*point)
            shot('gallery-clock-large')
            point = ocr.find(out/'gallery-clock-large.png', 'Now Playing', min_y=250, max_y=650)
            if not point:
                pointer_command('move 340 550')
                pointer_command('scroll 4')
                shot('gallery-clock-scrolled')
                point = ocr.find(out/'gallery-clock-scrolled.png', 'Now Playing', min_y=250, max_y=740)
            assert point, 'Now Playing missing from gallery'
            click(*point)
            shot('gallery-now-playing')
            msg('desktop-widgets-exit')
            print('PASS: clock/media card sizes, playback controls, idle state, focus blending, hover, colour preference, timezone and gallery', flush=True)
            print(out, flush=True)
        else:
            time.sleep(3)
            for mode in ('dark', 'light'):
                msg('color-scheme-set', 'community', 'macOS')
                msg('theme-mode-set', mode)
                time.sleep(1)
                before = shot(mode)
                after = shot(mode + '-idle')
                # Stable widget content must not change while the data remains unchanged.
                assert ImageChops.difference(before.crop((24,40,1144,952)), after.crop((24,40,1144,952))).getbbox() is None, 'idle cards changed'
            before = shot('calendar-before-navigation')
            click(652, 548)
            park_pointer()
            after = shot('calendar-next-month')
            assert ImageChops.difference(before.crop((248,520,680,728)), after.crop((248,520,680,728))).getbbox(), 'calendar next-month button did not work'
            click(464, 548)
            park_pointer()
            restored = shot('calendar-today')
            assert ImageChops.difference(before.crop((248,520,680,728)), restored.crop((248,520,680,728))).getbbox() is None, 'calendar month heading did not restore today'
            import ocr
            def click_text(name, label, **constraints):
                point = ocr.find(out/(name+'.png'), label, **constraints)
                assert point, f'{label} missing from {name}'
                click(*point)
    
            msg('desktop-widgets-edit')
            shot('editor')
            click(444, 92)
            for mode in ('dark', 'light'):
                msg('theme-mode-set', mode)
                park_pointer()
                shot('gallery-'+mode)
            click_text('gallery-light', 'Medium', min_x=450, min_y=280, max_y=330)
            medium = shot('gallery-weather-medium')
            click_text('gallery-weather-medium', 'Large', min_x=450, min_y=280, max_y=330)
            large = shot('gallery-weather-large')
            assert ImageChops.difference(medium.crop((452,340,1024,740)), large.crop((452,340,1024,740))).getbbox(), 'size picker did not change the preview'
            key(15)  # Focus a gallery control before dismissing it.
            key(1)   # Escape closes only the gallery.
            shot('gallery-dismissed')
            click(444, 92)
            shot('gallery-reopened')
            click_text('gallery-reopened', 'Calendar', min_y=250, max_y=660)
            shot('gallery-calendar')
            click(508, 800)
            shot('gallery-added')
            msg('desktop-widgets-exit')
            state_file = base/'state/noctalia/settings.toml'
            state = tomllib.loads(state_file.read_text())
            saved_widgets = state['desktop_widgets']['widget']
            additions = [v for k, v in saved_widgets.items() if not k.startswith(('weather_', 'calendar_'))]
            assert len(additions) == 1 and additions[0]['type'] == 'calendar', 'gallery did not add the chosen type'
            added = additions[0]
            assert added['settings']['card_size'] == 'medium', 'gallery did not save the chosen size'
            assert added['output'] == 'HEADLESS-1', 'gallery used the wrong output'
            assert added['cy'] >= 368 and added['cy'] <= 416, 'gallery did not use free space between existing cards'
            msg('desktop-widgets-edit')
            shot('editor-before-snap')  # Wait for the new overlay to receive input.
            click(128, 194)
            time.sleep(.4)
            pointer_command('move 128 194')
            pointer_command('press')
            pointer_command('move 138 222')
            pointer_command('release')
            msg('desktop-widgets-exit')
            state = tomllib.loads(state_file.read_text())
            moved = state['desktop_widgets']['widget']['weather_small']
            assert abs(moved['cy']-144) > 4, 'drag did not move the widget'
            msg('desktop-widgets-edit')
            shot('editor-before-align')
            click(int(moved['cx']), int(moved['cy']))
            time.sleep(.4)
            pointer_command(f"move {int(moved['cx'])} {int(moved['cy'])}")
            pointer_command('press')
            pointer_command('move 134 146')
            shot('snap-guides')
            pointer_command('release')
            msg('desktop-widgets-exit')
            state = tomllib.loads(state_file.read_text())
            small = state['desktop_widgets']['widget']['weather_small']
            assert abs(small['cx']-128) < .1 and abs(small['cy']-144) < .1, f'card did not snap to its neighbour: {small}'
            msg('desktop-widgets-edit')
            shot('editor-after-gallery')
            click(128, 194)
            click(732, 92)
            park_pointer()
            shot('inspector')
            click_text('inspector', 'Small', min_x=500, max_y=240)
            park_pointer()
            shot('size-options')
            import ocr
            point = ocr.find(out/'size-options.png', 'Medium')
            assert point, 'card size dropdown is missing Medium'
            click(*point)
            park_pointer()
            shot('preset-changed')
            msg('desktop-widgets-exit')
            state_file = base/'state/noctalia/settings.toml'
            state = tomllib.loads(state_file.read_text())
            assert state['desktop_widgets']['widget']['weather_small']['settings']['card_size'] == 'medium', 'card layout was not saved'
            assert state['desktop_widgets']['widget']['weather_small']['box_width'] == 0, 'choosing a preset did not restore its footprint'
            assert state['desktop_widgets']['widget']['weather_small']['cx'] >= 216, 'wider preset was clipped off the output'
            assert state['desktop_widgets']['widget']['calendar_large']['settings']['card_size'] == 'large', 'large calendar layout was not saved' 
            msg('desktop-widgets-hide')
            hidden = shot('hidden')
            msg('desktop-widgets-show')
            shown = shot('shown')
            assert ImageChops.difference(hidden, shown).getbbox(), 'widgets did not return'
            # A missing location is an explicit empty state and must hide stale forecasts.
            config_file.write_text((header + ''.join(blocks)).replace('enabled=true\n[calendar]', 'enabled=false\n[calendar]'))
            time.sleep(1)
            shot('weather-off')
            msg('desktop-widgets-edit')
            shot('editor-before-compact')
            click(444, 92)
            shot('gallery-before-compact')
            run(['wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '560x800@60'])
            pointer_width, pointer_height = 560, 800
            time.sleep(1)
            pointer_command('move 550 780')
            compact = shot('gallery-compact')
            assert compact.size == (560, 800), 'compact output was not configured'
            click_text('gallery-compact', 'Large', min_y=180, max_y=215)
            shot('gallery-compact-large')
            click(280, 155)
            shot('gallery-compact-types')
            click_text('gallery-compact-types', 'Clock')
            shot('gallery-compact-clock')
            click(514, 110)
            shot('gallery-compact-dismissed')
            msg('desktop-widgets-exit')
            print('PASS: card layouts, dark/light gallery, preview sizes, Escape, gallery placement and persistence, spacing guides, calendar navigation, inspector, visibility and compact gallery', flush=True)
            print(out, flush=True)
    finally:
        if coverage_suite and 'coverage' in locals():
            coverage.close()
        for process in reversed(processes):
            process.terminate()
        for process in reversed(processes):
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
