#!/usr/bin/env python3
"""Finish an Amazons WDL package written by `slabsolve reach --pkg std=DIR` or `--pkg tr=DIR`.

usage: make_wdl_package.py BOARD[,BOARD] REACH_ROOT PKG_DIR QUERY_BIN     e.g. 7x4 /data1/.../reach_4x7 /mnt2/amazons/amazons-7x4-wdl ./wdl_query

BOARD is the board the package answers, or both boards for a package written with --pkg-root
(its config.txt says so in `answers`, e.g. 5x6,6x5).  Adds to
PKG_DIR: wdl_query.cpp, README.md (package-README.md filled in), STATISTICS.md,
BOARD-wdl.csv and BOARD.json from the reach run's reach_progress.csv, and SHA256SUMS of
every file.  QUERY_BIN is a built wdl_query, used for the opening moves.
"""
import concurrent.futures, csv, hashlib, json, os, shutil, subprocess, sys

board, reach_root, pkg, query = sys.argv[1:5]
here = os.path.dirname(os.path.abspath(__file__))
cfg = dict(l.split(None, 1) for l in open(os.path.join(pkg, 'config.txt')) if l.strip() and not l.startswith('seeds'))
table = cfg['board'].strip()
boards = board.split(',')
if cfg.get('answers', '').strip().split(',') != boards:
    sys.exit('%s answers %s, not %s' % (pkg, cfg.get('answers', '?').strip(), board))
# reach_progress.csv: k,side,std_raw,std_closure,tr_raw,tr_closure,union,union_losses,std_wins,tr_wins
out, inn = {}, {}
for line in open(os.path.join(reach_root, 'reach_progress.csv')):
    f = line.strip().split(',')
    if len(f) == 10:
        (out if f[1] == 'out' else inn)[int(f[0])] = list(map(int, f[2:]))
plies = sorted(out)
kept = sorted(int(d[1:]) for d in os.listdir(pkg) if d.startswith('k') and d[1:].isdigit())
fmt = lambda x: '{:,}'.format(x) if x != '' else ''


def one_board(b):
    """Writes b-wdl.csv and b.json; returns the STATISTICS.md lines for b."""
    raw_i, win_i = (0, 6) if b == table else (2, 7)
    rows = []
    for k in plies:
        n = out[k][raw_i]
        r = {'ply': k, 'to_move': 'first' if k % 2 == 0 else 'second', 'reachable': n, 'win': '', 'loss': ''}
        if k in kept and k in inn:
            r['win'], r['loss'] = inn[k][win_i], n - inn[k][win_i]
        rows.append(r)
    with open(os.path.join(pkg, '%s-wdl.csv' % b), 'w', newline='') as f:
        wr = csv.DictWriter(f, fieldnames=['ply', 'to_move', 'reachable', 'win', 'loss'])
        wr.writeheader()
        wr.writerows(rows)
    lines = subprocess.run([query, pkg, b, '--moves'], capture_output=True, text=True, check=True).stdout.splitlines()
    root = 'first player wins' if lines[0].endswith('wins') else 'second player wins'
    wins = [l.split(':')[0] for l in lines[1:] if l.endswith('wins')]
    total = sum(r['reachable'] for r in rows)
    info = {'board': b, 'table_board': table, 'root': root, 'first_moves': len(lines) - 1,
            'winning_first_moves': wins, 'reachable_positions': total, 'plies_stored': kept}
    if len(boards) > 1:
        info['package_answers'] = boards
    json.dump(info, open(os.path.join(pkg, '%s.json' % b), 'w'), indent=1)
    md = ['* Root (standard start, first player to move): **%s**.' % root,
          '* Winning first moves: %d of %d%s.' % (len(wins), len(lines) - 1, (' (' + ', '.join(wins) + ')') if wins else ''),
          '* Reachable positions: **{:,}** in {} plies (0 to {} moves).'.format(total, len(rows), plies[-1]), '',
          '| ply | to move | reachable | win | loss |', '|---:|:---:|---:|---:|---:|']
    md += ['| %d | %s | %s | %s | %s |' % (r['ply'], r['to_move'], fmt(r['reachable']), fmt(r['win']), fmt(r['loss'])) for r in rows]
    md += ['', 'Machine-readable: `%s-wdl.csv`, `%s.json`.' % (b, b), '']
    return md


explain = ('Positions are counted without symmetry reduction: a position is the board and the side '
           'to move, and the side to move follows from the ply.  Wins and losses are for the side to '
           'move, given for the plies the package stores; the other plies are answered by one ply of search.')
title = ' and '.join(boards)
if len(boards) == 1:
    m = one_board(board)
    md = ['# Amazons %s statistics' % board, ''] + m[:3] + ['', explain, ''] + m[4:]
else:
    md = ['# Amazons %s statistics' % title, '',
          'This package answers %s.  The two boards are the same board turned a quarter turn, '
          'with different starting positions, so one table serves both.  Each section counts only '
          'the positions reachable from that board\'s own start; positions reachable from both '
          'starts are counted in both sections.' % ' and '.join(boards), '', explain, '']
    for b in boards:
        md += ['## %s' % b, ''] + one_board(b)
open(os.path.join(pkg, 'STATISTICS.md'), 'w').write('\n'.join(md))

shutil.copy(os.path.join(here, 'wdl_query.cpp'), pkg)
def phrase(b):
    w, h = b.split('x')
    return 'a %s-column by %s-row board' % (w, h)
if len(boards) == 1:
    note = '' if board == table else ' The files store the board transposed, as %s; `wdl_query` maps the moves.' % table
    combined = ''
    bp = phrase(board)
else:
    note = ''
    combined = ('\n\nOne package answers both %s: they are the same board turned, so the files store one '
                'table (as %s) holding every position reachable from either start.  Name the board on '
                'the command line; `wdl_query` sets up that board\'s start and maps the moves.  '
                'Per-board counts are in `STATISTICS.md` and the per-board CSV and JSON files.') % (' and '.join(boards), table)
    bp = ' and on '.join(phrase(b) for b in boards)
files = ', '.join('`%s-wdl.csv`, `%s.json`' % (b, b) for b in boards)
readme = open(os.path.join(here, 'package-README.md')).read()
for a, b in (('TITLE', title), ('BOARD_PHRASE', bp), ('TABLE_NOTE', note), ('COMBINED_NOTE', combined),
             ('QBOARD', boards[0]), ('FILES', files)):
    readme = readme.replace(a, b)
open(os.path.join(pkg, 'README.md'), 'w').write(readme)

def sha256(path):
    hh = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 24), b''):
            hh.update(block)
    return hh.hexdigest()

names = sorted(os.path.relpath(os.path.join(r, n), pkg) for r, _, fs in os.walk(pkg) for n in fs)
names = [n for n in names if n != 'SHA256SUMS']
with concurrent.futures.ThreadPoolExecutor(4) as ex:
    sums = list(ex.map(lambda n: sha256(os.path.join(pkg, n)), names))
with open(os.path.join(pkg, 'SHA256SUMS'), 'w') as o:
    o.writelines('%s  %s\n' % (s, n) for n, s in zip(names, sums))
print('package %s: %d files hashed' % (pkg, len(names)))
