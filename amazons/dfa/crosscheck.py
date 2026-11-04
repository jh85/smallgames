#!/usr/bin/env python3
"""Runs the symbolic and the dense solver on small boards and compares every
layer's win and loss counts and the root value.

The symbolic solver also relabels every position of every layer from its
successors with explicit rules (--exhaustive), so a pass here means both
engines reach the same complete solution and that solution is a fixpoint of
the rules.

Given a third binary, the bit-parallel solver is checked too, bit for bit
against the dense tables, through its files, with one and with several classes.
"""
import re
import subprocess
import sys
import tempfile

BOARDS = [
    # (board, queens, expected root or None)
    ("3x3", 1, None),
    ("3x3", 2, None),
    ("3x3", 4, None),
    ("3x4", 1, None),
    ("3x4", 2, None),
    ("4x3", 3, None),
    ("3x5", 2, None),
    ("4x4", 1, None),
    ("4x4", 2, None),
    ("4x4", 4, "second"),
]

SYMBOLIC_ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s")
DENSE_ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s")


def layers(text, pattern):
    out = {}
    for line in text.splitlines():
        if line.startswith("#"):
            continue
        m = pattern.match(line)
        if m:
            k, p, w, l = map(int, m.groups())
            out[k] = (p, w, l)
    return out


def root(text):
    m = re.search(r"# root(?: \d+x\d+)?: (first|second) player wins", text)
    return m.group(1) if m else None


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit("usage: crosscheck.py AMZDFA DENSE [SLABSOLVE]")
    amzdfa, dense = sys.argv[1:3]
    slabsolve = sys.argv[3] if len(sys.argv) == 4 else None
    failures = 0
    for board, queens, expected in BOARDS:
        sym = run([amzdfa, "solve", board, "--queens", str(queens), "--exhaustive", "--threads", "8"])
        with tempfile.TemporaryDirectory() as tmp:
            den = run([dense, "solve", board, tmp, "--queens", str(queens), "--threads", "8", "--verify-samples", "20000"])
            a, b = layers(sym, SYMBOLIC_ROW), layers(den, DENSE_ROW)
            ok = bool(a) and a == b
            if expected is not None:
                ok = ok and root(sym) == expected and root(den) == expected
            if slabsolve:
                for classes, canon in ((1, "min"), (3, "max")):
                    with tempfile.TemporaryDirectory() as store:
                        # A mismatch against the tables in tmp makes this exit non-zero.
                        out = run([slabsolve, "solve", board, "--root", store, "--queens", str(queens),
                                   "--threads", "8", "--aux", "8", "--classes", str(classes), "--canon", canon,
                                   "--reserve", "0", "--compare", tmp])
                        c = layers(out, DENSE_ROW)
                        ok = ok and c == b
                        if expected is not None:
                            ok = ok and root(out) == expected
        print(f"{board} q={queens}: {len(a)} layers {'ok' if ok else 'MISMATCH'}")
        if not ok:
            failures += 1
            for k in sorted(set(a) | set(b)):
                if a.get(k) != b.get(k):
                    print(f"  layer {k}: symbolic {a.get(k)} dense {b.get(k)}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
