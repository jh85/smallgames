#!/usr/bin/env python3
"""Query an Oware WDL table in .gmz format (gM-only, zstd block compressed).

    python3 oware_probe.py <table dir> s0 s1 ... s(2*ROW-1) captured0 captured1 side_to_move

Pit numbering follows OpenSpiel: player 0 owns pits 0..ROW-1, player 1 owns
ROW..2*ROW-1 (ROW = 6 for standard Oware, 7 for the 7x2 tables); sowing goes
from pit i to pit i+1.  The total of all pit counts plus both captured scores
must be the table's seed count (48, 36 or 28), which is read from the files.

Uses the `zstandard` package (pip install zstandard) when installed, otherwise
the `zstd` command.  Pure Python, one zstd block (64 KB) is decompressed per
lookup; a query with its legal moves takes about a second.

Output: the value of the position for the side to move and of every legal move:
  WIN / LOSS  forced under the real rules from a fresh repetition history
  DRAW        both sides can force exactly half the seeds
  DRAW*       neither side can force a win by play that terminates on its own;
              the real outcome would be decided by the repetition rule
The numbers are "seeds the side can force" as stored in the table, clamped to the
range that matters for win/draw/loss (see the package README).
"""
import os
import struct
import sys

try:
    if os.environ.get("OWARE_NO_ZSTD_MODULE"):
        raise ImportError
    import zstandard
    _HAVE_ZSTD_MODULE = True
except ImportError:
    import shutil
    import subprocess
    _HAVE_ZSTD_MODULE = False
    if not shutil.which("zstd"):
        sys.exit("this program needs the zstandard package (pip install zstandard) or the zstd command")


class _CliDecompressor:
    """Fallback when the zstandard module is missing: one `zstd -d` process per block."""

    def decompress(self, data, max_output_size=0):
        return subprocess.run(["zstd", "-d", "-q", "-c"], input=data, stdout=subprocess.PIPE, check=True).stdout

HEADER = struct.Struct("<8s8iQQQ")  # magic, n A V seeds blockBoards pack level w, size nblocks offsetsPos


class Rules:
    """Rules on mover-normalised boards: pits 0..ROW-1 belong to the side to move."""

    def __init__(self, row, seeds):
        self.ROW, self.PITS, self.SEEDS = row, 2 * row, seeds
        self.WIN = seeds // 2 + 1

    def legal(self, b):
        R = self.ROW
        if sum(b[R:]) > 0:
            return [i for i in range(R) if b[i]]
        return [i for i in range(R) if b[i] > R - 1 - i]  # must feed the opponent

    def play(self, b, i):
        """Returns (successor rotated so the new mover owns 0..ROW-1, seeds captured)."""
        R, P = self.ROW, self.PITS
        t = list(b)
        s, t[i] = t[i], 0
        pos = i
        if s >= P - 1:
            laps, s = divmod(s, P - 1)
            for j in range(P):
                if j != i:
                    t[j] += laps
            if s == 0:
                pos = P - 1 if i == 0 else i - 1
        while s:
            pos = 0 if pos == P - 1 else pos + 1
            if pos == i:
                continue
            t[pos] += 1
            s -= 1
        cap = 0
        if pos >= R and t[pos] in (2, 3):
            j = pos
            while j >= R and t[j] in (2, 3):
                cap += t[j]
                j -= 1
            if cap == sum(t[R:]):
                cap = 0  # grand slam: sowing stands, capture cancelled
            else:
                for q in range(j + 1, pos + 1):
                    t[q] = 0
        return tuple(t[R:] + t[:R]), cap


class Index:
    """Perfect hash of the boards with n seeds whose opponent row has an empty pit."""

    def __init__(self, row, max_seeds):
        R, S = row, max_seeds
        pa = [[0] * (S + 1) for _ in range(R + 1)]
        pz = [[[0, 0] for _ in range(S + 1)] for _ in range(R + 1)]
        for r in range(S + 1):
            pa[R][r] = 1 if r == 0 else 0
            pz[R][r] = [0, 1 if r == 0 else 0]
        for j in range(R - 1, -1, -1):
            for r in range(S + 1):
                pa[j][r] = sum(pa[j + 1][r - a] for a in range(r + 1))
                for f in range(2):
                    pz[j][r][f] = sum(pz[j + 1][r - a][f | (a == 0)] for a in range(r + 1))
        self.R = R
        self.preA = [[[0] * (S + 2) for _ in range(S + 1)] for _ in range(R)]
        self.preZ = [[[[0] * (S + 2) for _ in range(2)] for _ in range(S + 1)] for _ in range(R)]
        for j in range(R):
            for r in range(S + 1):
                for a in range(r + 1):
                    self.preA[j][r][a + 1] = self.preA[j][r][a] + pa[j + 1][r - a]
                    for f in range(2):
                        self.preZ[j][r][f][a + 1] = self.preZ[j][r][f][a] + pz[j + 1][r - a][f | (a == 0)]
        self.cntA = [pa[0][k] for k in range(S + 1)]
        self.cntZ = [pz[0][k][0] for k in range(S + 1)]
        self.base = []
        for n in range(S + 1):
            b = [0]
            for k in range(n + 1):
                b.append(b[-1] + self.cntZ[k] * self.cntA[n - k])
            self.base.append(b)

    def rank(self, b, n):
        R = self.R
        k = sum(b[R:])
        r, f, ro = k, 0, 0
        for j in range(R):
            a = b[R + j]
            ro += self.preZ[j][r][f][a]
            f |= a == 0
            r -= a
        r, rm = n - k, 0
        for j in range(R):
            a = b[j]
            rm += self.preA[j][r][a]
            r -= a
        return self.base[n][k] + ro * self.cntA[n - k] + rm


