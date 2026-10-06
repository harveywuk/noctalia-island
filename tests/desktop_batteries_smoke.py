"""Exercise battery rings, live device details and selection on a private bus."""
import json
from pathlib import Path
import subprocess
import time
import tomllib

from PIL import Image
import ocr
from desktop_stacks_smoke import Stacks


class Batteries(Stacks):
    def update(self, **properties):
        self.battery.stdin.write(json.dumps(properties)+'\n')
        self.battery.stdin.flush()
        assert self.battery.stdout.readline().strip() == 'ok'
        time.sleep(.2)

    def prepare(self):
        self.battery = subprocess.Popen(['python3', str(Path(__file__).parent/'fixtures/island_battery.py')],
            env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.processes.append(self.battery)
        assert self.battery.stdout.readline().strip() == 'ok'
        self.update(IsPresent=True, Model='Test laptop', Percentage=76, EnergyFull=45)
        for id, name, kind, percent, state in [
            ('headphones', 'Studio headphones', 19, 45, 1),
            ('keyboard', 'Desk keyboard', 6, 100, 4),
            ('mouse', 'Wireless mouse', 5, 14, 2),
        ]:
            self.update(add=id, properties=dict(IsPresent=True, Model=name, Type=kind, Percentage=percent,
                State=state, TimeToFull=1800 if state == 1 else 0))
        blocks = []
        for id, kind, x, y, size in [
            ('small', 'batteries', 140, 160, 'small'),
            ('medium', 'batteries', 570, 160, 'medium'),
            ('large', 'batteries', 260, 510, 'large'),
            ('single', 'batteries', 740, 440, 'small'),
            ('stack_battery', 'batteries', 0, 0, 'small'),
            ('stack_clock', 'clock', 0, 0, 'small'),
            ('stack', 'stack', 1050, 720, 'small'),
        ]:
            blocks.append(f'[desktop_widgets.widget.{id}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{id}.settings]\ncard_size="{size}"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if id == 'single':
                blocks.append('hidden_devices=["upower:headphones", "upower:keyboard", "upower:mouse"]\n')
            if kind == 'stack':
                blocks.append('members=["stack_battery", "stack_clock"]\n')
        self.config.write_text(self.header+''.join(blocks))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def has(name, label, **kwargs):
            point = ocr.find(self.out/(name+'.png'), label, **kwargs)
            assert point, f'{label} missing from {name}'
            return point

        def state():
            return tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']['widget']

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        for mode in ('dark', 'light'):
            msg('theme-mode-set', mode)
            park()
            shot('cards-'+mode)
        for text in ('76%', '45%', '100%', '14%', 'Test laptop', 'Desk keyboard'):
            has('cards-light', text)
        before = state()
        click(260, 510)
        park()
        shot('details')
        has('details', 'Battery Details')
        Image.open(self.out/'details.png').crop((485, 295, 925, 725)).save(self.out/'details-only.png')
        has('details-only', 'Battery health: 90%')
        has('details-only', 'remaining')
        has('details-only', 'until full')
        self.update(Percentage=82)
        shot('details-updated')
        has('details-updated', '82%', min_x=490)
        key(1)
        shot('escape')
        assert not ocr.find(self.out/'escape.png', 'Battery Details')
        assert state() == before, 'viewing details changed settings'
        click(570, 160)
        click(1220, 990)
        shot('outside')
        assert not ocr.find(self.out/'outside.png', 'Battery Details')
        # A stack forwards battery interactions to the same anchored detail panel.
        click(1050, 720)
        park()
        shot('stack-details')
        has('stack-details', 'Battery Details')
        key(1)
        # Zero and full are real endpoints. Charging suppresses the low-battery color.
        self.update(Percentage=0)
        shot('empty-charge')
        has('empty-charge', '0%')
        self.update(Percentage=100, State=4)
        shot('full-charge')
        has('full-charge', '100%')
        self.update(Percentage=12, State=1)
        shot('charging')
        has('charging', '12%')
        # A hotplug changes all cards and an already-open details panel.
        click(260, 510)
        self.update(add='gamepad', properties=dict(IsPresent=True, Model='Game controller', Type=12, Percentage=88, State=0))
        park()
        shot('device-added')
        has('device-added', 'Game controller', min_x=490)
        self.update(remove='gamepad')
        shot('device-removed')
        assert not ocr.find(self.out/'device-removed.png', 'Game controller')
        key(1)
        # Configure through the native context menu; device choices must persist and undo.
        pointer('move 570 160')
        pointer('right-press')
        pointer('right-release')
        park()
        shot('menu')
        hover = has('menu', 'Small')
        pointer(f'move {hover[0]} {hover[1]}')
        shot('menu')
        click(*has('menu', 'Configure', exact=True))
        park()
        shot('inspector')
        has('inspector', 'Devices on card')
        label = has('inspector', 'Test laptop', min_x=230, min_y=235, max_y=275)
        # Device switches sit in the right control column of the 520px inspector.
        click(707, label[1])
        park()
        shot('hidden')
        has('hidden', 'Show all devices')
        msg('desktop-widgets-exit')
        shot('selected-cards')
        assert 'upower:BAT0' in state()['medium']['settings']['hidden_devices'], 'device switch did not persist'
        # Undo from the desktop menu preserves the rest of the card configuration.
        pointer('move 570 160')
        pointer('right-press')
        pointer('right-release')
        park()
        shot('undo-menu')
        hover = has('undo-menu', 'Small')
        pointer(f'move {hover[0]} {hover[1]}')
        shot('undo-menu')
        click(*has('undo-menu', 'Undo', exact=True))
        assert not state()['medium']['settings'].get('hidden_devices'), 'device selection did not undo'
        # Empty services show an honest empty state, then recover on reconnect.
        self.update(IsPresent=False)
        for id in ('headphones', 'keyboard', 'mouse'):
            self.update(device=id, properties=dict(IsPresent=False))
        park()
        shot('no-devices')
        has('no-devices', 'No batteries')
        self.update(IsPresent=True, State=2, Percentage=76)
        shot('reconnected')
        has('reconnected', '76%')
        for id in ('headphones', 'keyboard', 'mouse'):
            self.update(device=id, properties=dict(IsPresent=True))
        click(260, 510)
        run(['wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '560x720@60'])
        shot('output-change')
        assert not ocr.find(self.out/'output-change.png', 'Battery Details')
        time.sleep(1)
        current = state()['small']
        click(round(current['cx']*1280/560), round(current['cy']*1024/720))
        shot('compact-details')
        has('compact-details', 'Battery Details')
        pointer('move 640 600')
        pointer('scroll 15')
        shot('compact-scrolled')
        has('compact-scrolled', 'Wireless mouse')
        key(1)
        print('PASS: battery sizes, themes, endpoints, live details, hotplug, selection, undo, empty state, stacks and compact scrolling', flush=True)
