#!/usr/bin/env python3
"""Normalize full-solve layer profiles by the layer size at a given game fraction."""
import math, re, sys

def parse(paths):
    fwd, retro, mode = {}, {}, 'f'
    for path in paths:
        for line in open(path):
            if line.startswith('Symbolic forward'): mode = 'f'
            elif line.startswith('Symbolic retrograde'): mode = 'r'
            parts = line.strip().split(',')
            if not parts[0].isdigit() or len(parts) not in (6, 7): continue
            try:
                v = [int(x) for x in parts[:-1]]
            except ValueError:
                continue
            if mode == 'f' and len(parts) == 6: fwd[v[0]] = dict(pos=v[1], L=v[2], active=v[3], pool=v[4])
            elif mode == 'r' and len(parts) == 7: retro[v[0]] = dict(W=v[1], D=v[2], Lo=v[3], WLD=v[4], pool=v[5])
    return fwd, retro

def smooth_at(fwd, plies, frac):
    # log-size averaged over plies k-1..k+1 (removes odd/even alternation), interpolated at frac
    xs = sorted(fwd)
    sm = {}
    for k in xs:
        ks = [j for j in (k - 1, k, k + 1) if j in fwd and fwd[j]['L'] > 0]
        sm[k] = sum(math.log(fwd[j]['L']) for j in ks) / len(ks)
    x = frac * plies
    lo = max(k for k in xs if k <= x); hi = min(k for k in xs if k >= x)
    if lo == hi: return math.exp(sm[lo])
    t = (x - lo) / (hi - lo)
    return math.exp(sm[lo] * (1 - t) + sm[hi] * t)

boards = [(b, int(c)) for b, c in [a.split(':') for a in sys.argv[2:]]]
frac = float(sys.argv[1])
print(f"normalized by smoothed |L| at game fraction {frac:.3f}")
print("board cells  ref|L|   peak|L|@ply  sum|L|  R_end  peakWLD  archive  perlayer_live_peak")
for spec, cells in boards:
    name, paths = spec.split('=')
    fwd, retro = parse(paths.split(','))
    ref = smooth_at(fwd, cells, frac)
    peak_k = max(fwd, key=lambda k: fwd[k]['L'])
    sumL = sum(f['L'] for f in fwd.values())
    R_end = fwd[max(fwd)]['pool']
    if retro:
        peakWLD = max(r['WLD'] for r in retro.values())
        archive = retro[min(retro)]['pool']
        live = []
        for k in fwd:
            if k in retro:
                nxt = retro.get(k + 1, {}).get('WLD', 0)
                live.append(fwd[k]['L'] + retro[k]['WLD'] + nxt)
        livepk = max(live) if live else float('nan')
    else:
        peakWLD = archive = livepk = float('nan')
    print(f"{name:5s} {cells:5d} {ref/1e6:8.3f}M  {fwd[peak_k]['L']/ref:6.2f}@{peak_k:<3d} {sumL/ref:7.2f} {R_end/ref:6.2f} {peakWLD/ref:7.2f} {archive/ref:8.2f} {livepk/ref:8.2f}")
