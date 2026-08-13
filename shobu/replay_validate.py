#!/usr/bin/env python3
"""Replay dataset games with a from-scratch reimplementation of the Java
Shobu engine rules to validate our understanding of the rules.

Mirrors: Move.isValid, Utilities.getStonesIntersected, Board.pushStones /
moveStone, GameRules.validateTurn / getWinner.
"""
import json, sys, os, random

W, B = 'o', 'x'

def quadrant(x, y):
    if x < 0 or y < 0 or x > 7 or y > 7: return -1
    return (0 if y < 4 else 2) + (0 if x < 4 else 1)

def heading_valid(hx, hy):
    if (hx, hy) == (0, 0): return False
    if abs(hx) > 2 or abs(hy) > 2: return False
    if abs(hx) == 2 and abs(hy) == 1: return False  # knight
    if abs(hx) == 1 and abs(hy) == 2: return False
    return True

def clamp(v): return max(-1, min(1, v))

class Board:
    def __init__(self, s=None):
        self.g = list(s) if s is not None else ['.'] * 64
    def get(self, x, y):
        if x < 0 or y < 0 or x > 7 or y > 7: return None  # None = off board here
        return self.g[x + y * 8]
    def set(self, x, y, v):
        if x < 0 or y < 0 or x > 7 or y > 7: return False
        self.g[x + y * 8] = v
        return True
    def ser(self): return ''.join(self.g)

def stones_intersected(b, ox, oy, hx, hy):
    """Stones on path origin+step .. origin+heading (within origin's quadrant)."""
    ux, uy = clamp(hx), clamp(hy)
    out = []
    cx, cy = ox + ux, oy + uy
    q0 = quadrant(ox, oy)
    while True:
        if quadrant(cx, cy) != q0: return out
        s = b.get(cx, cy)
        if s is not None and s != '.':
            out.append((cx, cy, s))
        if (cx, cy) == (ox + hx, oy + hy): break
        cx, cy = cx + ux, cy + uy
    return out

def move_stone(b, fx, fy, tx, ty):
    s = b.get(fx, fy)
    if s is None or s == '.': return
    # crossing quadrant boundary (the middle lines) deletes the stone
    if (fx > 3 and tx <= 3) or (fx <= 3 and tx > 3):
        b.set(fx, fy, '.'); return
    if (fy > 3 and ty <= 3) or (fy <= 3 and ty > 3):
        b.set(fx, fy, '.'); return
    b.set(fx, fy, '.')
    b.set(tx, ty, s)  # off-board set fails -> stone deleted (capture)

def push_stones(b, ox, oy, hx, hy, seen):
    """Returns count of stones moved including self."""
    for (sx, sy, s) in stones_intersected(b, ox, oy, hx, hy):
        if (sx, sy) not in seen:
            seen.add((sx, sy))
            push_stones(b, sx, sy, hx, hy, seen)
    move_stone(b, ox, oy, ox + hx, oy + hy)
    return len(seen) + 1

def validate(b, turn_color, p, a):
    """p, a = (ox, oy, hx, hy). Returns list of errors (empty = legal)."""
    err = []
    pox, poy, phx, phy = p
    aox, aoy, ahx, ahy = a
    if not heading_valid(phx, phy): err.append('passive heading invalid')
    if not heading_valid(ahx, ahy): err.append('aggressive heading invalid')
    if err: return err
    if not (0 <= pox <= 7 and 0 <= poy <= 7): err.append('passive origin oob')
    if not (0 <= aox <= 7 and 0 <= aoy <= 7): err.append('aggressive origin oob')
    if (phx, phy) != (ahx, ahy): err.append('headings differ')
    own = W if turn_color == 'WHITE' else B
    if b.get(pox, poy) != own or b.get(aox, aoy) != own:
        err.append('not own stone')
    pq, aq = quadrant(pox, poy), quadrant(aox, aoy)
    if turn_color == 'WHITE' and pq not in (0, 1): err.append('passive not home')
    if turn_color == 'BLACK' and pq not in (2, 3): err.append('passive not home')
    if pq in (0, 2) and aq not in (1, 3): err.append('aggr not opposite color board')
    if pq in (1, 3) and aq not in (0, 2): err.append('aggr not opposite color board')
    ip = stones_intersected(b, pox, poy, phx, phy)
    if len(ip) != 0: err.append('passive pushes')
    ia = stones_intersected(b, aox, aoy, ahx, ahy)
    if len(ia) >= 2: err.append('aggr pushes 2+')
    bb = Board(b.ser())
    np_ = push_stones(bb, pox, poy, phx, phy, set())
    na = push_stones(bb, aox, aoy, ahx, ahy, set())
    if np_ > 2: err.append('passive pushes through >1')
    if na > 2: err.append('aggr pushes through >1')
    for (_, _, s) in ia:
        if s == own: err.append('pushes own color')
    if quadrant(pox + phx, poy + phy) != pq: err.append('passive leaves quadrant')
    if quadrant(aox + ahx, aoy + ahy) != aq: err.append('aggr leaves quadrant')
    return err

def winner(b):
    for q in range(4):
        hasW = hasB = False
        for y in range(8):
            for x in range(8):
                if quadrant(x, y) != q: continue
                c = b.get(x, y)
                if c == W: hasW = True
                if c == B: hasB = True
        if not hasW: return 'BLACK'
        if not hasB: return 'WHITE'
    return None

def replay(path):
    with open(path) as f:
        g = json.load(f)
    states = g['game_states']
    b = Board(states[0]['board'])
    turn = states[0]['turn']
    assert turn == 'BLACK'
    for i, t in enumerate(g['turns']):
        cur = states[i]
        if b.ser() != cur['board']:
            return f"turn {i}: board mismatch\n got {b.ser()}\n exp {cur['board']}"
        if cur['turn'] != turn:
            return f"turn {i}: turn mismatch"
        p = t['passive']; a = t['aggressive']
        pm = (p['origin']['x'], p['origin']['y'], p['heading']['x'], p['heading']['y'])
        am = (a['origin']['x'], a['origin']['y'], a['heading']['x'], a['heading']['y'])
        errs = validate(b, turn, pm, am)
        if errs:
            return f"turn {i}: validation errors {errs}"
        push_stones(b, pm[0], pm[1], pm[2], pm[3], set())
        push_stones(b, am[0], am[1], am[2], am[3], set())
        turn = 'WHITE' if turn == 'BLACK' else 'BLACK'
    w = winner(b)
    if w != g['winner']:
        return f"winner mismatch: computed {w} file says {g['winner']}"
    return None

def main():
    root = 'Shobu_Games_104k_Dataset'
    files = []
    for d in ('black', 'white'):
        for fn in os.listdir(os.path.join(root, d)):
            files.append(os.path.join(root, d, fn))
    random.seed(1)
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 500
    if n < len(files):
        files = random.sample(files, n)
    bad = 0
    for i, fp in enumerate(files):
        r = replay(fp)
        if r:
            bad += 1
            print(f"FAIL {fp}: {r}")
            if bad > 5: break
        if (i + 1) % 5000 == 0:
            print(f"{i+1}/{len(files)} ok so far (bad={bad})", flush=True)
    print(f"done: {len(files)} games, {bad} failures")

if __name__ == '__main__':
    main()