class Table:
    def __init__(self, directory):
        self.dir = directory
        self.files = {}
        self.cache = {}
        self.dctx = zstandard.ZstdDecompressor() if _HAVE_ZSTD_MODULE else _CliDecompressor()
        # read one header to learn the game
        names = sorted(f for f in os.listdir(directory) if f.startswith("oware_n") and f.endswith(".gmz"))
        if not names:
            sys.exit(f"no .gmz files in {directory}")
        h = self._open(int(names[-1][7:9]))[1]
        self.seeds = h["seeds"]

    def _open(self, n):
        if n in self.files:
            return self.files[n]
        path = os.path.join(self.dir, f"oware_n{n:02d}.gmz")
        f = open(path, "rb")
        hv = HEADER.unpack(f.read(HEADER.size))
        h = dict(zip("magic n A V seeds blockBoards pack level w size nblocks offsetsPos".split(), hv))
        if h["magic"] != b"OWAREGZ1" or h["n"] != n:
            sys.exit(f"{path}: not a layer {n} file")
        f.seek(h["offsetsPos"])
        offs = struct.unpack(f"<{h['nblocks'] + 1}Q", f.read(8 * (h["nblocks"] + 1)))
        self.files[n] = (f, h, offs)
        return self.files[n]

    def gM(self, n, idx):
        """Clamped gM of board index idx in layer n, in seeds."""
        f, h, offs = self._open(n)
        b = idx // h["blockBoards"]
        key = (n, b)
        raw = self.cache.get(key)
        if raw is None:
            f.seek(offs[b])
            raw = self.dctx.decompress(f.read(offs[b + 1] - offs[b]), max_output_size=h["blockBoards"] + 2)
            if len(self.cache) > 64:
                self.cache.clear()
            self.cache[key] = raw
        j = idx - b * h["blockBoards"]
        if h["pack"]:
            bit = j * h["w"]
            v = raw[bit >> 3] | (raw[(bit >> 3) + 1] << 8 if (bit >> 3) + 1 < len(raw) else 0)
            code = (v >> (bit & 7)) & ((1 << h["w"]) - 1)
        else:
            code = raw[j]
        return h["A"] + code


class Probe:
    def __init__(self, directory):
        self.T = Table(directory)
        self.seeds = self.T.seeds
        row = 6 if self.seeds in (36, 48) else 7  # tables published so far
        self.rules = Rules(row, self.seeds)
        self.IX = Index(row, self.seeds)

    def clamp(self, n, g):
        W = self.rules.WIN
        A = n - W if n >= W else 0
        V = self.seeds + 3 - n if n >= W else n + 1
        return max(A, min(A + V - 1, g))

    def guarantees(self, b, n):
        """(gM, gN): seeds the mover / the other side can force, from board b with n seeds."""
        R = self.rules.ROW
        if any(x == 0 for x in b[R:]):  # indexed board
            gM = self.T.gM(n, self.IX.rank(b, n))
            moves = self.rules.legal(b)
            if not moves:
                return gM, self.clamp(n, sum(b[R:]))
            g = min(self.T.gM(n - cap, self.IX.rank(c, n - cap)) for c, cap in (self.rules.play(b, i) for i in moves))
            return gM, self.clamp(n, g)
        moves = self.rules.legal(b)  # only the initial position gets here
        if not moves:
            return sum(b[:R]), sum(b[R:])
        gM, gN = 0, 99
        for i in moves:
            c, cap = self.rules.play(b, i)
            cm, cn = self.guarantees(c, n - cap)
            gM, gN = max(gM, cap + cn), min(gN, cm)
        return gM, gN

    def verdict(self, cM, cN, gM, gN):
        win = self.rules.WIN
        half = win - 1
        if cM >= win or cM + gM >= win:
            return "WIN"
        if cN >= win or cN + gN >= win:
            return "LOSS"
        if cM + gM == half and cN + gN == half:
            return "DRAW"
        return "DRAW*"


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    pr = Probe(argv[1])
    R, P = pr.rules.ROW, pr.rules.PITS
    nums = [int(x) for x in argv[2:]]
    if len(nums) != P + 3:
        sys.exit(f"expected {P} pit counts, two captured scores and the side to move")
    pits, cap, stm = nums[:P], nums[P:P + 2], nums[P + 2]
    tot = sum(pits)
    if tot + cap[0] + cap[1] != pr.seeds or stm not in (0, 1):
        sys.exit(f"the state must contain {pr.seeds} seeds in total and side_to_move must be 0 or 1")
    if cap[0] + cap[1] == 1:
        sys.exit("impossible state: a single seed can never be captured")
    b = tuple(pits[(j + R * stm) % P] for j in range(P))  # mover-normalised
    cM, cN = cap[stm], cap[1 - stm]
    gM, gN = pr.guarantees(b, tot)
    print(f"side to move P{stm}: {pr.verdict(cM, cN, gM, gN)}  (can force a final score >= {cM + gM}, "
          f"opponent can force >= {cN + gN}; bounds are clamped to the range that matters)")
    win = pr.rules.WIN
    if cM >= win or cN >= win or (cM == win - 1 and cN == win - 1):
        return
    moves = pr.rules.legal(b)
    if not moves:
        print("no legal move: remaining seeds are collected by row ownership")
        return
    for i in moves:
        c, k = pr.rules.play(b, i)
        cm, cn = pr.guarantees(c, tot - k)
        print(f"  pit {i + R * stm:2d}: captures {k:2d} -> {pr.verdict(cM + k, cN, cn, cm)}")


if __name__ == "__main__":
    main(sys.argv)
