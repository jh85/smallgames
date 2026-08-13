#!/usr/bin/env python3
"""Mini-Shobu: 2-board reduced variant of Shobu, rules faithful to the
Java engine (validated against the 104k dataset replays).

- 2 boards, each N x N. Board 0 = White's home, Board 1 = Black's home.
- Each board starts with k white stones on row 0 and k black stones on
  row N-1.
- Black moves first.
- A turn = passive move (own stone, own home board, no pushing) +
  aggressive move (own stone, other board, same heading, may push <=1
  enemy stone by the same heading vector; pushed stone leaving the board
  is captured).
- Headings: queen directions, distance 1 or 2.
- Win: any board with zero stones of a color -> other color wins.
- Player unable to move loses (stalemate = loss).

State: (wmask0, bmask0, wmask1, bmask1, turn) with turn 0=white,1=black.
Squares 0..N*N-1 row-major, row 0 = white side.
"""
import sys, random
from collections import deque

HEADINGS = [(dx, dy) for dx in (-2, -1, 0, 1, 2) for dy in (-2, -1, 0, 1, 2)
            if (dx, dy) != (0, 0) and not (abs(dx) == 2 and abs(dy) == 1)
            and not (abs(dx) == 1 and abs(dy) == 2)]
assert len(HEADINGS) == 16

class Mini:
    def __init__(self, n=3, k=3, h=None):
        self.n = n          # width
        self.h = h if h is not None else n  # height
        self.k = k
        self.sq = n * self.h

    def initial(self):
        n, k = self.n, self.k
        wm = sum(1 << i for i in range(k))               # row 0
        bm = sum(1 << (self.sq - n + i) for i in range(k))  # last row
        return (wm, bm, wm, bm, 1)  # black to move

    def _onboard(self, x, y):
        return 0 <= x < self.n and 0 <= y < self.h

    def _try_push_dest(self, wm, bm, e, h):
        """Pushed stone at square e moves by heading h. Returns
        ('off',) captured / ('ok', newsq) / ('blocked',)."""
        n = self.n
        ex, ey = e % n, e // n
        tx, ty = ex + h[0], ey + h[1]
        if not self._onboard(tx, ty):
            return ('off',)
        t = tx + ty * n
        if (wm >> t) & 1 or (bm >> t) & 1:
            return ('blocked',)
        return ('ok', t)

    def gen_moves(self, st):
        """Yields successor states. st=(wm0,bm0,wm1,bm1,turn)."""
        n = self.n
        wm0, bm0, wm1, bm1, turn = st
        if turn == 0:  # white: home board 0
            own_home, opp_home = wm0, bm0
            own_away, opp_away = wm1, bm1
            flip = 0
        else:
            own_home, opp_home = bm1, wm1
            own_away, opp_away = bm0, wm0
            flip = 1
        for psq in range(self.sq):
            if not (own_home >> psq) & 1: continue
            px, py = psq % n, psq // n
            for h in HEADINGS:
                dx, dy = h
                ux, uy = (dx > 0) - (dx < 0), (dy > 0) - (dy < 0)
                dist = max(abs(dx), abs(dy))
                tx, ty = px + dx, py + dy
                if not self._onboard(tx, ty): continue
                # passive path must be empty
                ok = True
                cx, cy = px + ux, py + uy
                for _ in range(dist):
                    csq = cx + cy * n
                    if (own_home >> csq) & 1 or (opp_home >> csq) & 1:
                        ok = False; break
                    cx += ux; cy += uy
                if not ok: continue
                pd = tx + ty * n
                # passive applied
                n_own_home = (own_home & ~(1 << psq)) | (1 << pd)
                # aggressive on the other board with same heading
                for asq in range(self.sq):
                    if not (own_away >> asq) & 1: continue
                    ax, ay = asq % n, asq // n
                    bx, by = ax + dx, ay + dy
                    if not self._onboard(bx, by): continue
                    # scan path
                    path = []
                    cx, cy = ax + ux, ay + uy
                    for _ in range(dist):
                        path.append(cx + cy * n)
                        cx += ux; cy += uy
                    stones = [s for s in path
                              if (own_away >> s) & 1 or (opp_away >> s) & 1]
                    if len(stones) > 1: continue
                    if stones and (own_away >> stones[0]) & 1: continue  # own
                    ad = bx + by * n
                    n_own_away = (own_away & ~(1 << asq)) | (1 << ad)
                    n_opp_away = opp_away
                    if stones:
                        r = self._try_push_dest(own_away, opp_away, stones[0], h)
                        if r[0] == 'blocked': continue
                        n_opp_away = opp_away & ~(1 << stones[0])
                        if r[0] == 'ok':
                            n_opp_away |= (1 << r[1])
                    if flip == 0:
                        yield (n_own_home, opp_home, n_own_away, n_opp_away, 1)
                    else:
                        yield (n_opp_away, n_own_away, opp_home, n_own_home, 0)

    def terminal_loser(self, st):
        """If state is terminal, return the color that LOST (0 white,1 black),
        else None. A board missing a color ends the game."""
        wm0, bm0, wm1, bm1, _ = st
        for wm, bm in ((wm0, bm0), (wm1, bm1)):
            if wm == 0: return 0
            if bm == 0: return 1
        return None

def perft(mini, maxdepth):
    st = mini.initial()
    seen = {st}
    frontier = [st]
    for d in range(maxdepth):
        nxt = {}
        for s in frontier:
            if mini.terminal_loser(s) is not None: continue
            for t in mini.gen_moves(s):
                if t not in seen:
                    nxt[t] = 1
        seen.update(nxt)
        frontier = list(nxt)
        print(f"depth {d+1}: new={len(frontier)} total={len(seen)}", flush=True)
    return len(seen)

def random_game(mini, seed):
    rnd = random.Random(seed)
    st = mini.initial()
    for ply in range(1000):
        if mini.terminal_loser(st) is not None:
            return 1 - st[4], ply  # previous mover won
        moves = list(mini.gen_moves(st))
        if not moves:
            return 1 - st[4], ply
        st = rnd.choice(moves)
    return None, 1000

if __name__ == '__main__':
    mini = Mini(3, 3)
    if sys.argv[1] == 'perft':
        perft(mini, int(sys.argv[2]))
    elif sys.argv[1] == 'rand':
        nw = nb = nd = 0; plies = []
        for i in range(2000):
            w, p = random_game(mini, i)
            plies.append(p)
            if w == 0: nw += 1
            elif w == 1: nb += 1
            else: nd += 1
        plies.sort()
        print(f"white wins {nw}, black wins {nb}, unfinished {nd}")
        print(f"plies: min {plies[0]} med {plies[1000]} max {plies[-1]}")
