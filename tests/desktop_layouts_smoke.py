"""Exercise undo/redo and saved layouts on the private headless compositor."""
import json
import subprocess
import time
import tomllib

import ocr
from desktop_stacks_smoke import Stacks


class Layouts(Stacks):
    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def text(name, label, **limits):
            result = ocr.find(self.out/(name+'.png'), label, **limits)
            assert result, f'{label} missing from {name}'
            return result

        def saved():
            for file in (self.base/'state').rglob('*.toml'):
                value = tomllib.loads(file.read_text()).get('desktop_layouts', {}).get('presets')
                if value:
                    return json.loads(value)['layouts']
            return []

        def paste(value):
            process = subprocess.Popen(['wl-copy', '--foreground', '--', value], env=self.env)
            self.processes.append(process)
            time.sleep(.2)
            key('chord 2 30')
            key('chord 2 47')
            time.sleep(.2)

        def open_layouts(name):
            click(450, 45)
            park()
            shot(name)


        def save(name, monitor=False):
            open_layouts(name+'-dialog')
            click(540, 636)
            paste(name)
            if name == 'Stacked':
                key('chord 2 44')
                key('chord 3 44')  # Text undo/redo must not change the desktop.
            if monitor:
                click(550, 680)
                key(108)
                key(28)
            click(886, 760)
            park()
            shot(name+'-saved')
            found = next((item for item in saved() if item['name'] == name), None)
            assert found is not None, f'layout {name} not persisted'
            key(1)
            return found

        def widgets(layout):
            return {widget['id']: widget for widget in layout['widgets']}

        def drag(x, y, to_x, to_y):
            pointer(f'move {x} {y}')
            pointer('press')
            pointer(f'move {to_x} {to_y}')
            pointer('release')
            park()

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        msg('desktop-widgets-edit')
        original = save('Work')
        assert len(original['widgets']) == 3 and original['output'] == ''
        drag(248, 180, 760, 180)
        stacked = save('Stacked')
        stack = next(widget for widget in stacked['widgets'] if widget['type'] == 'stack')
        assert stack['settings']['members'] == ['clock', 'photos']
        key('chord 2 44')  # Ctrl+Z
        undone = save('Undo stack')
        assert undone['widgets'] == original['widgets'], 'undo did not restore standalone members'
        key('chord 3 44')  # Ctrl+Shift+Z
        redone = save('Redo stack')
        assert redone['widgets'] == stacked['widgets'], 'redo did not restore membership and sources'
        # One continuous move, even with multiple motion events, is one undo step.
        pointer('move 760 180')
        pointer('press')
        pointer('move 790 240')
        pointer('move 840 300')
        pointer('release')
        park()
        moved = save('Moved')
        assert widgets(moved)[stack['id']]['cy'] != stack['cy']
        key('chord 2 44')
        assert save('Undo move')['widgets'] == stacked['widgets']
        key('chord 2 21')  # Ctrl+Y
        assert save('Redo move')['widgets'] == moved['widgets']
        click(248, 480)
        key(111)
        deleted = save('Deleted')
        assert 'tips' not in widgets(deleted)
        click(270, 45)  # Toolbar Undo
        assert save('Undo delete')['widgets'] == moved['widgets']
        # A new edit clears the redo branch.
        click(248, 480)
        drag(248, 480, 300, 600)
        branch = save('Branch')
        key('chord 2 21')
        assert save('No redo')['widgets'] == branch['widgets']
        tips = widgets(branch)['tips']
        x, y = round(tips['cx']), round(tips['cy'])
        click(x, y)
        drag(x+104, y+104, x+168, y+152)
        resized = save('Resized')
        assert widgets(resized)['tips']['box_width'] > 208, 'resize did not change the tile'
        key('chord 2 44')
        assert save('Undo resize')['widgets'] == branch['widgets']
        click(x, y)
        pointer(f'move {x+104} {y+104}')
        pointer('press')
        pointer(f'move {x+180} {y+150}')
        key(1)
        pointer('release')
        assert save('Cancel resize')['widgets'] == branch['widgets']
        click(x, y)
        drag(x, y-116, x+116, y)
        rotated = save('Rotated')
        assert abs(widgets(rotated)['tips']['rotation']) > 1, 'rotation did not change the angle'
        key('chord 2 44')
        assert save('Undo rotation')['widgets'] == branch['widgets']
        click(x, y)
        pointer(f'move {x} {y-116}')
        pointer('press')
        pointer(f'move {x+116} {y}')
        key(1)
        pointer('release')
        assert save('Cancel rotation')['widgets'] == branch['widgets']
        monitor = save('Focus', monitor=True)
        assert monitor['output'] == 'HEADLESS-1' and monitor['widgets'] == branch['widgets']
        # Apply the first saved layout and undo it without closing the editor.
        open_layouts('apply-work')
        click(*text('apply-work', 'Apply', min_y=320, max_y=390, exact=True))
        assert save('Applied')['widgets'] == original['widgets']
        key('chord 2 44')
        assert save('Undo apply')['widgets'] == branch['widgets']
        # Existing saved layouts remain across editor sessions.
        msg('desktop-widgets-exit')
        msg('desktop-widgets-edit')
        time.sleep(.5)
        click(1220, 990)
        key('chord 2 44')
        persisted = save('New session')
        assert persisted['widgets'] == moved['widgets'], 'desktop undo history was lost on reopening the editor'
        key('chord 3 44')
        assert save('Resumed history')['widgets'] == branch['widgets'], 'redo was lost on reopening the editor'
        open_layouts('saved-layouts')
        text('saved-layouts', 'Work', min_y=320, max_y=390, exact=True)
        # Update the first preset using the current arrangement, then remove it.
        click(886, 352)
        assert saved()[0]['widgets'] == branch['widgets'], 'update did not replace the saved arrangement'
        click(926, 352)
        assert not any(item['name'] == 'Work' for item in saved()), 'delete did not remove the saved layout'
        key(1)
        msg('desktop-widgets-exit')
        print('PASS: editor undo/redo, atomic stack/move/delete, history branching, whole-desktop and monitor presets, persistence')
