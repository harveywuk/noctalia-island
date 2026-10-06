"""Exercise guided source setup and stack lifecycle in the isolated desktop harness."""
import http.server
import subprocess
import threading
import time
import tomllib

from PIL import Image
import ocr


class Guided:
    def __init__(self, base, out, env, config, header, processes):
        self.base, self.out, self.env = base, out, env
        self.config, self.header, self.processes = config, header, processes
        self.requests = 0
        self.clipboard = None

    def prepare(self):
        self.album = self.base/'album'
        self.album.mkdir()
        Image.new('RGB', (640, 480), (50, 100, 180)).save(self.album/'Fixture photo.png')
        owner = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                owner.requests += 1
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'<rss><channel><item><title>Guided headline</title><link>https://example.test/story</link></item></channel></rss>')
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f'http://127.0.0.1:{self.server.server_port}/feed'
        self.env.update(no_proxy='127.0.0.1,localhost', NO_PROXY='127.0.0.1,localhost')
        blocks = []
        for kind, x, y in [('photos', 248, 180), ('reminders', 760, 180), ('news', 248, 440), ('clock', 760, 440)]:
            blocks.append(f'[desktop_widgets.widget.{kind}]\ntype="{kind}"\noutput="HEADLESS-1"\n'
                          f'cx={x}.0\ncy={y}.0\nplacement_width=1280.0\nplacement_height=1024.0\n'
                          f'[desktop_widgets.widget.{kind}.settings]\ncard_size="medium"\n'
                          'background_padding=14\nbackground_radius=16\n')
        self.config.write_text(self.header+''.join(blocks))

    def exercise(self, binary, run, msg, shot, click, park, key, pointer):
        def text(name, label, **limits):
            point = ocr.find(self.out/(name+'.png'), label, **limits)
            assert point, f'{label} missing from {name}'
            return point
        def paste(value):
            if self.clipboard:
                self.clipboard.terminate()
                self.clipboard.wait(timeout=5)
            self.clipboard = subprocess.Popen(['wl-copy', '--foreground', '--', value], env=self.env)
            self.processes.append(self.clipboard)
            time.sleep(.2)
            key('chord 2 30')  # Select all.
            key('chord 2 47')  # Paste.
            time.sleep(.3)
        def field(name, label, value):
            _, y = text(name, label, min_x=340, min_y=290, max_y=720, exact=True)
            click(760, y)
            paste(value)
        def exported():
            raw = subprocess.check_output([binary, 'config', 'export', 'full'], env=self.env, text=True, stderr=subprocess.DEVNULL)
            return tomllib.loads(raw)['desktop_widgets']['widget']
        time.sleep(3)
        msg("color-scheme-set", "community", "macOS")
        msg("theme-mode-set", "light")
        shot('empty-cards')
        click(*text('empty-cards', 'Configure', max_y=300))
        shot('photos-setup')
        text('photos-setup', 'Set Up Photos', min_x=340)
        click(866, 788)  # Primary setup action.
        shot('photos-required')
        text('photos-required', 'Choose a readable image')
        field('photos-required', 'Folder', str(self.album))
        click(866, 788)  # Primary setup action.
        msg('desktop-widgets-exit')
        shot('photos-configured')
        text('photos-configured', 'Fixture photo')
        assert exported()['photos']['settings']['folder_path'] == str(self.album)
        click(*text('photos-configured', 'Configure', min_x=550, max_y=300))
        shot('reminders-setup')
        click(460, 392)
        paste('Buy bread')
        key(28)
        shot('reminders-item')
        text('reminders-item', 'Buy bread', min_x=340)
        click(866, 788)  # Primary setup action.
        msg('desktop-widgets-exit')
        shot('reminders-configured')
        text('reminders-configured', 'Buy bread')
        click(*text('reminders-configured', 'Configure', min_y=340, max_y=540))
        shot('news-setup')
        field('news-setup', 'Feed URL', 'file:///invalid')
        click(866, 788)  # Primary setup action.
        shot('news-invalid')
        text('news-invalid', 'Enter a complete')
        field('news-invalid', 'Feed URL', self.url)
        assert self.requests == 0, 'draft setup fetched a feed before saving'
        click(866, 788)  # Primary setup action.
        msg('desktop-widgets-exit')
        time.sleep(1)
        shot('news-configured')
        text('news-configured', 'Guided headline')
        assert self.requests > 0 and exported()['news']['settings']['feed_url'] == self.url
        msg('desktop-widgets-edit')
        shot('editor')
        click(248, 240)
        click(732, 92)
        shot('photo-inspector')
        click(*text('photo-inspector', 'Configure', min_x=220, min_y=150))
        shot('photos-edit-again')
        field('photos-edit-again', 'Folder', '/missing')
        key(1)  # Cancel while the text field still has focus.
        msg('desktop-widgets-exit')
        assert exported()['photos']['settings']['folder_path'] == str(self.album), 'cancel leaked draft edits'
        msg('desktop-widgets-edit')
        shot('editor-before-stack')
        click(444, 92)
        shot('gallery')
        for attempt in range(15):
            point = ocr.find(self.out/'gallery.png', 'Widget Stack', min_y=300, max_y=850)
            if point:
                break
            pointer('move 330 590')
            pointer('scroll 3')
            shot('gallery')
        assert point, 'Widget Stack missing from gallery'
        click(*point)
        shot('stack-gallery')
        click(476, 830)
        shot('stack-setup')
        for label in ('Photos', 'Clock'):
            _, y = text('stack-setup', label, min_x=360, min_y=300, max_y=690, exact=True)
            click(368, y)
            shot('stack-setup')
        click(866, 788)
        msg('desktop-widgets-exit')
        park()
        shot('stack-created')
        states = exported()
        stacks = [(name, data) for name, data in states.items() if data['type'] == 'stack']
        assert len(stacks) == 1
        stack_id, stack = stacks[0]
        assert stack['settings']['members'] == ['photos', 'clock']
        x, y = round(stack['cx']), round(stack['cy'])
        def photo_fraction(name):
            pixels = list(Image.open(self.out/(name+'.png')).convert('RGB').crop((x-80, y-80, x+80, y+60)).getdata())
            return sum(b > r+50 and b > g+30 for r, g, b in pixels)/len(pixels)
        assert photo_fraction('stack-created') > .6, 'stack did not start on Photos'
        pointer(f'move {x} {y}')
        pointer('scroll 1')
        park()
        shot('stack-clock')
        assert photo_fraction('stack-clock') < .2, 'scroll did not switch to Clock'
        saved = [p.read_text() for p in (self.base/'state').rglob('*.toml')]
        assert any('desktop_stacks' in body and 'clock' in body for body in saved), 'stack page was not saved'
        msg('desktop-widgets-hide')
        msg('desktop-widgets-show')
        shot('stack-restored')
        assert photo_fraction('stack-restored') < .2, 'stack did not restore its Clock page'
        # Dot navigation works and survives reconstruction of the shell surfaces.
        click(x-9, y+97)
        park()
        shot('stack-photo')
        text('stack-photo', 'Fixture photo')
        assert photo_fraction('stack-photo') > .6, 'first dot did not switch to Photos'
        msg('desktop-widgets-edit')
        shot('stack-editor')
        click(x, y)
        key('chord 2 46')  # Copy the selected stack.
        key('chord 2 47')  # Paste with independent member definitions.
        msg('desktop-widgets-exit')
        copied = exported()
        copies = [(name, data) for name, data in copied.items() if data['type'] == 'stack' and name != stack_id]
        assert len(copies) == 1, 'copy/paste did not create a second stack'
        copy_id, copy = copies[0]
        copy_members = copy['settings']['members']
        assert len(copy_members) == 2 and not set(copy_members) & {'photos', 'clock'}, 'copied stack shares member IDs'
        assert copied[copy_members[0]]['settings']['folder_path'] == str(self.album)
        assert copied[copy_members[1]]['type'] == 'clock'
        msg('desktop-widgets-edit')
        shot('copied-stack-editor')
        click(round(copy['cx']), round(copy['cy']))
        key(111)  # Removing the copied stack releases its copied cards.
        msg('desktop-widgets-exit')
        released = exported()
        assert copy_id not in released and stack_id in released
        assert all(member in released for member in copy_members), 'removing a stack deleted member definitions'
        msg('desktop-widgets-edit')
        shot('stack-editor-again')
        click(x, y)
        click(732, 92)
        shot('stack-inspector')
        click(*text('stack-inspector', 'Unstack Widgets', min_x=220, min_y=150))
        msg('desktop-widgets-exit')
        shot('unstacked')
        states = exported()
        assert not any(data['type'] == 'stack' for data in states.values()), 'unstack left a stack behind'
        assert states['photos']['settings']['folder_path'] == str(self.album) and states['clock']['type'] == 'clock', 'unstack lost member definitions'
        text('unstacked', 'Fixture photo')
        print('PASS: guided setup, validation, draft cancellation, reminders, RSS, gallery stack creation, scrolling, dots, page persistence, independent copies, removal and unstacking', flush=True)
        print(self.out, flush=True)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
