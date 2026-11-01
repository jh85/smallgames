#!/usr/bin/env python3
"""Run one experiment with an address-space cap and a host-memory safety stop.

Samples cheap /proc counters (status + meminfo only; never smaps) and kills the
experiment if system MemAvailable falls below --min-avail-gib, so a review run
can never starve the production solver.

usage: guard.py --csv OUT.csv [--as-gib N] [--min-avail-gib N] [--interval S] -- CMD...
"""
import argparse
import datetime
import os
import resource
import signal
import subprocess
import sys
import time


def meminfo_kib(key):
    with open('/proc/meminfo') as f:
        for line in f:
            if line.startswith(key + ':'):
                return int(line.split()[1])
    return 0


def status(pgid):
    """Sum counters over every process in the experiment's process group."""
    total = {'Threads': 0, 'VmSize': 0, 'VmRSS': 0, 'VmHWM': 0}
    found = False
    for name in os.listdir('/proc'):
        if not name.isdigit():
            continue
        try:
            if os.getpgid(int(name)) != pgid:
                continue
            with open(f'/proc/{name}/status') as f:
                for line in f:
                    k, _, v = line.partition(':')
                    if k in total and v.split():
                        total[k] += int(v.split()[0])
            found = True
        except OSError:
            continue
    return total if found else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', required=True)
    ap.add_argument('--as-gib', type=float, default=160)
    ap.add_argument('--min-avail-gib', type=float, default=110)
    ap.add_argument('--interval', type=float, default=2.0)
    ap.add_argument('cmd', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == '--' else a.cmd
    limit = int(a.as_gib * (1 << 30))

    def preexec():
        if limit > 0:  # --as-gib 0: no address-space cap (sanitizers reserve TiBs)
            resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        # If the host ever runs out of memory, the kernel must pick this
        # experiment, never a production job.  Raising the score needs no
        # privilege and is inherited across fork/exec.
        try:
            with open('/proc/self/oom_score_adj', 'w') as f:
                f.write('1000')
        except OSError:
            pass
        os.setpgrp()

    started = time.time()
    child = subprocess.Popen(cmd, preexec_fn=preexec)
    peak = {'VmRSS': 0, 'VmSize': 0}
    killed = None
    with open(a.csv, 'w') as out:
        out.write('utc,t,threads,VmSize_KiB,VmRSS_KiB,VmHWM_KiB,MemAvailable_KiB\n')
        while True:
            ret = child.poll()
            if ret is not None:
                break
            st = status(child.pid)
            avail = meminfo_kib('MemAvailable')
            if st:
                for k in peak:
                    peak[k] = max(peak[k], int(st.get(k, 0) or 0))
                out.write('%s,%.1f,%s,%s,%s,%s,%d\n' % (
                    datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds'),
                    time.time() - started, st.get('Threads', ''), st.get('VmSize', ''),
                    st.get('VmRSS', ''), st.get('VmHWM', ''), avail))
                out.flush()
            if avail < a.min_avail_gib * (1 << 20):
                killed = 'MemAvailable %.1f GiB below safety floor' % (avail / (1 << 20))
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
                break
            time.sleep(a.interval)
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    wall = time.time() - started
    print('GUARD: exit=%s wall=%.1fs maxrss_GiB=%.2f sampled_peak_rss_GiB=%.2f '
          'sampled_peak_vsz_GiB=%.2f user=%.0fs sys=%.0fs%s' % (
              child.returncode, wall, ru.ru_maxrss / (1 << 20), peak['VmRSS'] / (1 << 20),
              peak['VmSize'] / (1 << 20), ru.ru_utime, ru.ru_stime,
              (' KILLED: ' + killed) if killed else ''), file=sys.stderr, flush=True)
    sys.exit(child.returncode if child.returncode is not None else 99)


if __name__ == '__main__':
    main()
