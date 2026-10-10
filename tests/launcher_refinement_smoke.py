"""Native launcher: contextual actions, search restoration, scrolling and both densities."""
import json
import pathlib
import subprocess
import time

import ocr


def prepare(base, cfg, env):
    apps = pathlib.Path(env['XDG_DATA_HOME']) / 'applications'
    apps.mkdir(parents=True, exist_ok=True)
    marker = base / 'launcher-action'
    for name, actions in (('Browser', [('NewWindow', 'New window'), ('NewPrivateWindow', 'New private window')]),
                          ('Actions', [(f'Test{i:02d}', f'Fixture action {i:02d}') for i in range(20)])):
        entry = '[Desktop Entry]\nType=Application\nName=Orbit '+name+'\nComment=Launcher refinement fixture\n'
        entry += f'Exec=sh -c "echo open > {marker}"\nIcon=web-browser\n'
        entry += 'Actions=' + ';'.join(key for key, _ in actions) + ';\n'
        for key, label in actions:
            entry += f'\n[Desktop Action {key}]\nName={label}\nExec=sh -c "echo {key} > {marker}"\n'
        (apps / f'orbit-{name.lower()}.desktop').write_text(entry)
    state = pathlib.Path(env['XDG_STATE_HOME']) / 'noctalia'
    state.mkdir(parents=True, exist_ok=True)
    (state/'recent_results.json').write_text(json.dumps([{'provider': 'Applications', 'id': str(apps/'orbit-browser.desktop')}, {'provider': 'Applications', 'id': str(apps/'orbit-actions.desktop')}]))


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    helpers = []
    for kind, protocol, libs in (
        ('keyboard', repo/'protocols/virtual-keyboard-unstable-v1.xml', ['-lwayland-client', '-lxkbcommon']),
        ('pointer', repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml', ['-lwayland-client']),
    ):
        run(['wayland-scanner', 'client-header', str(protocol), str(base/(kind+'-client.h'))])
        run(['wayland-scanner', 'private-code', str(protocol), str(base/(kind+'-code.c'))])
        run(['cc', '-I'+str(base), str(repo/f'tests/fixtures/island_{kind}.c'), str(base/(kind+'-code.c')),
             *libs, '-o', str(base/kind)])
        helpers.append(subprocess.Popen([str(base/kind)], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True))
    keyboard, pointer = helpers
    original = (cfg/'config.toml').read_text()
    output = 'TEST-1'
    scale = 1
    codes = dict(zip('qwertyuiop', range(16, 26))) | dict(zip('asdfghjkl', range(30, 39))) | dict(zip('zxcvbnm', range(44, 51)))
    codes.update({' ': 57, '/': 53, '0': 11, **{str(n): n+1 for n in range(1, 10)}})

    def command(proc, value):
        proc.stdin.write(str(value)+'\n'); proc.stdin.flush()
        assert proc.stdout.readline().strip() == 'ok'
    def key(value): command(keyboard, value); time.sleep(.12)
    def type_text(text):
        for char in text: command(keyboard, codes[char])
        time.sleep(.6)
    def move(x, y):
        dispatch(f'hl.dsp.cursor.move({{x={int(x/scale)},y={int(y/scale)+(720 if output == "TEST-2" else 0)}}})')
        command(pointer, 'relative 1 0'); command(pointer, 'relative -1 0'); time.sleep(.25)
    def click():
        command(pointer, 'press'); command(pointer, 'release'); time.sleep(.5)
    def shot(name):
        time.sleep(.45)
        path = out/(name+'.png'); run(['grim', '-o', output, str(path)]); return path
    def contains(path, label):
        assert ocr.find(path, label), f'{label!r} missing from {path.name}'
    def open_query(text=''):
        msg('panel-close'); time.sleep(.2)
        msg('panel-toggle', 'launcher', text) if text else msg('panel-toggle', 'launcher')
        time.sleep(.65)
    def action_view(): key('chord 1 28'); time.sleep(.4)
    def configure(compact=False, placement='island', ui_scale=1):
        msg('panel-close')
        config = original
        if placement == 'floating':
            config = config.replace('[island]\nenabled=true', '[island]\nenabled=false')
            config = config.replace('[bar.default]\nenabled=false', '[bar.default]\nenabled=true')
        (cfg/'config.toml').write_text(config + f'\n[shell.launcher]\ncompact={str(compact).lower()}\n'
            f'[shell.panel]\nlauncher_placement="floating"\n[accessibility]\nui_scale={ui_scale}\n')
        msg('config-reload'); time.sleep(.75)

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})'); move(1100, 650)
        msg('color-scheme-set', 'community', 'macOS')
        configure()
        open_query(); contains(shot('launcher-idle'), 'Start typing')
        key(64); contains(shot('launcher-idle-browse'), 'Applications')
        open_query()
        # Keyboard selection on idle; the first Down must select the first result.
        key(108); action_view(); contains(shot('launcher-idle-actions'), 'Back to results'); key(1); key(1)
        open_query('orbit browser')
        contains(shot('launcher-results'), 'Orbit Browser')
        action_view(); actions = shot('launcher-actions'); contains(actions, 'New private window')
        assert not ocr.find(actions, 'Launcher refinement fixture'), 'Results leaked behind action view'
        type_text('private'); filtered = shot('launcher-actions-filtered'); contains(filtered, 'New private window')
        assert not ocr.find(filtered, 'Pin to Launcher'), 'Action filter did not narrow the list'
        key(1); restored = shot('launcher-restored'); contains(restored, 'orbit browser')
        contains(restored, 'Launcher refinement fixture')
        action_view(); type_text('zzzzzz'); contains(shot('launcher-actions-empty'), 'No matching actions')
        key(1); action_view(); type_text('private'); key(28)
        wait(lambda: (base/'launcher-action').exists(), 'Filtered action did not execute')
        assert (base/'launcher-action').read_text().strip() == 'NewPrivateWindow'
        print('PASS: contextual actions, filtered activation, empty state, Escape search restoration', flush=True)

        # Pointer entry uses the result ellipsis and returns through the footer.
        open_query('orbit browser')
        result = shot('launcher-pointer-results')
        title = ocr.find(result, 'Orbit Browser', min_y=100); assert title
        # The floating surface is centred; the ellipsis is 44 px inside its right edge.
        move(640 + 320 - 44, title[1]+8); click()
        contains(shot('launcher-pointer-actions'), 'New private window')
        back = ocr.find(shot('launcher-pointer-back'), 'Back to results'); assert back
        move(*back); click(); contains(shot('launcher-pointer-restored'), 'Launcher refinement fixture')
        # Returning through a button must restore typing focus as well as the query.
        key('chord 2 30'); key('chord 2 37'); type_text('orbit actions')
        contains(shot('launcher-pointer-typing'), 'Orbit Actions')
        move(1100, 650)
        # Lists beyond the old 14-row limit must scroll, with pointer and keyboard agreeing.
        open_query('orbit actions'); action_view(); key(103)
        contains(shot('launcher-actions-end'), 'Copy Hotkey Command')
        for _ in range(3): key(103)
        last = shot('launcher-actions-long-selected'); target = ocr.find(last, 'Fixture action 19'); assert target
        move(*target); click()
        wait(lambda: (base/'launcher-action').read_text().strip() == 'Test19', 'Scrolled pointer action executed the wrong entry')
        print('PASS: pointer entry/back and long action list keyboard/pointer mapping', flush=True)

        # Actions that stay inside the launcher retain argument and alias editing.
        move(1100, 650)
        open_query('/link duckduckgo'); action_view(); key(28)
        contains(shot('launcher-arguments'), 'Tab for the next argument')
        key(1); contains(shot('launcher-arguments-return'), '/link duckduckgo')
        open_query('orbit browser'); action_view(); type_text('alias'); key(28)
        contains(shot('launcher-alias'), 'Return saves the alias')
        type_text('orbitfixture'); key(28); time.sleep(.3)
        msg('launcher-run', 'orbitfixture')
        wait(lambda: (base/'launcher-action').read_text().strip() == 'open', 'Alias action did not save the selected app')
        print('PASS: pointer typing focus, command arguments and alias editing', flush=True)

        # Capture both densities, appearances, and the scaled island host.
        for compact, mode, output, scale, placement, ui_scale in (
            (True, 'dark', 'TEST-1', 1, 'island', 1),
            (False, 'light', 'TEST-1', 1, 'island', 1),
            (False, 'dark', 'TEST-2', 1.5, 'island', 1.25),
            (True, 'dark', 'TEST-1', 1, 'floating', 1),
        ):
            configure(compact, placement, ui_scale); msg('theme-mode-set', mode)
            dispatch(f'hl.dsp.focus({{monitor="{output}"}})'); move(1100*scale, 650*scale)
            tag = ('compact' if compact else 'comfortable') + '-' + mode + '-' + placement
            open_query('orbit browser'); contains(shot('launcher-'+tag), 'Orbit Browser')
            action_view(); contains(shot('launcher-'+tag+'-actions'), 'New private window')
            key(1); key(1)
            assert shell.poll() is None
        print('PASS: Compact, Comfortable, light/dark, scaled Island and standalone hosting', flush=True)
    finally:
        msg('panel-close')
        for helper in helpers:
            helper.terminate(); helper.wait(timeout=5)
