"""Interruptible gallery motion and inert drag previews on the private compositor."""
import time
import tomllib
import re
import ocr


def exercise(fixture, binary, run, msg, shot, click, park, key, pointer):
    original_config = fixture.config.read_text()

    def motion(enabled):
        fixture.config.write_text(original_config + '\n[shell.animation]\n'
                                 f'enabled={str(enabled).lower()}\nspeed=0.25\n')
        msg('config-reload')
        time.sleep(.2)

    def state():
        return tomllib.loads(run([binary, 'config', 'export', 'full']))['desktop_widgets']['widget']

    def has(name, text, **limits):
        assert ocr.find(fixture.out/(name+'.png'), text, **limits), f'{text} missing from {name}'

    def open_gallery():
        pointer('move 444 92')
        pointer('press')
        pointer('release')
        time.sleep(.05)

    msg('color-scheme-set', 'community', 'macOS')
    msg('theme-mode-set', 'light')
    motion(True)
    original = state()
    msg('desktop-widgets-edit')
    time.sleep(.5)
    open_gallery()
    key(1)  # Reverse the entrance before its spring settles.
    motion(False)  # Finish the in-flight exit without leaving an invisible modal.
    shot('reduced-motion-dismissed')
    assert not ocr.find(fixture.out/'reduced-motion-dismissed.png', 'Add Widgets', min_y=170)
    open_gallery()
    shot('reduced-motion-reopened')
    has('reduced-motion-reopened', 'Add Widgets', min_y=170)
    key(1)

    motion(True)
    open_gallery()
    key(1)
    msg('desktop-widgets-exit')  # Destroy the animated tree while the fade owns callbacks.
    msg('desktop-widgets-edit')
    time.sleep(.5)
    open_gallery()
    time.sleep(1.4)
    shot('reopened-after-teardown')
    has('reopened-after-teardown', 'Add Widgets', min_y=170)

    # Find Clock in the actual gallery; its real card should follow the pointer.
    point = ocr.find(fixture.out/'reopened-after-teardown.png', 'Clock', min_y=300, max_y=700)
    assert point, 'Clock category missing'
    click(*point)
    click(520, 332)  # Medium exposes the clock's actual time beside its dial.
    time.sleep(.3)
    pointer('move 740 548')
    pointer('press')
    pointer('move 640 790')
    shot('gallery-drag')
    has('gallery-drag', 'Release to place', min_y=600)
    assert any(re.fullmatch(r'\d{1,2}:?\d{2}', row['text']) and row['top'] > 600
               for row in ocr.words(fixture.out/'gallery-drag.png')), 'Dragged clock content missing'
    key(1)
    pointer('release')
    shot('gallery-drag-cancelled')
    has('gallery-drag-cancelled', 'Add Widgets', min_y=170)
    key(1)
    time.sleep(.8)

    pointer('move 180 220')
    pointer('press')
    pointer('move 500 500')
    shot('widget-lift')
    key(1)
    pointer('release')
    msg('desktop-widgets-exit')
    assert state() == original, 'cancelled motion changed the desktop layout'
    print('PASS: reversed gallery reveal, reduced motion during dismissal, teardown while closing, '
          'real drag preview, lifted widget cancellation and unchanged layout', flush=True)
