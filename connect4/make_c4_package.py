#!/usr/bin/env python3
"""Finish a Connect Four WDL package written by c4pack.

usage: make_c4_package.py PKG_DIR SOLVE_LOG W H

Adds to PKG_DIR: README.md (from package-README.md), STATISTICS.csv (per-ply W/D/L for the side to
move, parsed from the solve log), c4_wdl_query.cpp, and SHA256SUMS of every file. Before it writes
SHA256SUMS, it re-reads every .zst from disk and checks its size and SHA-256 against MANIFEST.tsv,
which c4pack recorded from the bytes it wrote. It also prints the raw-table manifest
(sha256sum format) for checksums/c4_wdl_<W>x<H>.SHA256SUMS in the repository.
"""
import concurrent.futures, hashlib, os, re, shutil, sys

pkg, log, W, H = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
here = os.path.dirname(os.path.abspath(__file__))
cells = W * H


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 24), b''):
            h.update(block)
    return h.hexdigest()


# MANIFEST.tsv: name, raw size, raw sha256, zst name, zst size, zst sha256
man = [l.rstrip('\n').split('\t') for l in open(os.path.join(pkg, 'MANIFEST.tsv')) if l.strip()]
names = {m[0] for m in man}
want = {'zdd.bin'} | {'slab_%d.bin' % k for k in range(cells)}
if names != want:
    sys.exit('MANIFEST.tsv is incomplete: missing %s' % sorted(want - names)[:5])
with concurrent.futures.ThreadPoolExecutor(16) as ex:
    got = dict(zip([m[3] for m in man], ex.map(lambda m: sha256(os.path.join(pkg, m[3])), man)))
for m in man:
    p = os.path.join(pkg, m[3])
    if os.path.getsize(p) != int(m[4]) or got[m[3]] != m[5]:
        sys.exit('%s does not match MANIFEST.tsv' % m[3])
print('all %d .zst files match MANIFEST.tsv' % len(man))

# per-ply statistics from the solve log (last entry per ply wins)
plies = {}
for l in open(log):
    r = re.match(r'\[ply\s*(\d+)\] n=([\d,]+)\s+W/D/L\(stm\) = ([\d,]+) / ([\d,]+) / ([\d,]+)', l)
    if r:
        plies[int(r[1])] = [int(x.replace(',', '')) for x in r.groups()[1:]]
missing = [k for k in range(cells) if k not in plies]
if missing:
    sys.exit('solve log lacks plies %s' % missing)
raw = {m[0]: int(m[1]) for m in man}
for k in range(cells):
    n = plies[k][0]
    if n != sum(plies[k][1:]) or raw['slab_%d.bin' % k] != (n + 4) // 5:
        sys.exit('ply %d: counts do not match the slab' % k)
with open(os.path.join(pkg, 'STATISTICS.csv'), 'w') as f:
    f.write('# Connect Four %dx%d: stored positions per ply and their values for the side to move\n' % (W, H))
    f.write('# ply %d (full boards without a four, all draws) is not stored\n' % cells)
    f.write('ply,to_move,positions,win,draw,loss\n')
    for k in range(cells):
        n, w, d, lo = plies[k]
        f.write('%d,%s,%d,%d,%d,%d\n' % (k, 'first' if k % 2 == 0 else 'second', n, w, d, lo))
tot = [sum(plies[k][i] for k in range(cells)) for i in range(4)]

shutil.copy(os.path.join(here, 'c4_wdl_query.cpp'), pkg)
zsize = sum(int(m[4]) for m in man)
rsize = sum(int(m[1]) for m in man)
readme = open(os.path.join(here, 'package-README.md')).read()
for a, b in (('{W}', str(W)), ('{H}', str(H)), ('{CELLS}', str(cells)), ('{LAST}', str(cells - 1)), ('{N}', '{:,}'.format(tot[0])),
             ('{WIN}', '{:,}'.format(tot[1])), ('{DRAW}', '{:,}'.format(tot[2])), ('{LOSS}', '{:,}'.format(tot[3])),
             ('{ZSIZE}', '{:,}'.format(zsize)), ('{ZTB}', '%.2f' % (zsize / 1e12)),
             ('{RSIZE}', '{:,}'.format(rsize)), ('{RTB}', '%.1f' % (rsize / 1e12)),
             ('{RATIO}', '%.1f' % (rsize / zsize))):
    readme = readme.replace(a, b)
open(os.path.join(pkg, 'README.md'), 'w').write(readme)

files = sorted(f for f in os.listdir(pkg) if f != 'SHA256SUMS' and not f.endswith('.tmp'))
small = [f for f in files if not f.endswith('.zst')]
sums = dict(got)
sums.update({f: sha256(os.path.join(pkg, f)) for f in small})
with open(os.path.join(pkg, 'SHA256SUMS'), 'w') as o:
    for f in sorted(files, key=lambda x: (not x.startswith('slab_'), int(re.sub(r'\D', '', x) or 0), x)):
        o.write('%s  %s\n' % (sums[f], f))
print('package %s: %d files, %s bytes compressed' % (pkg, len(files), '{:,}'.format(zsize)))
print('--- raw manifest (checksums/c4_wdl_%dx%d.SHA256SUMS) ---' % (W, H))
rows = sorted(man, key=lambda m: (m[0] == 'zdd.bin', int(re.sub(r'\D', '', m[0]) or 0)))
if os.path.exists(os.path.join(pkg, 'PACK_BASE3')):
    rows.append(['PACK_BASE3', None, sums['PACK_BASE3']])
for m in rows:
    print('%s  %s' % (m[2], m[0]))
