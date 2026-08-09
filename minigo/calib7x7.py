#!/usr/bin/env python3
"""Calibration sweep for the 7x7 endgame solver.

Samples KataGo book positions by empty-point count, runs the df-pn solver on
each with the komi-9 predicate (T=10: black area margin >= 10) under a node
budget, and records cost. Output: data/calib7x7.tsv

Usage: calib7x7.py [BOOK_TSV] [PER_BUCKET] [NODE_CAP]
"""
import sys, random, subprocess, concurrent.futures, re, time

BOOK = sys.argv[1] if len(sys.argv) > 1 else "data/book7x7_tt.tsv"
PER_BUCKET = int(sys.argv[2]) if len(sys.argv) > 2 else 8
NODE_CAP = int(sys.argv[3]) if len(sys.argv) > 3 else 1_500_000_000
ARITH = sys.argv[4] if len(sys.argv) > 4 else "pndn"
BINARY = sys.argv[5] if len(sys.argv) > 5 else "./minigo4"
TAG = sys.argv[6] if len(sys.argv) > 6 else ARITH
EXTRA = sys.argv[7:]
BUCKETS = [(13, 14), (15, 16), (17, 18), (19, 20), (21, 22), (23, 24), (25, 26)]
THREADS_PER = 8
CONCURRENT = 7
OUT = f"data/calib7x7_{TAG}.tsv"

def main():
    rows = []
    with open(BOOK) as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 6 or not p[2]:
                continue
            board = p[0]
            empties = board.count(".")
            rows.append((empties, board, p[1], p[2], p[3]))
    rng = random.Random(42)
    samples = []
    for lo, hi in BUCKETS:
        pool = [r for r in rows if lo <= r[0] <= hi]
        rng.shuffle(pool)
        samples += pool[:PER_BUCKET]
    print(f"{len(samples)} samples across {len(BUCKETS)} buckets", file=sys.stderr)

    def solve(s):
        empties, board, side, wl, ssm = s
        t0 = time.time()
        try:
            r = subprocess.run(
                [BINARY, "dfpn", "--w", "7", "--h", "7", "--T", "10",
                 "--pos", board, "--side", side, "--tt-gb", "6",
                 "--threads", str(THREADS_PER), "--nodes", str(NODE_CAP),
                 "--arith", ARITH] + EXTRA,
                capture_output=True, text=True, timeout=1800)
            out = r.stdout
            m = re.search(r"dfpn: (WIN|LOSS|UNSOLVED).*?nodes=([0-9,]+).*?in ([0-9.]+)s", out)
            if m:
                res, nodes, secs = m.group(1), int(m.group(2).replace(",", "")), float(m.group(3))
            else:
                res, nodes, secs = "ERROR", 0, time.time() - t0
        except subprocess.TimeoutExpired:
            res, nodes, secs = "TIMEOUT", NODE_CAP, time.time() - t0
        return (empties, side, wl, ssm, res, nodes, secs, board)

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=CONCURRENT) as ex:
        for res in ex.map(solve, samples):
            results.append(res)
            print(f"[{len(results)}/{len(samples)}] empties={res[0]} side={res[1]} "
                  f"wl={res[2]} -> {res[4]} nodes={res[5]:,} {res[6]:.1f}s", file=sys.stderr)

    results.sort()
    with open(OUT, "w") as f:
        f.write("empties\tside\tbook_wl\tbook_ssM\tresult\tnodes\tseconds\tboard\n")
        for r in results:
            f.write("\t".join(str(x) for x in r) + "\n")
    print(f"wrote {OUT}", file=sys.stderr)

if __name__ == "__main__":
    main()
