"""Long auto-scrolling labels stop when animations are disabled.

Plays a long track title in the Island's expanded media view: with animations on the
title strip changes between frames; after `[shell.animation] enabled = false` it must
stay still (shown truncated) while playback time keeps updating.
"""
import pathlib
import subprocess
import sys
import time

from PIL import Image, ImageChops

REPO = pathlib.Path(__file__).resolve().parents[1]
TITLE_STRIP = (601, 47, 777, 70)


def prepare(base, cfg, env):
    pass


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    proto = REPO/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(REPO/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={x},y={y}}})')
        for step in ('relative 1 0', 'relative -1 0'):
            pointer.stdin.write(step+'\n'); pointer.stdin.flush()
            assert pointer.stdout.readline().strip() == 'ok'
        time.sleep(.15)

    def strip_changes(name):
        move(1100, 600); time.sleep(1)
        move(640, 35); time.sleep(1.5)
        first = out/f'{name}-a.png'; second = out/f'{name}-b.png'
        run(['grim', '-o', 'TEST-1', str(first)])
        time.sleep(2.2)
        run(['grim', '-o', 'TEST-1', str(second)])
        a = Image.open(first).convert('RGB').crop(TITLE_STRIP)
        b = Image.open(second).convert('RGB').crop(TITLE_STRIP)
        return ImageChops.difference(a, b).getbbox() is not None

    env['ISLAND_TEST_ART'] = (REPO/'assets/noctalia-wallpaper.png').as_uri()
    env['ISLAND_TEST_EVENTS'] = str(out/'player-actions.log')
    env['ISLAND_TEST_TITLE'] = 'A little closer to home — a long track title that keeps scrolling as playback advances'
    env['ISLAND_TEST_TICK'] = '1'
    start([sys.executable, str(REPO/'tests/fixtures/island_player.py')], 'player.log')
    time.sleep(4)
    dispatch('hl.dsp.focus({monitor="TEST-1"})')
    try:
        assert strip_changes('animated'), 'Long title should scroll while animations are enabled'
        config = cfg/'config.toml'
        config.write_text(config.read_text() + '\n[shell.animation]\nenabled = false\n')
        msg('config-reload'); time.sleep(1.5)
        assert not strip_changes('reduced'), 'Long title must stay still when animations are disabled'
        assert shell.poll() is None
        print('PASS: marquee scrolls with motion and stops when animations are disabled', flush=True)
    finally:
        if pointer.poll() is None:
            pointer.terminate(); pointer.wait(timeout=5)
