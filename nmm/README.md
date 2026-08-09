# morris — strong solver for N Men's Morris via two-ZDD indexing

Implements and extends Takeda & Hoki, *"Analysis of the Number of Piece Configurations in
N Men's Morris"* (IPSJ SIG-GI 2020-GI-43(8)): the paper's two-ZDD minimal perfect hash
over unique, pseudo-reachable configurations, plus everything the paper does not cover —
placement-phase indexing, atomic move generation, cyclic retrograde analysis, and packed
WDL tables — for Twelve Men's Morris and a custom Sixteen Men's Morris stage-gated by an
exact feasibility report.

The paper and its English translation are not redistributed here; section and table
references below point into them.

## Build & test

```
mkdir -p build && cd build && cmake .. && make -j && ctest
```

Needs CMake ≥ 3.16, a C++20 compiler, and pthreads; builds `morris`, `cli_verify`, and the
four test binaries (board, zdd1, counts, 3MM full validation). Tables are read and written
under `data/m<game>/` **relative to the current working directory**, so the tree can be moved
without editing source; `--dir D` overrides the location.

## Commands

```
./build/morris build-zdds --game 12          # ZDD1s + integrity-gated ZDD2 forests (saved)
./build/morris solve --game 12               # full solve (resumable per partition/layer)
./build/morris estimate --game 16            # REQUIRED stage gate; exact Burnside counts
./build/morris solve --game 16               # refuses without --force (see estimate)
./build/morris nodecount-check --game 12     # paper's global ZDD2 reproduction
./build/morris query --game 12 --white ANW,BN --black CE,... --hands 0,0
./build/cli_verify 9|11|12             # per-subset paper-table reproduction (NOFILTER=1
                                       #   reproduces the 12MM unique-only Table 10)
```

## Generating the WDL tables (12MM)

The tables are **not** stored in this repository — 12MM is 44 GiB. Everything needed to
regenerate them is here:

```
./build/morris build-zdds --game 12          # ZDD2 forests, integrity-gated (1.6 h)
./build/morris solve      --game 12          # phase 2/3 then placement (8.4 h)
./build/morris query      --game 12 --white "" --black "" --hands 12,12   # initial position
```

Both steps write into `data/m12/` and are **resumable per partition/layer**: a completed
partition is skipped on re-run, so the solve can be interrupted and restarted. Requirements
for the full 12MM run, as measured on 2× EPYC 9115 / 64 threads / 723 GB RAM:

| | |
|---|---|
| Time | ~10 h total: forests 1.6 h, phase 2/3 5.1 h, placement 3.3 h |
| Disk | ~44 GiB: 1,478 partition files (~30 GiB) plus the two ZDD2 forests (14 GiB) |
| RAM | Dominated by the ZDD2 forests (497 M + 381 M nodes) plus the working partition |
| Threads | `--threads N`, default = hardware concurrency |

Output layout in `data/m12/`: `ph23_wWW_bBB.wdl` (100 phase-2/3 partitions),
`place_Hhh_wWB_bBB.wdl` (1,378 placement layers), `zdd2_ph23.bin` and `zdd2_place.bin` (the
two forests), and `MANIFEST.sha256`.

## Downloading and verifying the tables

The 12MM tables are hosted outside this repository as one `m12.tar.zst` archive: a 12.5 GiB
download that expands to 44 GiB. The overall ratio is 3.5× — the packed WDL partitions
compress far better than that, but the two ZDD2 forests are 14 GiB of node structure and
barely compress at all.

| board | archive | expands to | SHA-256 of archive | download |
|---|---|---|---|---|
| 12MM | 13,373,076,822 B | 46,892,638,927 B | `09ef5659cec657e935c92ece04cb94769cb45076026e264e704c3d394faf45f6` | not published yet |

Extract from this directory, which recreates `data/m12/` where the solver expects it:

```
tar --zstd -xf m12.tar.zst          # or: curl -sL <url> | tar --zstd -xf -
cd data/m12 && sha256sum -c MANIFEST.sha256
```

The archive ships with its own `MANIFEST.sha256` covering all 1,480 files; the same list is
committed here as [`checksums/m12.SHA256SUMS`](checksums/m12.SHA256SUMS), so you can verify
an extracted copy against the repository rather than against the download itself. Every one
of those 1,480 digests was re-checked against the on-disk tables before publication.

The archive is made with `zstd -12 --long=27 -T0`; the 128 MiB window is exactly zstd's
default decoder limit, so stock `tar --zstd -xf` works with **no extra flags**.

**16MM is not published.** The full 16MM solve is infeasible here (see below) and the
partial endgame run on this machine was interrupted by a power outage, so no 16MM table is
distributed — only the truncated-band tooling that produces one.

