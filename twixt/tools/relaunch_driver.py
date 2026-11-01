#!/usr/bin/env python3
"""Restart the solve driver between steps with a recalibrated memory model.

Waits for the running driver (driver.pid) to exit after a STOP request,
derives forward bytes/slot from the last forward step's measured wave peak
(the solver's "Parallel forward ply" line) plus a margin, removes STOP, and
starts the driver again with --forward-bps and the given extra arguments.

usage: relaunch_driver.py --store DIR [--margin 1.2] -- DRIVER_ARGS...
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

WAVE_VOLUME = {64: 27.54, 48: 20.06, 32: 12.58, 24: 8.84, 16: 5.10, 6: 4.13, 4: 3.07, 3: 2.31, 2: 1.53, 1: 0.77}
OVERHEAD = 3 << 30


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--store', required=True)
    ap.add_argument('--margin', type=float, default=1.2)
    ap.add_argument('--fixed-bps', type=float, help='use this forward bytes/slot instead of deriving it')
    ap.add_argument('--swap', help='converted-layer directory to swap in (tools/swap_layers.py) before restarting')
    ap.add_argument('--log', required=True)
    ap.add_argument('rest', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    rest = a.rest[1:] if a.rest and a.rest[0] == '--' else a.rest
    store = os.path.abspath(a.store)
    pid = int(open(os.path.join(store, 'driver.pid')).read())
    while True:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            break
        time.sleep(10)
    log = sorted(glob.glob(os.path.join(store, 'step-*-forward-*.log')))[-1]
    text = open(log, errors='replace').read()
    peaks = [float(x) for x in re.findall(r'Parallel forward ply \d+: .*?peak_RSS_GiB=([0-9.]+)', text)]
    ply = int(re.search(r'forward-(\d+)\.log$', log).group(1))
    nodes = {}
    for line in open(os.path.join(store, 'manifest.twxman')):
        f = line.split()
        if len(f) == 6 and f[0] == 'forward':
            nodes[int(f[1])] = int(f[5])
    extra = []
    if a.fixed_bps:
        extra = ['--forward-bps', '%.1f' % a.fixed_bps]
    elif peaks and ply in nodes and ply - 1 in nodes:
        workers = int(re.search(r'--workers (\d+)', open(os.path.join(store, 'driver.log')).read().split('RUN forward-%d' % ply)[-1]).group(1))
        if workers in WAVE_VOLUME:
            slots = nodes[ply - 1] + (1.4 + WAVE_VOLUME[workers]) * nodes[ply]
            bps = (peaks[-1] * (1 << 30) - OVERHEAD) / slots * a.margin
            extra = ['--forward-bps', '%.1f' % bps]
    with open(a.log, 'a') as out:
        out.write('%s relaunch: last forward step %s, wave peaks %s GiB, extra %s\n' % (
            time.strftime('%Y-%m-%dT%H:%M:%S'), os.path.basename(log), peaks, extra))
    if a.swap and os.path.exists(os.path.join(a.swap, 'LAYERS.txt')):
        swap = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'swap_layers.py'),
                               store, a.swap], capture_output=True, text=True)
        with open(a.log, 'a') as out:
            out.write('%s relaunch: swap exit %d: %s%s\n' % (time.strftime('%Y-%m-%dT%H:%M:%S'), swap.returncode,
                                                           swap.stdout.strip(), swap.stderr.strip()))
    stop = os.path.join(store, 'STOP')
    if os.path.exists(stop):
        os.remove(stop)
    out = open(a.log, 'a')
    subprocess.Popen([sys.executable] + rest + extra, stdout=out, stderr=subprocess.STDOUT,
                     stdin=subprocess.DEVNULL, start_new_session=True)


if __name__ == '__main__':
    main()
