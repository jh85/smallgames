#!/usr/bin/env python3
"""Summarize a per-layer solve: manifest, driver steps, and the running step."""
import glob
import json
import os
import sys

store = sys.argv[1] if len(sys.argv) > 1 else 'tables/6x6-store'
manifest = os.path.join(store, 'manifest.twxman')
if os.path.exists(manifest):
    fields, fwd, res = {}, {}, {}
    for line in open(manifest):
        line = line.strip()
        if line.startswith('forward ') or line.startswith('result '):
            kind, ply, name, size, checksum, nodes = line.split()
            (fwd if kind == 'forward' else res)[int(ply)] = (int(nodes), int(size))
        elif '=' in line:
            k, v = line.split('=', 1)
            fields[k] = v
    phase = int(fields['phase'])
    print('phase %s (%s), layers %s, cursor %s' % (
        phase, {0: 'forward', 1: 'retrograde', 2: 'complete'}[phase], fields['layers'], fields['cursor']))
    print('ply  forward_nodes   result_nodes   disk_GB')
    for ply in sorted(set(fwd) | set(res)):
        f, r = fwd.get(ply, (0, 0)), res.get(ply, (0, 0))
        print('%3d %14s %14s %9.2f' % (ply, '{:,}'.format(f[0]) if f[0] else '-',
                                       '{:,}'.format(r[0]) if r[0] else '-', (f[1] + r[1]) / 1e9))
else:
    print('no manifest yet in', store)
state = os.path.join(store, 'driver-state.json')
if os.path.exists(state):
    s = json.load(open(state))
    print('\nbytes/slot forward %.1f retro %.1f' % (s['forward_bps'], s['retro_bps']))
    print('recent steps (label, workers, exit, peak GiB, predicted GiB, seconds):')
    for st in s['steps'][-8:]:
        print('  %-12s %2d workers  exit %-3s peak %6.1f  pred %6.1f  %7d s  %s' % (
            st['label'], st['workers'], st['exit'], st['peak_gib'], st['predicted_gib'], st['seconds'],
            'ok' if st['advanced'] else 'NO PROGRESS'))
logs = sorted(glob.glob(os.path.join(store, 'step-*.log')))
if logs:
    print('\ncurrent/last step log:', os.path.basename(logs[-1]))
    lines = open(logs[-1], errors='replace').read().splitlines()
    for line in lines[-4:]:
        print('  ' + line[:160])
    csvs = sorted(glob.glob(os.path.join(store, 'step-*.csv')))
    if csvs:
        rows = open(csvs[-1]).read().splitlines()[1:]
        if rows:
            last = rows[-1].split(',')
            peak = max(int(r.split(',')[4] or 0) for r in rows)
            print('  elapsed %.0f s, RSS %.1f GiB (peak %.1f), host MemAvailable %.0f GiB' % (
                float(last[1]), int(last[4]) / 2**20, peak / 2**20, int(last[6]) / 2**20))
