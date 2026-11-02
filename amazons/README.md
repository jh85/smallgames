# amazons — ZDD + df-pn/BNS strong solver for small Amazons boards

A standalone C++20 program that **strongly solves** (exact win/loss game-theoretic
value) small variants of the Game of the Amazons, following

> J. Song and M. Müller, *An Enhanced Solver for the Game of Amazons*,
> IEEE TCIAIG 7(1), 2015 (`paper.pdf`).

and replacing/augmenting its df-pn search with the **BNS branch-number
arithmetic** ported from the shogi mate solver in `JHBR3/mate/bns.{h,cc}`
(Okabe's route-branch-number search).  Both arithmetics run in the *same*
search engine (a template/flag picks the number arithmetic only), which makes
the BNS-vs-df-pn comparison exact and lets each algorithm borrow the other's
machinery (threshold formulas, TT, move ordering) — see `src/arith.h`.

Proven positions are stored in a **ZDD (Zero-suppressed Decision Diagram)
verdict database** (`src/zdd.{h,cc}`): a position is encoded as a set of
on-variables (two bits per square: empty/white/black/burned), so a set of
positions is a family of sets and compresses via shared substructure.  The
solver probes it on node visits as an exact (collision-free) companion to the
lossy hash transposition table.

## Layout

```
src/board.{h,cc}       position, movegen, Zobrist hashing, symmetry canonicalization
src/arith.h            BNS / pn-dn arithmetic (ported from JHBR3/mate/bns.h)
src/tt.h               fixed-size clustered transposition table (hash-only keying)
src/zdd.{h,cc}         ZDD manager + VerdictDb (win/loss position families)
src/eval.{h,cc}        sound static bounds: area decomposition + plodding heuristic
src/solver.{h,cc}      the df-pn/BNS AND/OR search engine
src/bruteforce.{h,cc}  negamax+memo ground truth for tiny boards
src/main.cc            CLI: solve / verify / bench
test/                  unit + cross-validation tests (ctest)
tools/layercount.cc    exact per-layer reachable-position counts (RAM feasibility probe)
tools/wdlretro.cc      external-memory WDL table builder (see "5x5" section)
tools/wdlcheck.cc      exhaustive per-position consistency checker for wdlretro tables
```

Design notes:

- **DAG, not tree-with-cycles.** Every Amazons move burns one square, so the
  game graph is acyclic.  The shogi-specific machinery of `JHBR3/mate/bns.*`
  (repetition handling, path overrides, hand dominance, ply-keyed TT) is
  unnecessary and was dropped; the TT is keyed by canonical position hash
  only, maximizing transposition sharing.
- **Negamax frame.** Every node's numbers live in its own "side to move
  wins" frame; child views and thresholds are swapped when crossing an edge.
  With the swap every node is existential, so the search is a single
  OR-shaped recursion for both players.
- **Canonicalization.** Positions are canonicalized under board symmetries
  (D4 for square boards, 4 symmetries otherwise) *and* color swap with
  side-to-move normalization before hashing/ZDD-encoding — roughly a 8–16x
  reduction of the search space on symmetric boards.
- **Sound static bounds** (`src/eval.cc`): 8-connected areas of empty+queen
  squares cannot be entered by an opponent queen if they contain none
  (rays travel through 8-adjacent squares, and burned squares never
  recover).  A one-color area is a territory; the plodding heuristic gives a
  guaranteed move count `lo` for its owner.  With `E` empty squares total,
  first player F wins for sure if `2*f_lo > E`, second player S wins for
  sure if `2*s_lo >= E`.  This decides many late-game nodes without search
  and drives eval-informed proof-number initialization.
- **Standard small-board setup** is the literature's "(1,2)-points in the
  corner" placement (confirmed against Fig. 1 of the paper for 5x6), *not*
  the scaled 10x10 placement.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build                 # unit + cross-validation tests
```

## Usage

```sh
./build/amazons solve 4x4                        # strong-solve the 4x4 start
./build/amazons solve 4x5 --arith pn --seconds 60
./build/amazons solve 3x4 --setup one            # one-amazon-each variant
./build/amazons verify                           # tiny variants + known results
./build/amazons bench 4x4                        # BNS vs pn/dn, same engine
```

Options: `--arith bns|pn` `--tt-mb N` `--nodes N` `--seconds S`
`--eval on|off` `--zdd on|off` `--pn-init on|off` `--order on|off`.

## Results

All runs on this machine, Release build, default options (eval, ZDD verdict
DB, pn-init, move ordering on).  "nodes" = SearchImpl invocations.

| board | paper (Table I) | search solver | exhaustive table (`wdlretro`) |
|-------|-----------------|---------------|-------------------------------|
| 4x4   | 2nd player win  | 2nd player win ✓ (83,638 nodes, 1.0s) | 2nd player win ✓ (exact) |
| 4x5   | 1st player win  | unproven within 2h / 1.0e9 nodes | **2nd player win** (exact) — conflicts with the paper, see below |
| 5x4   | 1st player win  | unproven within 2e9 nodes | 1st player win ✓ (exact); the only losing reply of the 85 first moves is unique (layer 1: 84 W / 1 L) |
| 5x5   | 1st player win  | unproven within 1h / 2.2e8 nodes | infeasible on this machine (see below) |

**The 4x5 verdict contradicts Table I of the paper** (which lists 4 5 as a
first-player win).  The exhaustive table says the second player wins, and
the evidence for the table is about as strong as machine results get:

- the enumeration was reproduced exactly (every one of the 13 layer sizes,
  3,099,560,859 positions total) by an independent enumerator with a
  different canonicalization code path (`tools/layercount.cc`, using
  `Position::Canonical()` instead of the hot-path `CanonKey`);
- every stored verdict was re-derived from its child tables by an
  independent single-threaded checker for all 3.1e9 positions — the table
  is an exact fixpoint of the win/loss rule, hence correct given the
  graph;
- move generation was validated against an independently written
  grid-based reference on 537k positions (zero differences);
- the setup and movegen match the paper: after the paper's 5 6 opening
  White B1-B4xD4, Black has exactly 157 replies, the paper's own count.

Note the tables say 4x5 = second-player win but 5x4 = first-player win,
so the two orientations genuinely differ (the paper itself remarks that
"5 4 was much harder than 4 5").  The 4x4 and 5x4 tables agree with the
paper.  We report the 4x5 conflict as-is.

BNS vs pn/dn, same engine (`bench`): on 4x4, BNS solved with 84k node
entries vs 132k for pn/dn (1.6x fewer, 2.3s vs 8.0s).  On 4x5 neither
arithmetic proves the position within 1e9 nodes; the literature solve
needed the combinatorial-game machinery that is out of scope here (see
"Scope").  Both arithmetics agree on every verdict they reach (also
enforced by the test suite against brute force on tiny boards).

ZDD verdict DB: the persistent `wdl/std_*.bin` databases now contain only
sound verdicts (see "Errata"); the 4x5 attempt's DB holds ~1.6M proven
positions in ~52M ZDD nodes (~32 nodes/position, vs ~40 in a naive
per-variable encoding).

## Errata: the static eval was unsound

Earlier versions of this README reported 4x5 and 5x4 as first-player wins
*proven by the search solver* (83k–12M node runs, seconds to minutes).
Those verdicts were artifacts of an unsound static bound: `PloddingMoves`
in `src/eval.cc` never removed a queen's destination square from the
territory's empty set, so a second queen of the same color could "plod"
onto the just-occupied square, inflating the guaranteed move count and
letting `EvaluateBounds` declare wrong certain wins.  The bug only fires
in territories with two or more same-color queens, which the one-amazon
test suite never exercises.  With the fixed eval the search proves the
same node counts as before on 4x4 but cannot crack 4x5/5x4 within 1e9+
nodes — which is what exposed the bug: the freshly built exhaustive 4x5
table (root L) contradicted the solver's eval-tainted W, and a hand check
of the pivotal late-game node confirmed the table.  All ZDD verdict
databases written by the unsound solver were discarded and regenerated.
The exhaustive tables never used the eval and are unaffected.

## WDL tables

Amazons has no draws (a player unable to move loses), so WDL = W/L.
Two kinds of tables are produced:

**1. Exhaustive one-amazon tables** (`wdl/one_amazon_WxH.txt/.bin`):
every canonical position reachable from any one-amazon-each placement on
the board, retrograde-solved.  Text format: one line per position,
`white black burned verdict` as hex bitboards (canonical, side to move
normalized to white; `W` = side to move wins, `L` = loses).  Generate
more with:

```sh
./build/amazons wdl 3x5 --table wdl/one_amazon_3x5.txt --save-wdl wdl/one_amazon_3x5.bin
```

| board | canonical positions | wins | losses | time |
|-------|--------------------:|-----:|-------:|-----:|
| 2x2   | 7 | 3 | 4 | 0s |
| 2x3   | 75 | 50 | 25 | 0s |
| 2x4   | 510 | 339 | 171 | 0s |
| 2x5   | 2,858 | 1,903 | 955 | 0s |
| 2x6   | 16,916 | 11,380 | 5,536 | 0.02s |
| 3x3   | 860 | 605 | 255 | 0s |
| 3x4   | 26,055 | 19,390 | 6,665 | 0.03s |
| 3x5   | 324,096 | 241,483 | 82,613 | 0.6s |
| 3x6   | 3,801,334 | 2,822,842 | 978,492 | 12s |
| 4x4   | 428,708 | 327,747 | 100,961 | 1.2s |
| 4x5   | not completed within 15 min (position space too large for the plain enumerator; use `--limit` for a partial table) | | | |

**2. Solver verdict databases** (`wdl/std_WxH.bin`): the exact set of
positions proven during standard-setup solves, as a persistent ZDD.
They accumulate across runs — always pass the same file via
`--load-wdl`/`--save-wdl` and it only grows (verdicts are final; board
size is checked on load).  A warm table re-solves its root instantly
(4x4: 1 node entry vs 84k cold).  All databases were regenerated after
the eval fix (see "Errata"); only 4x4's contains its root verdict.

```sh
./build/amazons solve 5x4 --load-wdl wdl/std_5x4.bin --save-wdl wdl/std_5x4.bin
```

## 5x5: feasibility and the external-memory WDL builder

`tools/layercount.cc` measures the exact number of distinct canonical
positions per layer (layer k = k burned squares) reachable from the standard
5x5 start, by parallel BFS with hash dedup.  Measured on this machine
(`wdl/layercount_5x5.log`):

| layer | positions |
|------:|----------:|
| 0 | 1 |
| 1 | 130 |
| 2 | 22,872 |
| 3 | 1,614,937 |
| 4 | 77,756,261 |
| 5 | 1,409,771,529 |
| 6 | > ~5e9 (exceeded a 105 GiB address-space cap mid-expansion) |

The combinatorial ceiling is C(25,4)·C(21,4)·C(17,k)/16 per layer
(6.2e11 canonical positions total, peaking at layers 8–9 with 1.15e11
each).  With the reachability fraction trending toward the 75–87% seen in
the completed one-amazon tables, the full 5x5 table needs roughly
3e11–6e11 positions — ~1 TB RAM just to enumerate in memory, i.e. beyond
this machine.  Hence the disk-based builder:

`tools/wdlretro.cc` (`./build/wdlretro`) builds the full WDL table with
external-memory passes over a resumable on-disk layout.  Amazons' game
graph is a strictly layered DAG (every move burns one square), so one
forward enumeration sweep plus one backward retrograde sweep suffices —
no fixpoint.  Positions are canonical base-4 keys partitioned by hash;
RAM stays bounded by ~(layer size / --parts), edges are never stored
(children are regenerated), and every pass checkpoints so a rerun simply
continues.  See the design comment at the top of `tools/wdlretro.cc`.

```sh
./build/wdlretro 5x5 /ssd/amz55 --parts 4 --threads 64 --ram-gb 600 \
    --expect w --verify 100000
```

Resource model for 5x5 on a 750 GB RAM / 7 TB NVMe machine with
`--parts 4`: peak partition hash ~350 GB, retrograde state ~200 GB, final
table ~8.25 B/position (~2.5–5 TB), and roughly 2–4 weeks of compute
(enumeration costs `parts` expansion passes per layer, retrograde
`parts`+1).  `--expect w` checks the known literature result (5x5 is a
first-player win) at the end; `--verify N` re-derives N sampled verdicts
per layer from the child tables.  Validated end-to-end on 4x4 (root L),
4x5 (root L) and 5x4 (root W): every layer size of both 20-square tables
matches the independent `layercount` enumerator exactly, and every stored
verdict (3.1e9 positions per board) was re-derived from the child tables
by the independent `wdlcheck` checker (`tools/wdlcheck.cc`).  The built 4x5 and 5x4 tables are kept in
`wdlrun/` (keys + 2-bit verdicts, ~25 GB each).

## Scope / not implemented

The paper's full evaluation machinery (combinatorial-game values of local
areas, blocker-territory databases, thermographs, subzero thermography) is
what let it solve 5x6/6x4; it is out of scope here.  Without it, 5x5 and
larger are attempted under node/time limits and reported honestly as
unknown when unproven.  The ZDD is used as a verdict database only (no
symbolic retrograde engine).
