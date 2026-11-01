#!/usr/bin/env python3
"""Unattended per-layer TwixT solve: one ply per step, adaptive worker count.

Each step resumes the layer store, runs exactly one forward or retrograde
ply, and exits.  Before a step the driver predicts the peak RSS for each
candidate worker count from layer sizes and a bytes-per-slot factor that it
recalibrates from every measured step; it runs the fastest count that fits
the budget.  A memory failure (solver RSS cap, std::bad_alloc, or the guard's
host-memory stop) retries the same ply with fewer workers.  Create STOP in
the store directory to stop after the current step.

usage: solve_driver.py --store DIR --binary BIN [--start CKP] [--budget-gib N]
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

# Worst-wave image volume as a multiple of the new layer, measured at 6x6
# ply 14->15 (tasks are in cell order; later cells rebuild more prefix);
# 32 and 48 extrapolate the 16->24 slope.
WAVE_VOLUME = {64: 27.54, 48: 20.06, 32: 12.58, 24: 8.84, 16: 5.10, 6: 4.13, 4: 3.07, 3: 2.31, 2: 1.53, 1: 0.77}
CANDIDATES = [64, 48, 32, 24, 16, 6, 4, 3, 2, 1]
# Retrograde peak per node of (forward layer + next result layer), relative,
# measured on 5x6 plies 13-16 with level preimages restricted to the active
# layer (single wave from 24 workers; 16: 76 B, 32: 100 B, 64: 110-130 B).
RETRO_VOLUME = {64: 1.25, 48: 1.15, 32: 1.05, 24: 0.9, 16: 0.8, 6: 0.6, 4: 0.5, 3: 0.45, 2: 0.4, 1: 0.35}


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def read_manifest(path):
    m = {'forward': {}, 'result': {}}
    with open(path) as f:
        if f.readline().strip() != 'TWXMAN01':
            raise SystemExit('not a manifest: ' + path)
        for line in f:
            line = line.strip()
            if line == 'end':
                break
            if line.startswith('forward ') or line.startswith('result '):
                kind, ply, name, size, checksum, nodes = line.split()
                m[kind][int(ply)] = {'nodes': int(nodes), 'bytes': int(size)}
            elif '=' in line:
                k, v = line.split('=', 1)
                m[k] = v
    for k in ('phase', 'cursor', 'layers'):
        m[k] = int(m[k])
    return m


class Driver:
    def __init__(self, a):
        self.a = a
        self.store = os.path.abspath(a.store)
        self.manifest = os.path.join(self.store, 'manifest.twxman')
        self.state_path = os.path.join(self.store, 'driver-state.json')
        self.log_path = os.path.join(self.store, 'driver.log')
        os.makedirs(self.store, exist_ok=True)
        self.state = {'forward_bps': a.initial_bps, 'retro_bps': a.initial_bps * 1.5, 'steps': []}
        if os.path.exists(self.state_path):
            with open(self.state_path) as f:
                self.state.update(json.load(f))
        # Explicit calibration, e.g. after a solver change that alters peaks.
        if a.forward_bps:
            self.state['forward_bps'] = a.forward_bps
        if a.retro_bps:
            self.state['retro_bps'] = a.retro_bps
        self.budget = a.budget_gib * (1 << 30)

    def log(self, msg):
        line = '%s %s' % (now(), msg)
        print(line, flush=True)
        with open(self.log_path, 'a') as f:
            f.write(line + '\n')

    def save_state(self):
        tmp = self.state_path + '.partial'
        with open(tmp, 'w') as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, self.state_path)

    # ---- memory models -------------------------------------------------
    def forward_slots(self, m, workers):
        k = m['layers'] - 1
        lk = m['forward'][k]['nodes']
        sizes = [m['forward'][j]['nodes'] for j in sorted(m['forward']) if j >= k - 2]
        ratios = [b / a for a, b in zip(sizes, sizes[1:]) if a > 0]
        # Layer growth alternates with the mover; the larger of the last two
        # ratios plus a margin covers the next one.
        growth = min(3.5, max([1.0] + ratios) * 1.1)
        nxt = growth * lk
        return lk + (1.4 + WAVE_VOLUME[workers]) * nxt, nxt

    def retro_slots(self, m, ply, workers):
        fk = m['forward'][ply]['nodes']
        sk1 = m['result'].get(ply + 1, {'nodes': 0})['nodes']
        # retro_bps is then about bytes per node (~100 on 5x6).
        return (fk + sk1) * RETRO_VOLUME[workers]

    OVERHEAD = 3 << 30  # process baseline: block tables, stacks, buffers

    def choose(self, slots_of, bps, cap=None):
        for w in CANDIDATES:
            if (cap is not None and w > cap) or (self.a.max_workers and w > self.a.max_workers):
                continue
            if self.OVERHEAD + slots_of(w) * bps <= self.budget:
                return w
        return 1

    # ---- one solver step ----------------------------------------------
    def run(self, args, label):
        log = os.path.join(self.store, 'step-%03d-%s.log' % (len(self.state['steps']), label))
        csv = log[:-4] + '.csv'
        cmd = [sys.executable, self.a.guard, '--csv', csv, '--interval', '5', '--as-gib', '0',
               '--min-avail-gib', str(self.a.min_avail_gib), '--', '/usr/bin/time', '-v',
               self.a.binary] + args
        self.log('RUN %s: %s' % (label, ' '.join(args)))
        started = time.time()
        env = dict(os.environ)
        env['TWIXT_PROFILE'] = log[:-4] + '.prof'  # per-step memory/time trace
        env['TWIXT_PROFILE_MALLINFO'] = '1'
        with open(log, 'w') as out:
            code = subprocess.call(cmd, stdout=out, stderr=subprocess.STDOUT, env=env)
        text = open(log, errors='replace').read()
        peak = 0
        mr = re.search(r'Maximum resident set size \(kbytes\): (\d+)', text)
        if mr:
            peak = int(mr.group(1)) * 1024
        return code, text, peak, time.time() - started, log

    def memory_failure(self, code, text):
        return ('aggregate parallel worker memory cap reached' in text or 'std::bad_alloc' in text or
                'KILLED: MemAvailable' in text or code == -9)

    def common(self, workers):
        # One wave per ply once workers cover every move (24 per player on 6x6).
        wave = ['--single-wave'] if self.a.single_wave and workers >= 24 else []
        return wave + ['--resume-dfa', self.a.start if self.a.start and not os.path.exists(self.manifest) else self.manifest,
                '--workers', str(workers), '--parallel-memory-gb', str(self.a.budget_gib),
                '--dfa-max-nodes', '0', '--census-seconds', str(self.a.step_seconds)] + \
               (['--level-threads', str(self.a.level_threads)] if self.a.level_threads else []) + \
               (['--sample-check', str(self.a.sample_check)] if self.a.sample_check else []) + \
               (['--layer-dir', self.store] if self.a.start and not os.path.exists(self.manifest) else [])

    @staticmethod
    def progress_key(m):
        """Monotone measure of solve progress: forward layers, then solved plies."""
        if m is None:
            return (-1, 0)
        if m['phase'] == 0:
            return (0, m['layers'])
        if m['phase'] == 1:
            return (1, m['layers'] - m['cursor'])
        return (2, m['layers'])

    def step(self):
        """Run one step; return True to continue, False to stop."""
        if os.path.exists(os.path.join(self.store, 'STOP')):
            self.log('STOP file present; exiting')
            return False
        m = read_manifest(self.manifest) if os.path.exists(self.manifest) else None
        if m is None and not self.a.start:
            raise SystemExit('no manifest and no --start checkpoint')
        cap = self.state.get('retry_cap')
        if m is None:
            # Convert the monolithic checkpoint and generate the next ply.
            kind, target, workers, pred = 'forward', self.a.first_ply, cap or self.a.first_workers, 0.0
            args = self.common(workers) + ['--symbolic-ply', str(target), '--retrograde-stop', str(target)]
        elif m['phase'] == 0:
            kind, target = 'forward', m['layers']
            bps = self.state['forward_bps']
            workers = self.choose(lambda w: self.forward_slots(m, w)[0], bps, cap)
            if self.a.forward_workers:  # measured single-wave peaks are far below the memo-era model
                workers = min(self.a.forward_workers, cap) if cap else self.a.forward_workers
            pred = self.OVERHEAD + self.forward_slots(m, workers)[0] * bps
            # If this ply turns out terminal, retrograde solves it and stops.
            args = self.common(workers) + ['--symbolic-ply', str(target), '--retrograde-stop', str(target)]
        elif m['phase'] == 1:
            kind, target = 'retro', m['cursor'] - 1
            bps = self.state['retro_bps']
            workers = self.choose(lambda w: self.retro_slots(m, target, w), bps, cap)
            pred = self.OVERHEAD + self.retro_slots(m, target, workers) * bps
            args = self.common(workers) + (['--retrograde-stop', str(target)] if target > 0 else [])
        else:
            self.log('solution complete (phase 2)')
            return False
        label = '%s-%d' % (kind, target)
        self.log('STEP %s with %d workers; predicted peak %.0f GiB (budget %d GiB)' %
                 (label, workers, pred / (1 << 30), self.a.budget_gib))
        code, text, peak, secs, log = self.run(args, label)
        after = read_manifest(self.manifest) if os.path.exists(self.manifest) else None
        advanced = self.progress_key(after) > self.progress_key(m)
        self.state['steps'].append({'label': label, 'workers': workers, 'exit': code,
                                    'peak_gib': round(peak / (1 << 30), 2), 'seconds': round(secs),
                                    'predicted_gib': round(pred / (1 << 30), 1), 'log': log,
                                    'advanced': advanced, 'utc': now()})
        if advanced:
            self.state['retry_cap'] = None
            if m is not None and peak > 0:  # recalibrate bytes per predicted slot
                if kind == 'forward' and target in after['forward']:
                    lk = m['forward'][m['layers'] - 1]['nodes']
                    slots = lk + (1.4 + WAVE_VOLUME[workers]) * after['forward'][target]['nodes']
                    key = 'forward_bps'
                else:
                    slots = self.retro_slots(m, target, workers)
                    key = 'retro_bps'
                # Only steps where the slot term dominates the fixed overhead
                # say anything about bytes per slot.
                if peak > 4 * self.OVERHEAD:
                    measured = (peak - self.OVERHEAD) / max(slots, 1.0)
                    # Follow increases at once; let estimates fall by at most 20% per step.
                    floor = self.a.initial_bps * 0.6 if key == 'forward_bps' else 20.0
                    self.state[key] = max(floor, max(measured, 0.8 * self.state[key]))
            self.log('DONE %s: exit %s, %.0f s, peak %.1f GiB; bytes/slot forward %.1f retro %.1f' %
                     (label, code, secs, peak / (1 << 30), self.state['forward_bps'], self.state['retro_bps']))
            self.save_state()
            result = re.search(r'Symbolic strong result: .*', text)
            if result:
                self.log('RESULT ' + result.group(0))
                return False
            return True
        if self.memory_failure(code, text):
            smaller = [w for w in CANDIDATES if w < workers]
            if not smaller:
                self.log('FAIL %s: memory failure even with 1 worker; stopping (see %s)' % (label, log))
                self.save_state()
                return False
            self.state['retry_cap'] = smaller[0]
            key = 'forward_bps' if kind == 'forward' else 'retro_bps'
            self.state[key] *= 1.3
            self.log('RETRY %s: memory failure with %d workers (exit %s, peak %.1f GiB); next cap %d workers' %
                     (label, workers, code, peak / (1 << 30), smaller[0]))
            self.save_state()
            return True
        self.log('FAIL %s: exit %s without progress; stopping (see %s)' % (label, code, log))
        self.save_state()
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--store', required=True)
    ap.add_argument('--binary', required=True)
    ap.add_argument('--guard', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'guard.py'))
    ap.add_argument('--start', help='monolithic checkpoint to convert on the first step')
    ap.add_argument('--first-ply', type=int, default=17, help='ply generated by the conversion step')
    ap.add_argument('--first-workers', type=int, default=16, help='workers for the conversion step')
    ap.add_argument('--budget-gib', type=int, default=600)
    ap.add_argument('--min-avail-gib', type=int, default=40)
    ap.add_argument('--initial-bps', type=float, default=55.0)
    ap.add_argument('--step-seconds', type=int, default=172800)
    ap.add_argument('--forward-bps', type=float, help='override the forward bytes/slot estimate')
    ap.add_argument('--max-workers', type=int, help='never run more workers than this')
    ap.add_argument('--forward-workers', type=int, help='fixed worker count for forward steps (retries still cap it)')
    ap.add_argument('--single-wave', action='store_true', help='pass --single-wave to steps with >= 24 workers')
    ap.add_argument('--sample-check', type=int, help='solver --sample-check N: check each solved ply against the rules')
    ap.add_argument('--level-threads', type=int, help='threads for level-by-level operations (solver option)')
    ap.add_argument('--retro-bps', type=float, help='override the retrograde bytes/slot estimate')
    a = ap.parse_args()
    d = Driver(a)
    pid_path = os.path.join(d.store, 'driver.pid')
    with open(pid_path, 'w') as f:
        f.write('%d\n' % os.getpid())
    d.log('driver start (pid %d): store %s binary %s budget %d GiB' % (os.getpid(), d.store, a.binary, a.budget_gib))
    try:
        while d.step():
            pass
    finally:
        d.log('driver exit')
        try:
            os.remove(pid_path)
        except OSError:
            pass


if __name__ == '__main__':
    main()
