#!/usr/bin/env python3
"""Read-only process RAM/CPU/DRM profiling. CPU 100% means one logical core.

NVIDIA pmon's unavailable counters are kept as null, never interpreted as zero.
DRM engine counters are per client and deduplicated across duplicated file descriptors.
"""
import argparse
import json
import math
import os
import pathlib
import re
import statistics
import subprocess
import time


def snapshot(pid):
    root = pathlib.Path('/proc') / str(pid)
    stat = (root/'stat').read_text().rsplit(')', 1)[1].split()
    memory = {line.split(':')[0]: int(line.split()[1])
              for line in (root/'smaps_rollup').read_text().splitlines()[1:] if ':' in line}
    clients = {}
    for fd in (root/'fdinfo').iterdir():
        try:
            info = fd.read_text()
        except (OSError, PermissionError):
            continue
        client = re.search(r'^drm-client-id:\s*(\S+)', info, re.M)
        device = re.search(r'^drm-pdev:\s*(\S+)', info, re.M)
        if client:
            key = (device.group(1) if device else '', client.group(1))
            clients[key] = {k: int(v) for k, v in re.findall(r'^drm-engine-([\w-]+):\s*(\d+) ns', info, re.M)}
    engines = {}
    for values in clients.values():
        for key, value in values.items():
            engines[key] = engines.get(key, 0) + value
    return {'t': time.monotonic(), 'cpu_s': (int(stat[11])+int(stat[12]))/os.sysconf('SC_CLK_TCK'),
            'rss_mib': memory['Rss']/1024, 'pss_mib': memory['Pss']/1024,
            'private_mib': (memory['Private_Clean']+memory['Private_Dirty'])/1024,
            'swap_mib': memory['Swap']/1024, 'gpu_ns': engines,
            'fds': len(list((root/'fd').iterdir()))}


def summarize(samples):
    first, last = samples[0], samples[-1]
    elapsed = last['t']-first['t']
    result = {'seconds': round(elapsed, 2), 'cpu_percent_one_core': round(100*(last['cpu_s']-first['cpu_s'])/elapsed, 3),
              'gpu_engine_busy_percent': {key: round(100*(last['gpu_ns'][key]-value)/1e9/elapsed, 3)
                                          for key, value in first['gpu_ns'].items() if key in last['gpu_ns']} or None,
              'fds_start': first['fds'], 'fds_end': last['fds']}
    for key in ('rss_mib', 'pss_mib', 'private_mib', 'swap_mib'):
        result[key] = {'mean': round(statistics.mean(s[key] for s in samples), 2),
                       'peak': round(max(s[key] for s in samples), 2), 'end': round(last[key], 2)}
    return result


def nvidia_samples(text, pid):
    rows = []
    names = []
    for line in text.splitlines():
        if line.startswith('# gpu'):
            names = line[1:].split()
        elif names and not line.startswith('#'):
            fields = line.split()
            if len(fields) >= len(names) and fields[names.index('pid')] == str(pid):
                rows.append(dict(zip(names, fields)))
    result = {}
    for column, key in (('sm', 'sm_percent'), ('fb', 'framebuffer_mib')):
        values = [float(row[column]) for row in rows if row.get(column, '-').replace('.', '', 1).isdigit()]
        result[key] = {'mean': round(statistics.mean(values), 2), 'peak': max(values), 'samples': len(values)} if values else None
    return result


def measure(processes, seconds=20, nvidia=False):
    monitor = subprocess.Popen(['nvidia-smi', 'pmon', '-s', 'um', '-d', '1', '-c', str(math.ceil(seconds)+1)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) if nvidia else None
    readings = {name: [] for name in processes}
    end = time.monotonic()+seconds
    while True:
        for name, pid in processes.items():
            readings[name].append(snapshot(pid))
        if time.monotonic() >= end:
            break
        time.sleep(min(.5, max(0, end-time.monotonic())))
    result = {name: summarize(samples) for name, samples in readings.items()}
    if monitor:
        output, error = monitor.communicate(timeout=5)
        for name, pid in processes.items():
            result[name]['nvidia'] = nvidia_samples(output, pid)
        if monitor.returncode:
            result['nvidia_error'] = error.strip()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--process', action='append', required=True, help='label=pid')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--nvidia', action='store_true')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    assert args.seconds >= 1
    processes = {name: int(pid) for name, pid in (item.split('=', 1) for item in args.process)}
    result = measure(processes, args.seconds, args.nvidia)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
