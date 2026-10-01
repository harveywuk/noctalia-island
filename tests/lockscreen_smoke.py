"""Lock screen appearance on private outputs: idle, typing and a failed attempt.

Screenshots only; the session is never unlocked (the harness tears it down), so no
credentials are involved. Set NOCTALIA_LOCK_THEME=light to capture light mode.
"""
import os
import pathlib
import subprocess
import time


def prepare(base, cfg, env):
    config = cfg/'config.toml'
    config.write_text(config.read_text().replace('[shell]\n', '[shell]\noffline_mode=true\n', 1))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    proto = repo/'protocols/virtual-keyboard-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(proto), str(base/'keyboard-client.h')])
    run(['wayland-scanner', 'private-code', str(proto), str(base/'keyboard-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_keyboard.c'), str(base/'keyboard-code.c'),
         '-lwayland-client', '-lxkbcommon', '-o', str(base/'keyboard')])
    keyboard = subprocess.Popen([str(base/'keyboard')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def key(value):
        keyboard.stdin.write(str(value)+'\n'); keyboard.stdin.flush()
        assert keyboard.stdout.readline().strip() == 'ok'
        time.sleep(.12)

    def shot(name):
        for output in ('TEST-1', 'TEST-2'):
            run(['grim', '-o', output, str(out/f'{name}-{output}.png')])

    try:
        msg('color-scheme-set', 'community', 'macOS')
        msg('theme-mode-set', os.environ.get('NOCTALIA_LOCK_THEME', 'dark'))
        time.sleep(1)
        msg('session', 'lock')
        time.sleep(3)
        assert shell.poll() is None
        shot('lock-idle')
        for code in (30, 48, 46, 32):  # a b c d
            key(code)
        time.sleep(.5)
        shot('lock-typing')
        key(28)  # Enter: PAM rejects the bogus password for this user
        # A burst around the failure shows the password field shaking.
        deadline = time.monotonic() + 5
        frame = 0
        while time.monotonic() < deadline:
            run(['grim', '-g', '0,480 1280x80', str(out/f'lock-burst-{frame:03d}.png')])
            frame += 1
        shot('lock-failed')
        assert shell.poll() is None
        print('PASS: lock screen captured (idle, typing, failed attempt)', flush=True)
    finally:
        if keyboard.poll() is None:
            keyboard.terminate(); keyboard.wait(timeout=5)
