"""Exercise widget detail popups in the private headless compositor."""
import datetime as dt
import json
import time
import tomllib

from PIL import Image
import ocr
from desktop_stacks_smoke import Stacks


class Details(Stacks):
    def prepare(self):
        blocks = []
        for widget, kind, x, y, size in [
            ('weather', 'weather', 128, 144, 'small'),
            ('calendar', 'calendar', 128, 490, 'small'),
            ('month', 'calendar', 450, 740, 'medium'),
            ('member_weather', 'weather', 0, 0, 'small'),
            ('member_calendar', 'calendar', 0, 0, 'small'),
            ('stack', 'stack', 1110, 750, 'small'),
        ]:
            blocks.append(f'[desktop_widgets.widget.{widget}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{widget}.settings]\ncard_size="{size}"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if kind == 'stack':
                blocks.append('members=["member_weather", "member_calendar"]\nauto_rotate=true\nrotation_seconds=5\n')
        self.config.write_text(self.header+''.join(blocks))
        fixture = self.base/'calendars/local/review.ics'
        fixture.write_text(fixture.read_text().replace('SUMMARY:Review desktop widgets',
                           'LOCATION:Studio One\nSUMMARY:Review desktop widgets'))
        cache = self.base/'cache/noctalia/weather.json'
        payload = json.loads(cache.read_text())
        snapshot = payload['snapshot']
        snapshot['current'].update(wind_speed_kmh=12, relative_humidity_percent=65)
        today = dt.datetime.now().astimezone()
        for day in snapshot['forecast_days']:
            day.update(sunrise_iso=day['date_iso']+'T07:15', sunset_iso=day['date_iso']+'T18:30')
        snapshot['forecast_hours'] = [dict(time_iso=(today+dt.timedelta(hours=i)).strftime('%Y-%m-%dT%H:00'),
            temperature_c=18+i, weather_code=2, relative_humidity_percent=65,
            precipitation_probability_percent=10, wind_speed_kmh=12, is_day=True) for i in range(24)]
        cache.write_text(json.dumps(payload))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def has(name, label, **kwargs):
            point = ocr.find(self.out/(name+'.png'), label, **kwargs)
            assert point, f'{label} missing from {name}'
            return point

        def absent(name, label):
            assert not ocr.find(self.out/(name+'.png'), label), f'{label} still visible in {name}'

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        before = tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']
        click(128, 144)
        park()
        shot('weather-light')
        has('weather-light', 'Weather Details')
        has('weather-light', 'Humidity')
        has('weather-light', 'Sunrise')
        has('weather-light', 'Daily')
        click(*has('weather-light', 'Hourly'))
        park()
        shot('weather-hourly')
        has('weather-hourly', 'Rain')
        key(1)
        shot('weather-escape')
        absent('weather-escape', 'Weather Details')
        click(128, 144)
        click(1220, 990)
        shot('weather-outside')
        absent('weather-outside', 'Weather Details')
        click(128, 490)
        park()
        shot('calendar-agenda')
        has('calendar-agenda', 'Day Agenda')
        has('calendar-agenda', 'Review desktop widgets')
        # Crop the agenda so sparse OCR does not discard its small metadata.
        Image.open(self.out/'calendar-agenda.png').crop((250, 380, 670, 480)).save(self.out/'agenda-metadata.png')
        has('agenda-metadata', 'Studio One')
        has('agenda-metadata', 'Fixture')
        event = self.base/'calendars/local/review.ics'
        event.write_text(event.read_text().replace('Review desktop widgets', 'Updated desktop widgets'))
        for _ in range(10):
            shot('calendar-updated')
            if ocr.find(self.out/'calendar-updated.png', 'Updated desktop widgets'):
                break
        else:
            raise AssertionError('open agenda did not refresh after its calendar changed')
        # Keyboard traversal: close, previous day, next day. Navigate then Escape.
        key(15)
        key(15)
        key(28)
        shot('calendar-other-day')
        has('calendar-other-day', 'No events')
        key(1)
        # Select a particular day in the month grid, away from its month controls.
        shot('month')
        point = has('month', '15', min_x=245, min_y=680, max_y=835)
        click(*point)
        park()
        shot('calendar-selected')
        has('calendar-selected', 'Day Agenda')
        key(1)
        # Right-click remains the context menu; it must not open the detail panel.
        pointer('move 128 144')
        pointer('right-press')
        pointer('right-release')
        park()
        shot('weather-menu')
        has('weather-menu', 'Configure')
        absent('weather-menu', 'Weather Details')
        key(1)
        click(1089, 845)
        click(1110, 750)
        park()
        shot('stack-weather')
        has('stack-weather', 'Weather Details')
        key(1)
        pointer('move 1110 750')
        pointer('scroll 1')
        time.sleep(.5)
        click(1110, 750)
        park()
        shot('stack-calendar')
        has('stack-calendar', 'Day Agenda')
        # Editor entry closes the popup before destroying its parent surface.
        msg('desktop-widgets-edit')
        shot('editor')
        absent('editor', 'Day Agenda')
        msg('desktop-widgets-exit')
        msg('theme-mode-set', 'dark')
        click(128, 144)
        park()
        shot('weather-dark')
        has('weather-dark', 'Weather Details')
        key(1)
        after = tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']
        assert before == after, 'viewing details changed the widget layout'
        # Repeated opening catches stale dismiss callbacks and teardown errors.
        for _ in range(4):
            click(128, 144)
            key(1)
            click(128, 490)
            key(1)
        click(1089, 845)  # Explicitly select the weather page.
        click(1110, 750)
        park()
        time.sleep(6)
        shot('stack-rotation-paused')
        has('stack-rotation-paused', 'Weather Details')
        def page():
            for file in (self.base/'state').rglob('*.toml'):
                raw = tomllib.loads(file.read_text()).get('desktop_stacks', {}).get('pages')
                if raw:
                    return json.loads(raw).get('stack')
        assert page() == 'member_weather', 'stack rotated beneath an open detail popup'
        key(1)
        park()
        time.sleep(7)
        assert page() == 'member_calendar', f'stack did not resume rotation after dismissal: {page()}'
        click(128, 144)
        run(['wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '560x720@60'])
        shot('output-change')
        absent('output-change', 'Weather Details')
        time.sleep(1)
        current = tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']['widget']['weather']
        # Fixture pointer coordinates still use the original output dimensions.
        click(round(current['cx']*1280/560), round(current['cy']*1024/720))
        shot('compact-weather')
        has('compact-weather', 'Weather Details')
        has('compact-weather', 'Humidity')
        pointer('move 640 600')
        pointer('scroll 15')
        shot('compact-forecast')
        has('compact-forecast', 'Daily')
        has('compact-forecast', 'Hourly')
        key(1)
        print('PASS: weather forecasts, selected-day agenda, dismissal, stacks and compact output', flush=True)
