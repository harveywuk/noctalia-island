"""Repeatable idle and UI memory measurements in the private compositor fixture."""
import json
import os
import pathlib
import time


def run_checks(base, cfg, out, env, run, ctl, dispatch, msg, wait, start, shell):
    p = pathlib.Path('/proc') / str(shell.pid)
    def sample(label):
        stat=(p/'stat').read_text().rsplit(')',1)[1].split()
        status=dict(line.split(':',1) for line in (p/'status').read_text().splitlines() if ':' in line)
        mem={line.split(':')[0]:int(line.split()[1]) for line in (p/'smaps_rollup').read_text().splitlines()[1:] if ':' in line}
        return {'label':label,'time':time.monotonic(),'cpu_ticks':int(stat[11])+int(stat[12]),
                'main_switches':int(status['voluntary_ctxt_switches']),
                'memory_mib':{k:round(mem[k]/1024,2) for k in ('Rss','Pss','Private_Dirty','Swap')},
                'fds':len(list((p/'fd').iterdir())),'threads':int(status['Threads'])}
    results=[]
    def record(label):
        s=sample(label);results.append(s);print(json.dumps(s),flush=True)
        (out/'performance.json').write_text(json.dumps(results,indent=2))
    (out/'inspect-env.json').write_text(json.dumps(env))
    (out/'shell.pid').write_text(str(shell.pid))
    dispatch('hl.dsp.focus({monitor="TEST-1"})');dispatch('hl.dsp.cursor.move({x=1100,y=600})')
    time.sleep(5);record('idle-start');time.sleep(20);record('idle-end')
    for batch in range(3):
        for section in ['appearance','keybinds','input-motion','displays','workspaces','session','system','plugins']:
            msg('settings-open',section);time.sleep(.4);msg('settings-close');time.sleep(.2)
        for _ in range(5):
            for panel in ['launcher','control-center','clipboard']:
                msg('panel-toggle',panel);time.sleep(.4);msg('panel-close');time.sleep(.2)
            dispatch('hl.dsp.cursor.move({x=640,y=30})');time.sleep(.5)
            dispatch('hl.dsp.cursor.move({x=1100,y=600})');time.sleep(.5)
        time.sleep(5);record(f'closed-after-batch-{batch+1}')
    time.sleep(20);record('settled')
    msg('settings-open','appearance');time.sleep(2);record('settings-open')
    run(['grim','-o','TEST-1',str(out/'performance-settings.png')])
    msg('settings-close');time.sleep(5);record('settings-closed')
    if os.environ.get('NOCTALIA_TEST_PERFORMANCE_INSPECT'):
        print('INSPECT: performance session ready',flush=True)
        deadline=time.monotonic()+int(os.environ['NOCTALIA_TEST_PERFORMANCE_INSPECT'])
        while time.monotonic()<deadline and not (out/'inspect-done').exists():time.sleep(.25)
    assert shell.poll() is None
    assert all(s['fds'] == results[0]['fds'] for s in results), 'UI cycles leaked file descriptors'
    # Allow cache warmup on the first batch; compare subsequent identical work.
    first = next(s for s in results if s['label']=='closed-after-batch-1')
    last = next(s for s in results if s['label']=='closed-after-batch-3')
    assert last['memory_mib']['Private_Dirty'] <= first['memory_mib']['Private_Dirty'] + 8, 'Repeated UI cycles retained increasing private memory'
    print('PASS: UI lifecycle benchmark completed',flush=True)
