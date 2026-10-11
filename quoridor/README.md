# Quoridor WDL tables for 6x6 and 7x7 boards

Strong solution (full win/draw/loss table over *all* states, not just those reachable
from the start) of small Quoridor variants, produced by `quoridor_wdl.cpp`.

Rules and conventions follow the reference Rust solver in `../quoridor-solving/`
(Grant Slatton's "Solving Quoridor"): Player1 starts at the north center square and
races south, Player2 mirrored, Player1 moves first; standard jump/diagonal rules; a
wall may not cut off either pawn's current square from its goal row; forced
repetitions and stalemates are draws.

## Method

A state is `(side to move, pawn1, pawn2, walls left 1, walls left 2, wall configuration)`.

1. **ZDD over wall slots.** The family of all non-overlapping wall configurations is
   built as a zero-suppressed decision diagram via frontier-based construction (the
   frontier tracks, per anchor column, whether a vertical wall hangs down from the row
   above, plus the horizontal-wall state of the current/previous anchor). The ZDD
   yields *exact* per-cardinality configuration counts — used to size the tables (cf.
   the product-form upper bound in the MCTS paper, formula (2), which is off by ~2x at
   higher wall counts) — and cross-validates the explicit enumeration (the solve
   aborts if enumeration and ZDD disagree). For 6x6 the 95.4M legal ≤8-wall
   configurations compress to 4,195 ZDD nodes.

2. **Layered retrograde analysis.** Walls on the board only ever increase, so the
   game graph is a DAG over layers k = walls placed. Layer k is solved given layer
   k+1: for each wall configuration and each split of walls-in-hand, the subgame of
   pure pawn moves (2·S² states) is solved by backward induction with counters
   (BFS attractor propagation); wall placements are "exit edges" into the already
   solved layer k+1. Unresolved states after the fixpoint are draws — the correct
   treatment of loopy game graphs. Two layers reside in RAM at a time; each layer is
   embarrassingly parallel over wall configurations.

Values are 2 bits per state: 0 = draw, 1 = Player1 wins, 2 = Player2 wins
(winner-encoded, not side-to-move-relative; terminal positions carry the winner).

## Validation

- `selftest` mode solves the board with the fast engine, then re-solves the reachable
  game graph with a deliberately naive, independent engine (string-and-BFS legality,
  map-based retrograde) and compares *every reachable state's* value. Passes on
  2x3, 3x3 (w=1,2), 4x3, 3x4, 2x5, 4x4.
- `validate_readme.sh` reproduces all 36 solved entries checked from the reference
  repo's results table, including the 8x3 w=3 forced draw and 4x7's alternating
  pattern. 36/36 match.

## Build

```sh
make            # g++ -O3 -march=native -fopenmp -std=c++17
make test       # ./qwdl selftest 3 3 2 — cross-check vs the naive reference engine
make qwdl_sym   # separate four-way-symmetry solver; leaves qwdl unchanged
make test-sym   # symmetry + recomputed-top cross-checks
./validate_readme.sh   # reproduce all 36 reference-repo results (36/36)
```

## Files

- `quoridor_wdl.cpp` — solver (single file, OpenMP)
- `quoridor_wdl_sym.cpp` — symmetry-reduced solver, built as `qwdl_sym`;
  it does not change `quoridor_wdl.cpp` or the existing table format
- `run_production.sh` — the production sweep: 6x6 w=0..4 and 7x7 w=0..3
- `validate_readme.sh` — reproduces the reference repo's published results
- `checksums/*.SHA256SUMS` — per-file digests for completed tables
- `tables/WxH_wN/` — saved tables: `meta.txt`, per-layer `layer_k.masks`
  (sorted u128 wall bitmasks) and `layer_k.wdl` (packed 2-bit values, one
  `blockBytes` block of 2·S² states per (config, walls-in-hand split)).
  Not in this repository — see *Downloading the tables* below.

Not redistributed here, and gitignored: `quoridor-solving/` (Grant Slatton's
reference Rust solver, [github.com/grantslatton/quoridor-solving](https://github.com/grantslatton/quoridor-solving)),
`paper.pdf`, `production.log`, and the compiled binary.

## Usage

```sh
./qwdl count 6 6 8          # ZDD exact wall-config counts + state-space size table
./qwdl solve 6 6 3 --save tables/6x6_w3   # full WDL solve (all layers)
./qwdl selftest 3 3 2       # cross-check vs naive reference engine
./qwdl probe tables/6x6_w3  # interactive query of a saved table

./qwdl_sym count 7 7 8      # exact four-way symmetry counts and memory estimates
./qwdl_sym solve 7 7 4 --recompute-top  # low-memory start-outcome solve
./qwdl_sym persistent 7 7 4 tables/7x7_w4_sym --threads 64
./qwdl_sym probe tables/7x7_w4_sym
```

`qwdl_sym` quotients wall configurations by left/right reflection and by
top/bottom reflection or 180-degree rotation combined with swapping the players.
For 7x7 w=4 this reduces 4,451,897,991 configurations to 1,112,995,030
representatives. A normal two-layer symmetry solve would still need about 1.40 TiB,
but `--recompute-top` generates final-layer blocks on demand and has a projected
427.54 GiB peak. This mode deliberately does not support `--save` yet: it computes
the start outcome by exhaustive retrograde analysis but does not retain the complete
top-layer table. The `persistent` mode first streams that top layer to a resumable
file, then saves every lower layer; the completed 7x7 w=4 raw table is
1,611,374,418,659 bytes and can answer every state through `qwdl_sym probe`.

Probe query format: `t p1r p1c p2r p2c l1 l2 [Hr,c Vr,c ...]` (t = 1 or 2 to move,
coordinates row,col zero-based, walls by anchor + orientation). Prints the state's
value and the value after every legal move, e.g. the 6x6 w=3 start position:

```
echo "1 0 3 5 3 3 3" | ./qwdl probe tables/6x6_w3
```

## Downloading the tables

The legacy tables are **not** stored in this repository — 238 GB across four games.
Each `(board size, wall count)` is published as its own archive, because each
`tables/WxH_wN/` directory is exactly what `./qwdl probe` consumes, and the games are
wildly unequal in size: 7x7 w=2 is a 13 MB download, 7x7 w=3 over 150 times that.

| game | archive | expands to | ratio | SHA-256 of archive | download |
|---|---:|---:|---:|---|---|
| 7x7 w=2 | 13,316,185 B | 1,132,856,068 B | 85× | `e99416c9d8b650dcbecef2eff3238f001886ca9bdfac1e95445bb295de64d9fc` | not published yet |
| 6x6 w=3 | 160,799,267 B | 6,276,744,913 B | 39× | `8a8b9b24d2367f98d1bd81a878934786d74a57006a66e9e5b3f9155b857f3989` | not published yet |
| 6x6 w=4 | 3,814,541,904 B | 116,802,888,793 B | 31× | `469539a80e293cbd1e13ba39d6ba5deea6bddadd34ab039605dd18c95a48ed28` | not published yet |
| 7x7 w=3 | 2,056,449,999 B | 130,355,258,883 B | 63× | `17dc3608f7699b5e558e2ccd6c55cb3c83809619ea81c4b34bf71e1315a2a3c2` | not published yet |

All four together are 5.6 GiB, expanding to 237 GiB. The ratios differ a lot by game because
the two file kinds compress very differently: the packed 2-bit `.wdl` planes reach ~79x on
their own, while the sorted `.masks` bitmasks manage ~19x, so a game's ratio tracks its mix.

Extract from this directory, which recreates `tables/WxH_wN/` where the solver expects it:

```sh
tar --zstd -xf 7x7_w3.tar.zst          # or: curl -sL <url> | tar --zstd -xf -
cd tables/7x7_w3 && sha256sum -c ../../checksums/7x7_w3.SHA256SUMS
echo "1 0 3 6 3 3 3" | ./qwdl probe tables/7x7_w3
```

The archives use `zstd -12 --long=27 -T0`; the 128 MiB window is exactly zstd's default
decoder limit, so stock `tar --zstd -xf` works with **no extra flags**. Each game also has a
per-file manifest in `checksums/`, which after extraction identifies *which* layer is
corrupt rather than only that the archive did not match — worth running, since a single
layer of 7x7 w=3 is 96 GB.

Re-solving instead of downloading is feasible for the small games and expensive for the
large ones: `./qwdl solve 7 7 2 --save tables/7x7_w2` is minutes, while 6x6 w=4 and
7x7 w=3 are the multi-hour, high-memory runs recorded in `production.log`
(`w>=4` on 7x7 exceeds this machine's RAM with the legacy `qwdl` solver).

## Results

The full WDL tables deliberately include states that cannot be reached from the
standard initial position. The start-position outcome under optimal play and the
reachable subsets (exact except where marked estimated) are:

| configuration | start-position outcome | reachable positions | full WDL table | reachable |
|---|---|---:|---:|---:|
| 6x6 w=3 | Player1 wins | 18,654,053,548 | 23,916,601,800 | 77.996% |
| 6x6 w=4 | Player1 wins | 322,608,610,016 | 446,051,566,800 | 72.325% |
| 7x7 w=2 | Player2 wins | 3,677,348,176 | 4,357,291,680 | 84.395% |
| 7x7 w=3 | Player2 wins | 418,124,712,600 | 501,660,690,720 | 83.348% |
| 7x7 w=4 | Player2 wins | ~20.43 trillion (estimated) | 24,821,119,282,848 | ~82.3% (estimated) |
| 8x8 w=2 | Player1 wins | not enumerated | 26,850,257,280 | ~85-90% (estimated) |
| 8x8 w=3 | Player1 wins | not enumerated | 6,346,552,383,360 | ~85-90% (estimated) |

Here `w` is the number of walls per player. Reachable counts include terminal
positions; play stops as soon as either pawn reaches its goal row. The w=4 reachable
figure and the 8x8 ranges are extrapolations, not enumerated counts. The 7x7 w=4
four-way symmetry table has
6,205,280,412,387 mathematical state orbits and 6,205,411,261,056 stored states; see
`RESULTS.md` for the symmetry scheme and detailed state definition.

See `RESULTS.md` for full-table statistics. The outcomes and statistics are also
recorded in `production.log`.
