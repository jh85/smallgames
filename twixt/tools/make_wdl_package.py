#!/usr/bin/env python3
"""Assemble a WDL package from a completed layer store.

usage: make_wdl_package.py BOARD STORE OUT_DIR      e.g. 6x6 tables/6x6-store tables/packages

Creates OUT_DIR/twixt-BOARD-wdl/ with the store's result layers (hard links),
a results-only manifest, layout.txt (twixt --export-layout), wdl_query.cpp,
README.md (wdl/package-README-BOARD.md), STATISTICS.md and the results/ CSV
and JSON, and SHA256SUMS; then OUT_DIR/twixt-BOARD-wdl.tar.zst and its .sha256.
"""
import csv, glob, hashlib, json, os, shutil, subprocess, sys

board, store, out = sys.argv[1], sys.argv[2], sys.argv[3]
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.dirname(here)                       # twixt/
pkg = os.path.join(out, 'twixt-%s-wdl' % board)
os.makedirs(pkg, exist_ok=True)
for f in glob.glob(os.path.join(store, 'result-*.twxlay')):
    target = os.path.join(pkg, os.path.basename(f))
    if not os.path.exists(target):
        os.link(f, target)
with open(os.path.join(store, 'manifest.twxman')) as m, open(os.path.join(pkg, 'manifest.twxman'), 'w') as o:
    o.writelines(l for l in m if not l.startswith('forward '))
with open(os.path.join(pkg, 'layout.txt'), 'w') as o:
    subprocess.run([os.path.join(src, 'twixt'), '--resume-dfa', os.path.join(store, 'manifest.twxman'),
                    '--export-layout'], stdout=o, check=True)
shutil.copy(os.path.join(src, 'wdl', 'wdl_query.cpp'), pkg)
shutil.copy(os.path.join(src, 'wdl', 'package-README-%s.md' % board), os.path.join(pkg, 'README.md'))
for f in ('%s-wdl.csv' % board, '%s.json' % board):
    shutil.copy(os.path.join(src, 'results', f), pkg)

rows = list(csv.DictReader(open(os.path.join(src, 'results', '%s-wdl.csv' % board))))
info = json.load(open(os.path.join(src, 'results', '%s.json' % board)))
tot = {k: sum(int(r[k]) for r in rows) for k in ('reachable_positions', 'win', 'draw', 'loss')}
peak = max(rows, key=lambda r: int(r['reachable_positions']))
pct = lambda k: 100.0 * tot[k] / tot['reachable_positions']
lines = ['# TwixT %s statistics' % board, '',
         '* Root (empty board, White to move): **%s** for White; %s.' % (info['root_side_to_move_result'], info['root_absolute_result']),
         '* Opening moves: win %s; draw %s; loss %s.' % tuple(', '.join(info['first_moves'][k]) or 'none' for k in ('win', 'draw', 'loss')),
         '* Reachable positions: **{:,}** in {} plies; largest ply {} with {:,}.'.format(
             tot['reachable_positions'], len(rows), peak['ply'], int(peak['reachable_positions'])),
         '* Over all positions, for the side to move: win {:,} ({:.1f}%), draw {:,} ({:.1f}%), loss {:,} ({:.1f}%).'.format(
             tot['win'], pct('win'), tot['draw'], pct('draw'), tot['loss'], pct('loss')),
         '', '| ply | to move | positions | win | draw | loss |', '|---:|:---:|---:|---:|---:|---:|']
lines += ['| {} | {} | {:,} | {:,} | {:,} | {:,} |'.format(r['ply'], r['side_to_move'], int(r['reachable_positions']),
          int(r['win']), int(r['draw']), int(r['loss'])) for r in rows]
lines += ['', 'Machine-readable: `%s-wdl.csv`, `%s.json`.' % (board, board), '']
open(os.path.join(pkg, 'STATISTICS.md'), 'w').write('\n'.join(lines))

def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 24), b''):
            h.update(block)
    return h.hexdigest()
names = sorted((n for n in os.listdir(pkg) if n not in ('SHA256SUMS', 'wdl_query')),
               key=lambda n: (not n.startswith('result-'), int(n[7:-7]) if n.startswith('result-') else 0, n))
with open(os.path.join(pkg, 'SHA256SUMS'), 'w') as o:
    for n in names:
        o.write('%s  %s\n' % (sha256(os.path.join(pkg, n)), n))
archive = pkg + '.tar.zst'
subprocess.run(['tar', '--sort=name', '-I', 'zstd -T0 -10', '-cf', archive + '.partial', '-C', out,
                os.path.basename(pkg)], check=True)
os.replace(archive + '.partial', archive)
with open(archive + '.sha256', 'w') as o:
    o.write('%s  %s\n' % (sha256(archive), os.path.basename(archive)))
print(open(archive + '.sha256').read(), end='')
