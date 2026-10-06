"""Exercise desktop context menus on a private bus and compositor."""
import json
import subprocess
import sys
import time
import tomllib
from pathlib import Path

import ocr
from desktop_stacks_smoke import Stacks


class QuickActions(Stacks):
    def prepare(self):
        blocks = []
        for widget, kind, x, y in [('clock', 'clock', 128, 144), ('notes', 'notes', 300, 750),
                                   ('member_clock', 'clock', 850, 200), ('media', 'media_player', 850, 720),
                                   ('stack', 'stack', 850, 470)]:
            blocks.append(f'[desktop_widgets.widget.{widget}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{widget}.settings]\ncard_size="small"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if kind == 'stack':
                blocks.append('members=["member_clock", "media"]\nsmart_rotate=false\nauto_rotate=false\n')
        self.config.write_text(self.header+''.join(blocks))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def state():
            return tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets'].get('widget', {})

        def saved(key_name):
            for file in (self.base/'state').rglob('*.toml'):
                raw = tomllib.loads(file.read_text()).get('desktop_stacks', {}).get(key_name)
                if raw:
                    return json.loads(raw).get('stack')

        def open_menu(widget, name):
            current = state()[widget]
            pointer(f'move {round(current["cx"])} {round(current["cy"])}')
            pointer('right-press')
            pointer('right-release')
            park()
            shot(name)

        def choose(name, label):
            screenshot = self.out/(name+'.png')
            # Hover another enabled row: menus retain their highlight after the
            # pointer leaves, and OCR cannot reliably read white text on blue.
            hover = None
            for size in ('Small', 'Medium', 'Large'):
                if size != label:
                    hover = ocr.find(screenshot, size)
                    if hover:
                        break
            assert hover, f'hover target missing from {name}'
            pointer(f'move {hover[0]} {hover[1]}')
            shot(name)
            last_row = ocr.find(screenshot, 'Edit Widgets', exact=True)
            assert last_row, f'context menu missing from {name}'
            point = ocr.find(screenshot, label, max_y=last_row[1]+12,
                             exact=label not in ('Small', 'Medium', 'Large', 'Smart Rotate',
                                                 'Pin this card', 'Unpin and resume rotation'))
            assert point, f'{label} missing from {name}'
            click(*point)
            park()
            time.sleep(.7)

        def action(widget, label, name):
            open_menu(widget, name)
            choose(name, label)

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        original = state()
        open_menu('clock', 'clock-menu')
        key(1)
        assert state() == original, 'Escape modified the desktop'
        open_menu('clock', 'dismiss-menu')
        click(1220, 990)
        assert state() == original, 'outside click modified the desktop'
        action('clock', 'Large', 'resize-menu')
        large = state()
        assert large['clock']['settings']['card_size'] == 'large'
        assert large['clock']['cx'] >= 216 and large['clock']['cy'] >= 216, 'larger card was not kept on-screen'
        action('clock', 'Undo', 'undo-resize-menu')
        assert state()['clock'] == original['clock'], 'quick resize did not undo'
        action('clock', 'Redo', 'redo-resize-menu')
        assert state()['clock'] == large['clock'], 'quick resize did not redo'
        action('clock', 'Duplicate Widget', 'duplicate-menu')
        duplicated = state()
        assert len(duplicated) == len(large)+1, 'duplicate did not create one widget'
        copied = next(widget for widget in duplicated if widget not in large)
        assert duplicated[copied]['settings'] == large['clock']['settings'], 'duplicate lost settings'
        action(copied, 'Remove Widget', 'remove-menu')
        assert copied not in state(), 'remove did not save'
        # Editor history includes desktop actions, and survives returning to the desktop.
        action('notes', 'Edit Widgets', 'editor-menu')
        time.sleep(1)
        click(1220, 990)  # The editor takes keyboard focus on demand.
        shot('editor-history-before')
        key('chord 2 44')
        shot('editor-history-after')
        msg('desktop-widgets-exit')
        assert copied in state(), 'editor could not undo desktop removal'
        action('notes', 'Redo', 'redo-remove-menu')
        assert copied not in state(), 'desktop lost redo after returning from editor'
        action('clock', 'Configure', 'configure-clock-menu')
        shot('clock-inspector')
        assert ocr.find(self.out/'clock-inspector.png', 'Card Size'), 'Configure did not open the inspector'
        msg('desktop-widgets-exit')
        action('notes', 'Configure', 'configure-notes-menu')
        shot('notes-setup')
        assert ocr.find(self.out/'notes-setup.png', 'Cancel'), 'Configure did not open guided setup'
        key(1)
        msg('desktop-widgets-exit')
        print('PASS: desktop actions, guided/inspector setup and shared history', flush=True)
        repo = Path(__file__).resolve().parents[1]
        self.env.update(ISLAND_TEST_ART=(repo/'assets/noctalia-wallpaper.png').as_uri(),
                        ISLAND_TEST_EVENTS=str(self.out/'player-actions.log'), ISLAND_TEST_CAN_RAISE='1')
        (self.out/'player-actions.log').write_text('')
        with (self.out/'player.log').open('w') as log:
            player = subprocess.Popen([sys.executable, str(repo/'tests/fixtures/island_player.py')], env=self.env, stdout=log, stderr=log)
        self.processes.append(player)
        time.sleep(1)
        action('stack', 'Smart Rotate', 'smart-menu')
        assert state()['stack']['settings']['smart_rotate'], 'Smart Rotate was not enabled'
        time.sleep(2)
        assert saved('pages') == 'media', 'Smart Rotate did not select Now Playing'
        action('stack', 'Open Player', 'player-menu')
        assert 'Raise' in (self.out/'player-actions.log').read_text(), 'Open Player did not call MPRIS Raise'
        assert 'PlayPause' not in (self.out/'player-actions.log').read_text(), 'right-click activated playback controls'
        action('stack', 'Pin this card', 'pin-menu')
        assert saved('pins') == 'media', 'pin was not saved'
        action('stack', 'Unpin and resume rotation', 'unpin-menu')
        assert saved('pins') == '', 'pin was not cleared'
        before_copy = state()
        action('stack', 'Duplicate Widget', 'duplicate-stack-menu')
        after_copy = state()
        assert len(after_copy) == len(before_copy)+3, 'stack copy lost its members'
        stack_copy = next(key for key in after_copy if key not in before_copy and after_copy[key]['type'] == 'stack')
        assert all(member not in before_copy for member in after_copy[stack_copy]['settings']['members']), 'stack copy shares original members'
        action('notes', 'Undo', 'undo-stack-copy-menu')
        assert len(state()) == len(before_copy)
        action('stack', 'Edit Stack', 'edit-stack-menu')
        shot('stack-setup')
        assert ocr.find(self.out/'stack-setup.png', 'Set Up Widget Stack'), 'Edit Stack did not open member setup'
        key(1)
        msg('desktop-widgets-exit')
        action('stack', 'Remove Widget', 'remove-stack-menu')
        assert 'stack' not in state() and 'media' in state() and 'member_clock' in state(), 'stack removal deleted its cards'
        action('notes', 'Undo', 'undo-stack-remove-menu')
        assert 'stack' in state(), 'stack removal could not undo'
        msg('desktop-widgets-hide')
        msg('desktop-widgets-show')
        assert state()['stack']['settings']['smart_rotate'], 'quick changes did not survive reconstruction'
        shot('finished')
        print('PASS: stack menus and MPRIS Raise', flush=True)
        for widget in ('stack', 'media', 'member_clock', 'notes', 'clock'):
            action(widget, 'Remove Widget', 'remove-last-'+widget)
        assert not state(), 'last widget removal was not persisted'
        msg('desktop-widgets-edit')
        time.sleep(.7)
        click(1220, 990)
        key('chord 2 44')
        msg('desktop-widgets-exit')
        assert list(state()) == ['clock'], 'editor could not restore the last removed widget'
        print('PASS: desktop quick menus, size/clamping, configure, copy/remove, shared undo/redo, stack actions and MPRIS Raise', flush=True)
