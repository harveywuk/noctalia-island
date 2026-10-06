"""Gallery search, favorites and draft placement on a private compositor and bus."""
import json
import time
import tomllib

from PIL import Image
import ocr
from desktop_stacks_smoke import Stacks


class Gallery(Stacks):
    def prepare(self):
        self.album = self.base/'album'
        self.album.mkdir()
        Image.new('RGB', (600, 400), (40, 100, 180)).save(self.album/'Gallery photo.png')
        blocks = []
        widgets = [('clock', 'clock', 180, 220)]
        widgets += [(f'member{i}', 'clock', 1060, 270) for i in range(8)]
        widgets += [('full', 'stack', 1060, 270)]
        for widget, kind, x, y in widgets:
            blocks.append(f'[desktop_widgets.widget.{widget}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{widget}.settings]\ncard_size="small"\n'
                          'background_padding=14\nbackground_radius=16\n')
            if kind == 'stack':
                blocks.append('members='+json.dumps([f'member{i}' for i in range(8)])+'\n')
        self.config.write_text(self.header+''.join(blocks))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def find(name, label, **limits):
            point = ocr.find(self.out/(name+'.png'), label, **limits)
            assert point, f'{label} missing from {name}'
            return point

        def state():
            return tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets'].get('widget', {})

        def saved_favorite():
            return any(tomllib.loads(file.read_text()).get('desktop_gallery', {}).get('favorite_'+b'clock'.hex())
                       for file in (self.base/'state').rglob('*.toml'))

        def fill(value):
            # Type through the private keyboard so every search refresh is exercised.
            keys = dict(zip('qwertyuiop', range(16, 26)))
            keys.update(zip('asdfghjkl', range(30, 39)))
            keys.update(zip('zxcvbnm', range(44, 51)))
            keys.update(zip('1234567890', range(2, 12)))
            keys.update({'/': 53, '-': 12, '.': 52, ' ': 57})
            key('chord 2 30')
            key(14)
            for char in value:
                if char == '_':
                    key('chord 1 12')
                elif char.isupper():
                    key(f'chord 1 {keys[char.lower()]}')
                else:
                    key(keys[char])
            time.sleep(.3)

        def gallery():
            click(444, 92)
            time.sleep(.5)

        def query(value, name):
            click(610, 244)
            fill(value)
            shot(name)

        def category(label, name):
            click(292, 292)
            shot(name+'-categories')
            key(102)  # Home, then keyboard navigation exercises the native select.
            for _ in range(['All Widgets', 'Favorites', 'Productivity', 'Media', 'System',
                            'Home', 'Information', 'Appearance', 'Plugins'].index(label)):
                key(108)
            key(28)
            park()
            shot(name)
            find(name, label, min_y=270, max_y=305)

        def drag(x, y, name):
            pointer('move 740 548')
            pointer('press')
            pointer(f'move {x} {y}')
            shot(name)

        def reopen():
            msg('desktop-widgets-exit')
            current = state()
            msg('desktop-widgets-edit')
            time.sleep(.5)
            return current

        time.sleep(3)
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', 'light')
        original = state()
        msg('desktop-widgets-edit')
        time.sleep(.5)
        gallery()
        query('zz-no-widget', 'no-results')
        find('no-results', 'No widgets match')
        query('cloc', 'partial-search')
        key(37)  # Type k after the results rebuild. Focus and the caret must survive.
        shot('clock-search')
        find('clock-search', 'clock', min_y=230, max_y=270)
        find('clock-search', 'Clock', min_x=420, min_y=270, max_y=320)
        key(45)  # x makes the query fail, then Backspace restores it.
        shot('typed-no-match')
        find('typed-no-match', 'No widgets match')
        key(14)
        shot('typed-restored')
        find('typed-restored', 'Clock', min_x=420, min_y=270, max_y=320)
        category('Media', 'category-no-match')
        find('category-no-match', 'No widgets match')
        category('All Widgets', 'all-again')
        click(1084, 292)  # Favorite the selected clock.
        time.sleep(.4)
        assert saved_favorite(), 'favorite was not saved to state'
        category('Favorites', 'favorites-clock')
        query('', 'favorites-only')
        find('favorites-only', 'Clock', min_x=420, min_y=270, max_y=320)
        key(1)
        assert reopen() == original, 'browsing or favoriting changed the desktop'
        gallery()
        shot('favorites-reopened')
        find('favorites-reopened', 'Favorites')
        find('favorites-reopened', 'Clock', min_x=420, min_y=270, max_y=320)
        click(1084, 292)
        shot('favorites-empty')
        find('favorites-empty', 'Star a widget')
        assert not saved_favorite(), 'unfavorite was not persisted'
        category('All Widgets', 'all-widgets')
        query('clock', 'clock-before-cancel')
        drag(640, 810, 'placement-cancel')
        find('placement-cancel', 'Release to place')
        key(1)
        pointer('release')
        shot('gallery-restored')
        find('gallery-restored', 'Add Widgets', min_y=170, max_y=230)
        key(1)
        assert reopen() == original, 'canceled placement changed the desktop'
        print('PASS: gallery search, focus, combined filters, favorites and canceled placement', flush=True)

        gallery()
        query('clock', 'clock-medium')
        click(520, 332)  # Medium; verify the saved preset after placement.
        drag(640, 810, 'placement-medium')
        pointer('release')
        time.sleep(.5)
        placed = reopen()
        new_id = next(key for key in placed if key not in original)
        assert len(placed) == len(original)+1 and placed[new_id]['settings']['card_size'] == 'medium'
        assert abs(placed[new_id]['cx']-640) <= 16 and abs(placed[new_id]['cy']-810) <= 16
        click(1220, 990)
        key('chord 2 44')
        assert reopen() == original, 'one Undo did not remove the gallery addition'
        click(1220, 990)
        key('chord 3 44')
        assert reopen() == placed, 'Redo lost the placement'

        gallery()
        query('weather', 'weather-gallery')
        drag(180, 220, 'create-stack')
        find('create-stack', 'Release to create')
        pointer('release')
        time.sleep(.5)
        stacked = reopen()
        stack_id = next(key for key in stacked if key not in placed and stacked[key]['type'] == 'stack')
        assert len(stacked[stack_id]['settings']['members']) == 2
        gallery()
        query('photos', 'photos-gallery')
        drag(180, 220, 'append-photos-cancel')
        find('append-photos-cancel', 'Release to add')
        pointer('release')
        shot('photos-setup-cancel')
        find('photos-setup-cancel', 'Set Up Photos')
        key(1)  # Discard source setup, then close the restored gallery.
        key(1)
        assert reopen() == stacked, 'canceled guided drop changed stack membership'
        gallery()
        query('photos', 'photos-gallery-again')
        drag(180, 220, 'append-photos')
        pointer('release')
        shot('photos-setup')
        _, folder_y = find('photos-setup', 'Folder', min_x=340, min_y=290, max_y=720, exact=True)
        click(760, folder_y)
        fill(str(self.album))
        click(866, 788)
        appended = reopen()
        photos = next(key for key in appended if key not in stacked)
        assert appended[photos]['settings']['folder_path'] == str(self.album)
        assert appended[stack_id]['settings']['members'][-1] == photos
        click(1220, 990)
        key('chord 2 44')
        assert reopen() == stacked, 'guided addition and stack membership did not undo together'
        print('PASS: placement, Undo/Redo, stack creation, guided stack addition and cancel', flush=True)

        gallery()
        query('clock', 'full-stack-gallery')
        drag(1060, 270, 'full-stack-placement')
        find('full-stack-placement', 'Release to place')
        pointer('release')
        after_full = reopen()
        assert len(after_full) == len(stacked)+1 and len(after_full['full']['settings']['members']) == 8
        gallery()
        query('weather', 'compact-before')
        run(['wlr-randr', '--output', 'HEADLESS-1', '--custom-mode', '640x720@60'])
        time.sleep(.8)
        shot('compact-gallery')
        find('compact-gallery', 'Weather', min_y=120, max_y=240)
        find('compact-gallery', 'Small')
        find('compact-gallery', 'Medium')
        find('compact-gallery', 'Large')
        key(1)
        msg('desktop-widgets-exit')
        print('PASS: full-stack fallback and compact gallery; all gallery checks passed', flush=True)