## Validation summary (see docs/design.md for the full reconciliation)

* Tables 4–10 reproduced: 3/5/9MM exact; 6MM/11MM/12MM exact after arithmetically-proven
  typos (6MM 5-3=90,540; 11MM 10-3=89,297,208, 11-11=367,447; 12MM 6-5=144,232,144);
  12MM Table 10 shown to be the *unique-only* counts (multinomial-symmetry proof); our
  filtered 12MM total is 16,147,057,219. 7MM 3-3 documented as an unresolved source
  anomaly; every other 7MM row exact with the 14-mill board.
* 3-3 subsets of flying games use the mill-preserving group (order 48 on 3-ring boards);
  this reproduces the paper/Gasser exactly and is proven value-preserving (both players
  fly forever; adjacency is dead). Board group everywhere else, including all placement.
* ZDD1: 12MM count = 264,369,400,848 = the paper's "maximum integer". Node counts are
  canonical-minimal and differ from Table 11 (their variable order differs; functionally
  irrelevant, all round-trip tests exhaustive/randomized pass).
* Second-ZDD structure: 6MM global reachable nodes 90,321 vs paper 90,320 (leaf-count
  convention) — structural reproduction.
* 3MM strongly solved end-to-end and compared with an independent flat solver with its
  own hand-written rules: all 5,934 reachable states identical; initial value WIN.
* Every ZDD2 build passes a hard integrity gate (per-subset counts vs sweep tally); an
  early lock-free hash-cons race was found this way and replaced with striped-lock
  chaining.

## State model

* Canonical black-free encoding: (white mask, black mask, white hand, black hand); White
  moves first; side-to-move is derived during placement (white iff wh == bh) and explicit
  in phase 2/3. Full state partitions: `ph23_wWW_bBB.wdl` (dense rank × 2 stm) and
  `place_Hhh_wWB_bBB.wdl` (one hand pair per H = wh+bh; single acyclic sweep per layer).
* WDL encoding: 2 bits/state — 00 unknown (construction only), 01 LOSS, 10 DRAW, 11 WIN,
  side-to-move perspective. Rules per the paper + prompt: one capture per mill event
  (double mills give one), captured piece not from a mill unless all are, capture skipped
  if the opponent has no board piece, flying at exactly 3 pieces, <3 pieces or no legal
  move loses in phases 2/3, 12/16MM full-board placement is a draw, unresolved cycles are
  draws (no repetition rule).

## Sixteen Men's Morris (custom, stage-gated)

Rules banner is printed at startup. The mill list (16 ring sides + 16 consecutive-triple
spoke mills) is compiled into `src/board.cpp`; `config/morris16.json` is a dump of it,
written by the `board` test — the JSON files are generated documentation, not inputs.

`estimate --game 16` (exact): 1.10 × 10¹⁵ states, 274 TB flat WDL, ≈ 544 machine-days here
— the full solve therefore refuses to run without `--force`. The generic implementation, formats, and resumable
solver support a future cluster run; exact truncated endgame solves (piece-count bands)
are the supported alternative on this machine.

## RESULTS (solved 2026-08-01/02 on 2x EPYC 9115, 64 threads, 723 GB RAM)

**TWELVE MEN'S MORRIS IS A FIRST-PLAYER WIN** (White, moving first from the empty board).
Every winning first placement is a corner point; midpoint placements only draw. Unlike
Nine Men's Morris (draw, Gasser 1996), the diagonal spokes and full 12-piece hands make
the first move decisive. Solve time ~10 h (forests 1.6 h + phase 2/3 5.1 h + placement
3.3 h); tables ~12 GB across 1,478 partition files + two ZDD2 forests (~14 GB).
Verification: sampled retrograde-invariant audit clean (2M states, 0 failures, 0
unknowns); mirror partitions bitwise-consistent; the identical pipeline matches an
independent solver on every reachable 3MM state.

## Files

| file | contents |
|---|---|
| `src/`, `CMakeLists.txt` | the solver: board/moves, two ZDD layers, retrograde solve, CLI |
| `tests/` | board, ZDD1 round-trip, paper-count, and full 3MM validation tests |
| `config/*.json` | generated board dumps (points, edges, mills, symmetries) for reference |
| `docs/design.md` | design notes and the full paper-table reconciliation |
| `README.md` | results, rules, formats, build and command reference |
| `checksums/m12.SHA256SUMS` | per-file SHA-256 manifest for the externally published 12MM tables |

The CMake build tree, run logs, and the generated `data/` tables are excluded by
`.gitignore`. The source paper, its English translation, and the coding-agent prompt used to
produce this implementation are not redistributed here.
