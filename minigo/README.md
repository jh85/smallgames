# minigo — solving Go on small boards (Tromp-Taylor rules)

`minigo.cpp` is a single-file C++ solver for Go on boards up to 7x7 under
Tromp-Taylor rules:

- **Area scoring**: final score = stones + empty regions reaching only one
  color (black minus white).
- **Game end**: two consecutive passes; the board is then scored as-is.
- **Positional superko**: a play may not recreate any earlier board position
  (passes exempt).
- **Suicide**: allowed by default (multi-stone suicide removes the placed
  chain), Tromp-Taylor style. `--suicide 0` switches to no-suicide rules.

## Results (empty board, black to move, komi 0)

| board | value (black area margin) | matches literature |
|-------|---------------------------|--------------------|
| 1x1   | 0 (draw)                  | yes                |
| 2x2   | B+1                       | yes (Tromp's 2x2 analysis) |
| 3x3   | B+9                       | yes (van der Werf, MIGOS)  |
| 4x4   | B+2                       | yes (van der Werf, MIGOS)  |
| 5x5   | B+25 (whole board)        | yes (van der Werf, MIGOS)  |

5x5 is pinned exactly by proving the predicate "Black can force final area
score >= 25" (WIN at threshold T=25); 25 is the maximum possible score, so the
value is exactly +25. The proof ran on the parallel df-pn engine: 80.1 billion
node visits across 60 threads in 86 minutes (2026-08-06), leaving a proof tree
of 239,900,492 proven positions in `data/go5x5_T25.wdl` (3.8 GB). Black's
winning opening is the center point c3, as in van der Werf's MIGOS solution.
A second run under no-suicide rules confirms B+25 there as well (74.0 billion
nodes, 78 minutes; `data/go5x5_T25_nosuicide.wdl`, 240,804,509 positions).

## Why not a ZDD backward solve (the Breakthrough approach)?

The `/data1/breakthrough` solver works because Breakthrough is **acyclic**
(every move strictly increases an advancement measure), so a ZDD minimal
perfect hash over state slabs admits a single backward sweep. Go is
fundamentally different:

1. **Cyclic game graph** — captures and kos revisit board positions, so there
   is no layer ordering and a backward sweep needs a fixpoint over ~10^12
   states x edges.
2. **History dependence** — under positional superko the legality of a move
   depends on the whole game history, not the board; a `(board, side)` state
   does not have a well-defined game value, so a full-table "strong solution"
   in the Breakthrough sense is not even well-posed for Go.
3. **Size** — legal 5x5 boards alone number 414,295,148,741; with side/pass/ko
   annotations a table would run to terabytes, for a value function that
   superko makes ill-defined anyway.

So ZDDs are not useful here, and the solver instead does a **forward proof
search** from the root (the approach of van der Werf's MIGOS, which first
solved 5x5 in 2002): three-valued (WIN/LOSS/UNKNOWN) iterative-deepening
AND/OR search with

- exact position keys (no hash collisions): board pair + side + pass flag in
  2N+2 bits, symmetry-folded over the 8 board symmetries for the TT;
- positional superko enforced exactly against the actual search path, with a
  GHI (graph-history-interaction) guard: values that depend on repetitions of
  ancestors outside a node's own subtree are never stored in the TT;
- static cutoffs from Benson unconditional life extended to unconditional
  territory (alive stones + enclosed regions where the opponent can never form
  an eye, including trapped opponent stones — the opponent there can never
  make an eye because every empty point of the region touches an immortal
  chain);
- enhanced transposition cutoffs, staged TT move, killer moves, history
  heuristic, capture-first ordering;
- lazy-SMP parallel search: N threads run independent iterative deepening with
  jittered move ordering over one shared lockless TT (XOR-guarded 24-byte
  entries; torn writes fail validation and count as misses).

Superko caveat (shared with all published small-board Go solutions): static
life-and-territory cutoffs assume the standard result that an unconditionally
alive group's area cannot be affected by superko restrictions; van der Werf's
5x5 result rests on the same machinery, and our values reproduce the
literature exactly.

## Build

```
make            # g++ -O3 -march=native -std=c++17 -pthread
make test       # ./minigo selftest — rules, Benson life, territory statics
```

## Files

- `minigo.cpp` — the solver; modes: `selftest`, `solve` (parallel ID),
  `dfpn` (parallel proof-number search, fastest), `sweep` (exact value via
  threshold bisection), `probe`
- `calib7x7.py` — the 7x7 calibration sweep driver
- `extract_book.py` — extracts book positions from the KataGo 7x7 books
- `data/calib7x7*.tsv` — calibration results (committed; they are the evidence
  behind the 7x7 tables above)
- `data/go5x5_T25.wdl` — WDL table for 5x5, threshold T=25 (format below);
  `data/go4x4_T2.wdl`, `data/go3x3_T9.wdl`, `data/go2x2_T1.wdl` — small boards.
  Published separately, see *Proof tables* below.

Not in this repository, and gitignored:

| excluded | why |
|---|---|
| `KataGo/` | upstream source tree, cloned for reference — [github.com/lightvector/KataGo](https://github.com/lightvector/KataGo) |
| `book7ttb40s9435-20210806.tar.gz`, `book7jpb40s9435-20210806.tar.gz` | KataGo's 7x7 opening books (Tromp-Taylor / Japanese), not ours to redistribute |
| `data/book7x7_tt.tsv`, `data/book7x7_jp.tsv` | ~1.13M positions each, extracted from those books by `extract_book.py`; regenerate them from your own copy of the books rather than downloading them here |
| solver binaries, `*.log`, `*.out`, `*.pid` | build products and run scratch |

## WDL file format (`MGWDL1`)

Little-endian binary:

```
char magic[8] = "MGWDL1\0\0"
u32 W, H
u8  suicideAllowed, u8 superko (1 = positional)
i16 threshold T
u64 count
count records, 16 bytes each, sorted by key:
  u64 canonicalKey   // min over 8 symmetries of (black | white << N),
                     // | side << 2N | passFlag << (2N+1)
  u8  res            // 1 = WIN (black can force area score >= T), 2 = LOSS
  u8  best           // best/refuting move hint: cell index, 63 = pass, 255 = n/a
  u8  depth          // search depth at which the entry was proven
  u8  pad[5]
```

Every record is a **proven** fact about optimal play (no heuristic entries).
The table contains all positions proved during the run that were reachable and
path-independent (see GHI guard above); it includes the empty board, whose
entry is the game value. Note the table is a *proof tree*, not a full strong
solution table: at Black-to-move (OR) nodes only one winning move needs
proving, so positions after Black mistakes (e.g. a corner opening) may be
absent — they can be solved on demand with `solve`/`dfpn` on that position.
Records from the ID engine carry a best-move hint; df-pn records store 0xff.

### df-pn implementation notes (hard-won, keep in mind when extending)

- Unproven pn/dn sums must clamp to a value strictly below "infinity"
  (PN_SAT = PN_INF-1): if clamped sums saturate to the same value that means
  proven, the root can be pseudo-disproven and the run dies (observed).
- Do not monotone-merge (max) unproven TT stores across threads: combined
  with clamping it ratchets estimates upward until saturation.
- Each node's expand/select loop is bounded (512 passes) as a livelock valve
  against shared-TT races between threads.
- Pass-terminal static results must be stored in the TT; parents refresh child
  values from the TT and would otherwise reselect a proven pass child forever.
- Superko/GHI: track the minimum ply of any path repetition that influenced a
  value; store to the TT only when that ply is not above the node itself.

Query a position:

```
$ ./minigo probe --file data/go5x5_T25.wdl \
    --pos '...../...../...../...../.....' --side b --passed 0
side=black passed=0 threshold=25
WIN (black can reach >= 25)
```

That is the empty board, so its entry is the game value itself. Probing prints WIN/LOSS at
the file's threshold plus the stored best-move hint, or `NOT IN TABLE` if the position was
not needed for the proof — which is common, since at Black-to-move nodes only one winning
move has to be proven:

```
$ ./minigo probe --file data/go5x5_T25.wdl \
    --pos 'X..../...../...../...../.....' --side w --passed 0
NOT IN TABLE
```

Any such position can be re-proven on demand with `./minigo solve` or `dfpn`.

## Proof tables: download, or re-prove

The tables are **not** stored in this repository. Each is published as a single
zstd-compressed file:

| table | archive | expands to | ratio | SHA-256 of archive |
|---|---:|---:|---:|---|
| `go2x2_T1.wdl.zst` | 90 B | 172 B | 1.9× | `f270829402ef1730e84fde18c65789af069d843e875c49aac8e41d5e1bd395f4` |
| `go3x3_T9.wdl.zst` | 2,890 B | 15,468 B | 5.3× | `bc25d3306755c0d4876b7275968a994f218fe26aac135665152cce047668e02f` |
| `go4x4_T2.wdl.zst` | 7,781,321 B | 44,224,172 B | 5.6× | `75bbbd18c5289d81c6a59a0673aaf76087e0f8cbafc3fd5c6b4070bd45c38bfa` |
| `go5x5_T25.wdl.zst` | 386,745,262 B | 3,838,407,900 B | 9.9× | `1dad778228e3f6ab3600c1bfe941b789747092325f92d534367ebd89b28cc0b1` |
| `go5x5_T25_nosuicide.wdl.zst` | 401,022,031 B | 3,852,872,172 B | 9.6× | `30030dfd72a3e1e7f022d99ad5f7081447eead1782aef350ac2541d261cf6302` |

All five together are 759 MiB, expanding to 7.7 GB. Decompress into `data/`, then check the
uncompressed files against [`checksums/tables.SHA256SUMS`](checksums/tables.SHA256SUMS):

```
zstd -d go5x5_T25.wdl.zst -o data/go5x5_T25.wdl
cd data && sha256sum -c ../checksums/tables.SHA256SUMS
```

Archives use `zstd -12 --long=27`; the 128 MiB window is zstd's default decoder limit, so
plain `zstd -d` needs no extra flags.

### These tables are not reproducible — download them

Unlike the other solvers in this repository, **re-running a solve does not reproduce the
published file**, and cannot be expected to. The parallel engine is lazy-SMP: threads run
independent iterative deepening over one shared lockless TT, so which positions get proven —
and therefore what lands in the dump — depends on thread interleaving. A proof tree only has
to contain *one* winning move at each Black-to-move node, and different runs pick different
ones.

Measured on 3x3 T=9, the same command twice with 8 threads:

```
run A: 286 proven positions, 4,604 bytes
run B: 121 proven positions, 1,964 bytes
```

Both are correct proofs of the same verdict (WIN); they are simply different certificates.
`--threads 1` *is* deterministic and reproduces byte-for-byte, but single-threaded is not a
practical way to redo the 5x5 run (80.1 billion node visits across 60 threads, 86 minutes).

So the digests above verify a **download**, not a rebuild. What is reproducible is the
*answer*: any position can be re-proven on demand with `solve`/`dfpn`, and the game values in
the results table are stable facts that match the literature.

## 7x7 calibration sweep (2026-08-06)

`calib7x7.py` sampled 56 KataGo book positions in buckets of 13-26 empty
points and solved each with df-pn at T=10 ("Black wins at komi 9", ties to
White), 8 threads, 1.5e9-node cap. Results in `data/calib7x7.tsv`:

| empties | solved | bucket mean nodes | bucket max nodes |
|---------|--------|-------------------|------------------|
| 13-14   | 6/8    | —                 | 2 hit the cap    |
| 15-16   | 8/8    | 22k               | 112k             |
| 17-18   | 8/8    | 31k               | 82k              |
| 19-20   | 8/8    | 683k              | 3.8M             |
| 21-22   | 8/8    | 37M               | 178M             |
| 23-24   | 8/8    | 1.3M              | 5.6M             |
| 25-26   | 8/8    | 13M               | 91M              |

Practical frontier: everything with <= 24-26 empties solves in seconds to ~2
minutes on 8 threads; the whole book population with <= 24 empties (~175k
positions) is a few days of compute on this machine. The two capped positions
were 14-empty near-jigo whole-game questions (proof cost concentrates near
the true value, not with tree size).

Key finding about the book: deep off-line book leaves carry unreliable evals
(tiny visit counts; one sampled leaf claimed Black -37 in a position where
Black has a free 3-stone capture — our solver proves Black >= +20 raw there,
and the ID engine's rules core is validated on 5x5). On-line book nodes with
large visit counts agree with our results. So exact solving is genuinely
adding information at the book's fringes — filter comparisons by the `visits`
column. Solved book positions are evaluated without game history (superko
restarts at the given position), the standard convention for such tables.

## Arithmetic A/B: proof/disproof numbers vs branch numbers (2026-08-07)

`--arith pndn|bns` switches the opposite-side number at a node between
classic df-pn sums (Nagai) and Okabe-style route branch numbers
(selected child + count of unresolved siblings, as in JHBR3's mate/bns.cc);
everything else in the engine is identical. Measured, same statics/TT/GHI:

| problem                     | pndn                | bns                  |
|-----------------------------|---------------------|----------------------|
| 3x3 T=9 (1 thread)          | 2,329 nodes         | 963 nodes            |
| 4x4 T=2/T=3/T=16 (1 thread) | 1.2M / 1.4M / 2.0M  | 3.7M / 4.2M / 4.3M   |
| 7x7 calib, 56 positions     | 3.42B nodes, 1189s  | 5.01B nodes, 2097s   |
| 5x5 T=25 proof (60 threads) | 80.1B nodes, 86 min | 373.1B nodes, 326 min|

Verdict: pn/dn wins at every scale that matters here (bns only won the toy
3x3). Interpretation: (a) the symmetry-folded exact-key TT already collapses
most DAG duplication, so the sum-inflation disease branch numbers cure is
mild; (b) with PN_SAT clamping, inflation can no longer cause the
saturation failure; (c) sums carry a work-remaining signal that counts do
not, which matters at Go's wide AND nodes; (d) the df-pn+ heuristic inits
feed magnitudes into sums but are largely flattened by counts. This matches
JHBR3's own measured default (kPnDn). bns remains available via --arith.

## JHBR3-derived engineering ports (2026-08-07)

Three techniques from JHBR3's mate/bns.cc were ported to the pn/dn engine
(toggles: `--mc`, `--freshen`, `--ovr`; all default on, binary `minigo6`):

- move cache: memoized movegen + child canonical keys per position
  (superko filtered per visit against the live path);
- sibling-view freshening: between full refreshes every 16 passes, the
  just-searched child keeps its returned values and siblings are not
  re-probed;
- path-override list: proven-but-route-dependent verdicts (superko taints)
  cached while their anchoring ancestor stays on the path, with max-shallow-
  dependency anchors and level-by-level "loop head owns its loops"
  dissolution (replaces discard-and-re-search).

Same-binary 5x5 T=25 measurement (60 threads, 192 GB TT):

| config           | nodes  | wall    |
|------------------|--------|---------|
| all features off | 94.2B  | 105 min |
| all features on  | 41.3B  | 50 min  |

~2.3x fewer nodes / ~2.1x faster on the deep flagship problem. On the 7x7
calibration suite (56 short solves) the same trio measured neutral — the
techniques need long runs with warm caches and many superko taints to pay
off. Node reduction comes from freshening + overrides (the move cache is
pure memoization; its t0 hit rate on 5x5 was 11.8%). Attribution between
freshening and overrides individually was not separated.

## Usage

```
./minigo selftest
./minigo solve --w 5 --h 5 --T 25 --tt-gb 150 --threads 60 --dump data/go5x5_T25.wdl
./minigo sweep --w 4 --h 4 --tt-gb 8 --threads 16     # exact value by threshold bisection
./minigo probe --file data/go5x5_T25.wdl --pos '...' --side b
```

`--T k` solves the boolean game "can Black force final score >= k". The WDL
verdict for any komi follows: Black wins at komi k iff value > k (integer
values; the 5x5 value is +25, so Black wins for any komi < 25).
