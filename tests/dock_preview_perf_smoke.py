"""Cost and robustness of live dock previews with real applications.

Measures the shell and the compositor (CPU, amdgpu/i915/xe per-client GPU time,
memory, file descriptors and capture buffers) while the preview popup is closed,
open over static windows, open over 60 fps video and open over windows on another
workspace. Then checks that a source dying mid-capture and an output being
unplugged while its popup is open leave no buffers behind.

Results go to dock-preview-perf.json; measurements are reported, not asserted,
apart from leaks and crashes, because they depend on the machine.
"""
import json
import os
import pathlib
import re
import subprocess
import time

from dock_preview_smoke import prepare as prepare_preview

SETTLE = 4.0
WINDOW = float(os.environ.get('NOCTALIA_PREVIEW_PERF_SECONDS', '15'))
GPU_KEY = re.compile(r'^drm-engine-(gfx|render|rcs|ccs)\w*:\s+(\d+) ns', re.M)


def prepare(base, cfg, env):
    prepare_preview(base, cfg, env)


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    repo = pathlib.Path(__file__).resolve().parents[1]
    protocol = repo/'tests/fixtures/wlr-virtual-pointer-unstable-v1.xml'
    run(['wayland-scanner', 'client-header', str(protocol), str(base/'pointer-client.h')])
    run(['wayland-scanner', 'private-code', str(protocol), str(base/'pointer-code.c')])
    run(['cc', '-I'+str(base), str(repo/'tests/fixtures/island_pointer.c'), str(base/'pointer-code.c'),
         '-lwayland-client', '-o', str(base/'pointer')])
    pointer = subprocess.Popen([str(base/'pointer')], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    # The harness runs in this process and spawned the nested Hyprland directly.
    hyprland = int(next(p.name for p in pathlib.Path('/proc').iterdir() if p.name.isdigit()
                        and _comm(p) == 'Hyprland' and _ppid(p) == os.getpid()))
    tick = os.sysconf('SC_CLK_TCK')

    def command(value):
        pointer.stdin.write(value+'\n'); pointer.stdin.flush()
        assert pointer.stdout.readline().strip() == 'ok'

    def move(x, y, delay=.12):
        dispatch(f'hl.dsp.cursor.move({{x={int(x)},y={int(y)}}})')
        command('relative 1 0'); command('relative -1 0'); time.sleep(delay)

    def reveal(output=1):
        move(1100, 350 if output == 1 else 1050, .4)
        move(640, 719 if output == 1 else 1439, .35)
        move(640, 670 if output == 1 else 1390, 1.2)

    def hold_popup(seconds):
        # Small pointer motion inside the popup keeps hover alive like a reading user.
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            command('relative 1 0'); time.sleep(.5); command('relative -1 0'); time.sleep(.5)

    def dismiss():
        move(1100, 350, 1.0)

    def capture_buffers():
        found = set()
        for fd in pathlib.Path(f'/proc/{shell.pid}/fd').iterdir():
            try:
                if 'noctalia-toplevel-thumbnail' in str(fd.readlink()):
                    found.add(fd.stat().st_ino)
            except (FileNotFoundError, PermissionError):
                pass
        return found

    def gpu_ns(pid):
        # One DRM client per fd; sum unique clients so duplicated fds are not counted twice.
        clients = {}
        for info in pathlib.Path(f'/proc/{pid}/fdinfo').iterdir():
            try:
                text = info.read_text()
            except (FileNotFoundError, PermissionError):
                continue
            client = re.search(r'^drm-client-id:\s+(\d+)', text, re.M)
            if client:
                clients[client.group(1)] = sum(int(v) for _, v in GPU_KEY.findall(text))
        return sum(clients.values())

    def proc_sample(pid):
        p = pathlib.Path('/proc')/str(pid)
        stat = (p/'stat').read_text().rsplit(')', 1)[1].split()
        mem = {line.split(':')[0]: int(line.split()[1])
               for line in (p/'smaps_rollup').read_text().splitlines()[1:] if ':' in line}
        return {'cpu_s': (int(stat[11]) + int(stat[12])) / tick, 'gpu_ns': gpu_ns(pid),
                'rss_mib': round(mem['Rss']/1024, 1), 'pss_mib': round(mem['Pss']/1024, 1),
                'private_dirty_mib': round(mem['Private_Dirty']/1024, 1),
                'fds': len(list((p/'fd').iterdir()))}

    results = []

    def measure(label, seconds, during=None, **extra):
        time.sleep(SETTLE if during is None else 0)
        begin = {'shell': proc_sample(shell.pid), 'hyprland': proc_sample(hyprland), 't': time.monotonic()}
        (during or time.sleep)(seconds)
        end = {'shell': proc_sample(shell.pid), 'hyprland': proc_sample(hyprland), 't': time.monotonic()}
        elapsed = end['t'] - begin['t']
        row = {'label': label, 'seconds': round(elapsed, 1), 'capture_buffers': len(capture_buffers()), **extra}
        for name in ('shell', 'hyprland'):
            row[name] = {
                'cpu_pct': round(100 * (end[name]['cpu_s'] - begin[name]['cpu_s']) / elapsed, 1),
                'gpu_pct': round(100 * (end[name]['gpu_ns'] - begin[name]['gpu_ns']) / 1e9 / elapsed, 2),
                **{k: end[name][k] for k in ('rss_mib', 'pss_mib', 'private_dirty_mib', 'fds')},
            }
        results.append(row)
        print(json.dumps(row), flush=True)
        (out/'dock-preview-perf.json').write_text(json.dumps(results, indent=2))
        return row

    def clients():
        return [c for c in json.loads(ctl('-j', 'clients')) if c['class'] == 'dock-motion-test']

    def kitty(title):
        return start(['kitty', '--config', 'NONE', '--class', 'dock-motion-test', '--title', title,
                      '-o', 'confirm_os_window_close=0', 'sleep', '600'], title+'.log')

    def video(title):
        return start(['mpv', '--no-config', '--no-audio', '--loop=inf', '--vo=gpu', '--hwdec=no',
                      '--wayland-app-id=dock-motion-test', '--title='+title, '--force-window=immediate',
                      'av://lavfi:testsrc2=size=1280x720:rate=60'], title+'.log')

    def close_all():
        for client in clients():
            dispatch('hl.dsp.window.close({window="address:'+client['address']+'"})')
        wait(lambda: not clients(), 'Test windows did not close')

    def on_workspace(workspace, launcher, count):
        dispatch(f'hl.dsp.focus({{workspace="{workspace}"}})')
        procs = [launcher(f'{launcher.__name__}-{workspace}-{i}') for i in range(count)]
        expected = len(clients()) + count
        wait(lambda: len(clients()) == expected, f'{launcher.__name__} windows on {workspace}')
        return procs

    try:
        ctl('dismissnotify'); dispatch('hl.dsp.focus({monitor="TEST-1"})')
        dispatch('hl.dsp.focus({workspace="1"})'); dismiss()
        measure('baseline-no-windows', WINDOW)

        # Six static windows: the compositor may still produce frames for unchanged content.
        on_workspace('1', kitty, 6)
        dismiss()
        measure('static-6-popup-closed', WINDOW)
        reveal()
        static = measure('static-6-popup-open', WINDOW, during=hold_popup)
        assert static['capture_buffers'] == 6, static
        dismiss(); time.sleep(1)
        assert not capture_buffers(), 'Static previews retained buffers after closing'
        close_all()

        # Three 60 fps videos alongside three static windows.
        videos = on_workspace('1', video, 3)
        on_workspace('1', kitty, 3)
        dismiss()
        measure('video-3-static-3-popup-closed', WINDOW)
        reveal()
        live = measure('video-3-static-3-popup-open', WINDOW, during=hold_popup)
        assert live['capture_buffers'] == 6, live
        dismiss(); time.sleep(1)
        assert not capture_buffers(), 'Video previews retained buffers after closing'
        close_all()

        # Windows on another workspace (Hyprland's closest analogue of minimised windows).
        videos = on_workspace('2', video, 3)
        dispatch('hl.dsp.focus({workspace="1"})'); time.sleep(1)
        reveal()
        hidden = measure('video-3-off-workspace-popup-open', WINDOW, during=hold_popup)
        dismiss(); time.sleep(1)
        assert not capture_buffers(), 'Off-workspace previews retained buffers after closing'

        # A source dies while its stream is active.
        reveal(); time.sleep(1)
        before = len(capture_buffers())
        videos[0].kill(); videos[0].wait(timeout=5)
        wait(lambda: len(clients()) == 2, 'Killed video did not disappear')
        time.sleep(1.5)
        after_kill = len(capture_buffers())
        assert shell.poll() is None, 'Shell exited when a captured source died'
        assert after_kill < before, f'Dead source kept its capture buffer ({before} -> {after_kill})'
        dismiss(); time.sleep(1)
        assert not capture_buffers(), 'Buffers survived closing after a source died'
        close_all()

        # Unplug the output while its popup is open and streaming.
        dispatch('hl.dsp.focus({monitor="TEST-2"})')
        on_workspace('3', video, 2)
        reveal(2); time.sleep(1)
        streaming = len(capture_buffers())
        ctl('output', 'remove', 'TEST-2')
        wait(lambda: 'TEST-2' not in [m['name'] for m in json.loads(ctl('-j', 'monitors'))], 'Output removed')
        time.sleep(2)
        assert shell.poll() is None, 'Shell exited when the popup output was removed'
        after_unplug = len(capture_buffers())
        ctl('output', 'create', 'headless', 'TEST-2')
        wait(lambda: 'TEST-2' in [m['name'] for m in json.loads(ctl('-j', 'monitors'))], 'Output reconnected')
        time.sleep(1.5)
        dispatch('hl.dsp.focus({monitor="TEST-1"})'); dismiss(); time.sleep(1)
        close_all()
        settled = measure('settled-after-all', WINDOW)
        assert not capture_buffers(), 'Buffers survived the full scenario'

        summary = {
            'unplug': {'streaming_buffers': streaming, 'buffers_after_unplug': after_unplug},
            'source_death': {'before': before, 'after': after_kill},
            'off_workspace_buffers': hidden['capture_buffers'],
        }
        (out/'dock-preview-perf-summary.json').write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary), flush=True)
        base_row = results[0]
        assert settled['shell']['fds'] <= base_row['shell']['fds'] + 2, 'File descriptors leaked across the scenario'
        assert settled['shell']['private_dirty_mib'] <= static['shell']['private_dirty_mib'] + 16, \
            'Private memory grew after previews closed'
        print('PASS: dock preview cost measured; source death, off-workspace and unplug left no buffers', flush=True)
    finally:
        try:
            close_all()
        except Exception:
            pass
        if pointer.poll() is None:
            pointer.terminate(); pointer.wait(timeout=5)


def _comm(p):
    try:
        return (p/'comm').read_text().strip()
    except OSError:
        return ''


def _ppid(p):
    try:
        return int((p/'stat').read_text().rsplit(')', 1)[1].split()[1])
    except (OSError, ValueError, IndexError):
        return -1
