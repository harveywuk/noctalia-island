"""Exercise stack dragging and rotation on the private headless compositor."""
import json
import subprocess
import time
import tomllib

from PIL import Image
import ocr


class Stacks:
    def __init__(self, base, out, env, config, header, processes):
        self.base, self.out, self.env = base, out, env
        self.config, self.header, self.processes = config, header, processes

    def prepare(self):
        self.photo = self.base/'Blue fixture.png'
        Image.new('RGB', (640, 480), (50, 100, 180)).save(self.photo)
        blocks = []
        for kind, x, y in [('photos', 248, 180), ('clock', 760, 180), ('tips', 248, 480)]:
            blocks.append(f'[desktop_widgets.widget.{kind}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{kind}.settings]\ncard_size="small"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if kind == 'photos':
                blocks.append(f'image_path={json.dumps(str(self.photo))}\n')
        self.config.write_text(self.header+''.join(blocks))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def text(name, label, **limits):
            result = ocr.find(self.out/(name+'.png'), label, **limits)
            assert result, f'{label} missing from {name}'
            return result

        def exported():
            raw = subprocess.check_output([binary, 'config', 'export', 'full'], env=self.env, text=True, stderr=subprocess.DEVNULL)
            return tomllib.loads(raw)['desktop_widgets']['widget']

        def saved(key):
            for file in (self.base/'state').rglob('*.toml'):
                value = tomllib.loads(file.read_text()).get('desktop_stacks', {}).get(key)
                if value:
                    return json.loads(value).get(stack_id)
            return None

        def begin_drag(x, y, to_x, to_y):
            pointer(f'move {x} {y}')
            pointer('press')
            pointer(f'move {to_x} {to_y}')

        def inspector(name):
            msg('desktop-widgets-edit')
            shot(name+'-editor')
            click(760, 180)
            click(732, 92)
            park()
            shot(name)

        def wait_page(expected, timeout=8):
            deadline = time.monotonic()+timeout
            while time.monotonic() < deadline:
                if saved('pages') == expected:
                    return
                time.sleep(.2)
            raise AssertionError(f'stack did not rotate to {expected}; got {saved("pages")}')

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        msg('desktop-widgets-edit')
        shot('before-drag')
        begin_drag(248, 180, 760, 180)
        shot('create-preview')
        text('create-preview', 'Release to create')
        key(1)
        pointer('release')
        msg('desktop-widgets-exit')
        states = exported()
        assert not any(state['type'] == 'stack' for state in states.values()) and states['photos']['cx'] == 248, 'Escape did not cancel stack creation'

        msg('desktop-widgets-edit')
        shot('before-create')
        begin_drag(248, 180, 760, 180)
        pointer('release')
        msg('desktop-widgets-exit')
        states = exported()
        stacks = [(name, data) for name, data in states.items() if data['type'] == 'stack']
        assert len(stacks) == 1, 'drop did not create one stack'
        stack_id, stack = stacks[0]
        assert stack['settings']['members'] == ['clock', 'photos']
        assert (stack['cx'], stack['cy']) == (760, 180) and states['photos']['cx'] == 248

        msg('desktop-widgets-edit')
        shot('before-append')
        begin_drag(248, 480, 760, 180)
        shot('append-preview')
        text('append-preview', 'Release to add')
        pointer('release')
        msg('desktop-widgets-exit')
        assert exported()[stack_id]['settings']['members'] == ['clock', 'photos', 'tips']

        inspector('members')
        _, photo_y = text('members', 'Configure Photos', min_x=250, min_y=220)
        _, clock_y = text('members', 'Configure Clock', min_x=250, min_y=220)
        begin_drag(250, photo_y, 250, clock_y)
        shot('reorder-preview')
        pointer('release')
        msg('desktop-widgets-exit')
        assert exported()[stack_id]['settings']['members'] == ['photos', 'clock', 'tips'], 'drag reorder failed'

        inspector('before-detach')
        _, tips_y = text('before-detach', 'Configure Tips', min_x=250, min_y=220)
        begin_drag(250, tips_y, 1080, 800)
        shot('detach-preview')
        text('detach-preview', 'Release to place')
        key(1)
        pointer('release')
        msg('desktop-widgets-exit')
        assert exported()[stack_id]['settings']['members'] == ['photos', 'clock', 'tips'], 'cancel detached a member'
        inspector('detach-again')
        _, tips_y = text('detach-again', 'Configure Tips', min_x=250, min_y=220)
        begin_drag(250, tips_y, 1080, 800)
        pointer('release')
        msg('desktop-widgets-exit')
        states = exported()
        assert states[stack_id]['settings']['members'] == ['photos', 'clock']
        assert states['tips']['cx'] >= 1070 and states['tips']['cy'] == 800

        inspector('rotation-settings')
        _, rotate_y = text('rotation-settings', 'Automatic Rotation', min_x=220)
        click(707, rotate_y)
        shot('rotation-enabled')
        _, interval_y = text('rotation-enabled', 'Rotate Every', min_x=220)
        click(699, interval_y)
        clip = subprocess.Popen(['wl-copy', '--foreground', '--', '5'], env=self.env)
        self.processes.append(clip)
        time.sleep(.2)
        key('chord 2 30')
        key('chord 2 47')
        key(28)
        msg('desktop-widgets-exit')
        states = exported()
        assert states[stack_id]['settings']['auto_rotate'] and states[stack_id]['settings']['rotation_seconds'] == 5

        # Explicit navigation selects the first page and restarts its interval.
        click(739, 277)
        park()
        wait_page('clock')
        shot('rotated-clock')
        pointer('move 760 180')
        time.sleep(6)
        assert saved('pages') == 'clock', 'hover failed to pause rotation'
        park()
        wait_page('photos')
        shot('rotation-resumed')
        click(841, 277)
        park()
        assert saved('pins') == 'photos', 'pin was not saved'
        time.sleep(6)
        assert saved('pages') == 'photos', 'pinned page rotated'
        msg('desktop-widgets-hide')
        msg('desktop-widgets-show')
        time.sleep(6)
        assert saved('pins') == 'photos' and saved('pages') == 'photos', 'pin did not survive surface reconstruction'
        shot('pinned-photo')
        click(841, 277)
        park()
        wait_page('clock')
        assert saved('pins') == '', 'unpin was not persisted'

        # Pulling a second member out removes the container and keeps both cards.
        inspector('last-members')
        _, photo_y = text('last-members', 'Configure Photos', min_x=250, min_y=220)
        begin_drag(250, photo_y, 1040, 600)
        pointer('release')
        msg('desktop-widgets-exit')
        states = exported()
        assert stack_id not in states and states['clock']['cx'] == 760
        assert states['photos']['settings']['image_path'] == str(self.photo)
        shot('cards-restored')
        print('PASS: stack drop previews, cancel, create, append, drag reorder, detach, rotation, hover pause, pin persistence, resume and final-card recovery', flush=True)
        print(self.out, flush=True)
