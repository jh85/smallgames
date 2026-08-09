# Dobutsu Shogi strong solver (ZDD + retrograde analysis) — 4×3 through 8×3

Strongly solves Dobutsu Shogi (どうぶつしょうぎ) on boards from 4×3 through 8×3 with a
ZDD-based minimal perfect hash and retrograde analysis — the NOCCA × NOCCA architecture
(after Yamamoto & Hoki, GPW 2022), extended to a game with captures, pieces in hand,
drops, and promotion. Produces a packed 2-bit WDL table answering win/draw/loss for any
position. Both full oriented indexing and dense left/right-reflection-orbit indexing are
supported. Results through 6×3 have been computed; the reflection-reduced 7×3 production
solve is in progress as of 2026-08-08, and 8×3 is supported but not yet solved.

## Results (solved + validated 2026-07-30)

* **Gote (the second player) wins**: the initial position is a LOSS for Sente, decided at
  retrograde pass **78** — reproducing Tanaka (2009)'s famous 78-ply result.
* Longest forced win in the position space: **173 plies** (last positions decided at pass
  173), matching the maximum distance reported in the literature.
* Position space (pseudo-reachable, black-to-move canonical): **1,175,944,473** positions;
  totals W/L/D = 960,361,246 / 209,460,776 / 6,122,451.
* Validation:
  * **Full agreement with an independent solver**: all **246,803,167** reachable
    left/right-reflection orbits (the symmetry-reduced count matching Tanaka 2009)
    compared against an independent Rust reference implementation — **0 mismatches**, and
    the symmetry-reduced reachable W/L/D split
    **196,773,087 / 47,347,380 / 2,682,700** equals the reference exactly.
  * Fixpoint audit over all 1,175,944,473 positions: 0 inconsistencies.
  * Independent depth-bounded minimax spot checks: consistent.
  * ZDD cardinality equals an independent DP count; mirror-symmetric positions always
    receive equal values.
* Solve time on 2× EPYC 9115 (64 threads): 159.9 seconds with full indexing and 75.5
  seconds with reflection-reduced indexing. The reduced result was compared against both
  orientations of every orbit in the full table: 0 mismatches.

## Results — 5×3 variant (`--board 5x3`, solved + validated 2026-07-30)

* **The initial position is a DRAW** — neither side can force a win, agreeing with the
  reference solver.
* Position space: **11,986,076,088** pseudo-reachable positions (ZDD: 7,090 nodes);
  totals W/L/D = 9,426,875,292 / 2,386,404,108 / 172,796,688. Longest forced win in the
  space: **191 plies** (convergence at pass 192).
* Validation, same protocol as 4×3:
  * All **3,359,910,526** reachable reflection orbits compared against the reference Rust
    solver (ROWS=5): **0 mismatches**; symmetry-reduced reachable W/L/D =
    2,597,975,993 / 683,720,498 /
    78,214,035, equal to the reference and its README.
  * Fixpoint audit over all 11,986,076,088 positions: 0 inconsistencies.
  * Independent minimax spot checks + mirror-value consistency: all pass.
* Reflection-reduced validation: **5,993,385,012** pseudo-position orbits solved in 28.2
  minutes (versus 53 minutes full-rank), with W/L/D =
  4,713,708,013 / 1,193,273,640 / 86,403,359. Every reduced value matched both
  orientations in the full table, and the reduced K plane contains exactly
  3,359,910,526 reachable orbits.
* Full table `wdl_5x3.bin` is 2.8 GB; reduced WDL+K table `wdl_5x3_lr.bin` is 2.25 GB.
* Note on files: `wdl_*.bin` are this solver's WDL tables (the deliverables);
  `rust_dump*.bin` are the reference solver's per-position dumps used only as
  cross-validation input and can be deleted once verification has passed.

## Results — 6×3 variant (`--board 6x3`, solved 2026-07-31) — new result

* **The initial position is a DRAW**; unlike 5×3, even the immediate chick push holds
  (all 4 opening moves draw). To our knowledge the 6×3 game value had not been
  published before.
* Position space: **72,908,154,405** pseudo-reachable positions (ZDD: 9,142 nodes);
  totals W/L/D = 55,023,935,614 / 16,019,869,041 / 1,864,349,750. Longest forced win:
  **243 plies** (convergence at pass 244). Solve time: 11.4 h; `wdl_6x3.bin` is 18.2 GB.
* Validation: no independent reference solver exists at this size (the Rust reference's
  in-memory pipeline does not fit and its disk mode computes no values), so 6×3 rests on:
  the full fixpoint audit over all 72.9 G positions (**passed, 0 inconsistencies**),
  independent depth-bounded minimax spot checks (passed), mirror-value consistency
  (passed), and the record-by-record validation of this exact code at 4×3 and 5×3.

## Supported larger variants (not yet solved)

The independent ZDD/DP count cross-check and all structural self-tests pass at both sizes:

| board | full positions | reflection orbits | reduced ZDD nodes | reduced WDL |
|---|---:|---:|---:|---:|
| 7×3 | 318,621,272,760 | 159,318,056,898 | 35,844 | 39,829,522,424 bytes |
| 8×3 | 1,107,543,870,153 | 553,797,891,822 | 43,481 | 138,449,481,152 bytes |

## Reflection reduction, reachability, and table formats

The default mode retains the original full oriented ZDD ranks. `--symmetry lr` instead
builds a dense ZDD containing exactly one representative of each left/right-reflection
orbit. The reduced rank is not a sparse minimum of two old ranks: its cardinality is
independently checked using Burnside's lemma and a DP count of mirror-fixed positions.

`solve` writes an 8 KiB header followed by a two-bit WDL plane. `reach` performs an
independent, resumable forward traversal and appends a one-bit **K plane** (`1` = reachable)
to the same file. Only after the traversal, count check, K-plane write, and `fsync` succeed
is the header updated. Interrupted work resumes from mode-specific `reach_work_*` and
`reach_frontier_*` files.

```
./dobutsu solve --board 4x3
./dobutsu reach --board 4x3 --threads 64
./dobutsu analyze --board 4x3       # now also prints reachability

./dobutsu solve --board 7x3 --symmetry lr --threads 64 --ckpt 5
./dobutsu reach --board 7x3 --symmetry lr --threads 64
```

The C++ ZDD assigns separate ranks to left/right reflections. Consequently its 4×3 K plane
contains **493,573,042** reachable full-rank positions. The frequently cited
**246,803,167** count is after the Rust/reference solver identifies reflected positions;
33,292 of those reflection orbits are fixed points. For symmetry-reduced NN evaluation,
select one representative from each reachable mirror pair rather than interpreting the
non-representative K bit as unreachable.

In a reflection-reduced table the K plane has one bit per **orbit**. It therefore records
that the orbit is reachable, rather than maintaining separate bits for its two oriented
members. Mirror-fixed orbits have only one member.

All formats keep the same plane layout:

```
offset 0:                         8 KiB header
offset 8192:                      ceil(N/32) uint64 WDL words (2 bits/rank)
offset 8192 + WDL bytes:          ceil(N/64) uint64 K words (1 bit/rank)
```

* Version 1: full-rank WDL.
* Version 2: full-rank WDL+K.
* Version 3: reflection-reduced WDL, with an optional orbit K plane.

The current binary reads existing version-1 and version-2 tables unchanged. Version-3
tables use a different dense rank mapping and therefore require `--symmetry lr`; they do
not overwrite or reuse the full-rank files.

| board | full WDL | full WDL+K | reduced WDL | reduced WDL+K |
|---|---:|---:|---:|---:|
| 4×3 | 293,994,312 | 440,987,376 | 147,013,680 | 220,516,424 |
| 5×3 | 2,996,527,216 | 4,494,786,728 | 1,498,354,448 | 2,247,527,576 |
| 6×3 | 18,227,046,800 | 27,340,566,104 | 9,113,975,704 | 13,670,959,464 |
| 7×3 | 79,655,326,384 | 119,482,985,480 | 39,829,522,424 | 59,744,279,544 |
| 8×3 | 276,885,975,736 | 415,328,959,512 | 138,449,481,152 | 207,674,217,632 |

Sizes in this table are bytes and include the 8 KiB header.

## Build & commands

```
make                 # g++ -O3, C++20
./dobutsu selftest   # ZDD/DP count, rank/unrank, iterator, child-rank checks
./dobutsu solve      # full solve -> wdl_4x3.bin (resumable, ~2 min)
./dobutsu selftest --board 8x3
./dobutsu selftest --board 8x3 --symmetry lr
./dobutsu solve --board 7x3 --symmetry lr --threads 64 --ckpt 5
./dobutsu reach --board 4x3 --threads 64
./dobutsu reach --board 4x3 --symmetry lr --threads 64
./dobutsu reach-audit --board 4x3
./dobutsu info --board 4x3
./dobutsu verify-rust  # optional: compare with an external rust_dump.bin
./dobutsu compare-full --symmetry lr  # reduced table vs existing full table
./dobutsu audit      # full fixpoint audit of the finished table
./dobutsu bfcheck [n d]  # independent minimax spot check
./dobutsu analyze    # initial position + per-move values
./dobutsu probe <b|w> <ROWS*3 cells> <sente-hand|-> <gote-hand|->
```

Use `--board ROWSx3` with `ROWS` from 4 through 8; the default is 4×3. The default table is
`wdl_<rows>x3.bin`; `--symmetry lr` uses `wdl_<rows>x3_lr.bin`. `--table PATH` overrides
either name. Tables and reference dumps are read and written relative to the current
working directory, so the solver tree can be moved without changing source code.

Generated WDL tables, reference dumps, reachability work files, and the compiled executable
are excluded by `.gitignore`. They should be published separately; record a direct download
URL, byte size, and SHA-256 digest here after choosing the artifact host:

| board/format | external WDL+K download | SHA-256 |
|---|---|---|
| 4×3 full v2 | not published yet | `bbbed0ea5ea0dc0db12d2c6cd646bd5a60efd9d4535dde53ca8989ab73ee7b13` |
| 4×3 reduced v3 | not published yet | `3b28fcf677567f81f904d58b64d078f44b8002fd8848a7942ea109794815c2f1` |
| 5×3 reduced v3 | not published yet | `692ec9297bd028eb9b1e419a1670327c8a2db1773bdea57353ee9e62b8db7562` |
| 6×3 | not published yet | — |
| 7×3 | not published yet | — |

Probe cells are row-major from row 0 (White/Gote's home, top) to row `ROWS-1` (Sente's
home): `.` empty, `LEGCH` = Sente's Lion/Elephant/Giraffe/Chick/Hen, `legch` = Gote's.
Hands are letter strings from `EGC` (e.g. `EC`), `-` if empty. Example for 4×3:

```
./dobutsu probe b  e l g  . c .  . C .  G L E  -  -
```

## Position space and ZDD design

Canonical positions are **black (Sente) to move**; white-to-move positions are handled by
the involution flip rows + swap colors + swap hands. The space contains every assignment
with:

* exactly one lion per side on the board, the black lion **never on row 0** (a position
  with the mover's lion already on the far rank cannot occur — the opponent's previous
  turn would have been terminal);
* 2 elephants, 2 giraffes, 2 chicks in total, each either on the board (under either
  owner; chicks possibly promoted to hens) or in a hand (always as an unpromoted chick).

**ZDD items** (`ROWS × 3 × 10 + 6` variables): each square has 10 non-empty piece states,
then **6 unary items for Sente's hand only** — E≥1, E≥2, G≥1, G≥2, C≥1, C≥2. Gote's hand
is the per-type remainder (2 − on-board − Sente's hand), so it needs no items and the
hash stays bijective. The construction frontier tracks only (square-flag, lions placed,
#E, #G, #C on board). The full-index ZDD ranges from **5,038 nodes** at 4×3 to **13,246
nodes** at 8×3. The reflection-reduced ZDD carries a small comparison state and ranges
from **13,097** to **43,481 nodes**, while reducing the indexed universe to
553,797,891,822 orbits at 8×3. Rank/unrank/iteration work exactly as in the NOCCA solver.

A pleasant consequence of the canonicalization: after Sente moves, the child's *explicit*
(Sente) hand equals the parent's Gote hand, which the move never touches — so every child
differs from the flipped parent in **at most 2 board squares**, making prefix-cached
child ranking very effective.

## Rules / value conventions

Follows the reference Rust solver (which reproduces Tanaka 2009):

* Chick promotes to Hen (gold-general moves) on *moving* into the far rank (mandatory);
  dropped chicks do not promote in place.
* Captures join the mover's hand (hen demotes to chick); drops on any empty square with
  no restrictions.
* Terminal WIN: the mover can capture the opponent's lion (regardless of protection).
* Terminal LOSS: the opponent's lion stands on the mover's home rank and cannot be
  captured — the Try rule, formalized one ply after the entering move. Equivalently
  (absorbed into pass 1 of the retrograde): moving your lion onto the far rank on a
  square no enemy piece attacks is an immediate win. Note that the enemy **lion** counts
  as an attacker (source of a subtle bug found during validation — the value of the
  initial position flips to DRAW without it).
* A stalemated player (no legal move at all) is a draw by the reference convention;
  repetition/infinite play is a draw (retrograde-unknown).

## Files

| file | contents |
|---|---|
| `dobutsu.cpp`, `Makefile` | complete standalone solver and build recipe (~2,400 lines C++) |
| `README.md` | results, format documentation, build instructions, and command examples |
| `wdl_<rows>x3.bin`, `wdl_<rows>x3_lr.bin` | packed full-rank or reflection-reduced table: 8 KiB header + two-bit WDL plane and, after `reach`, one-bit K plane; WDL 0 = draw, 1 = win, 2 = loss; K 1 = reachable |

The optional `verify-rust` command expects a separately generated 17-byte-per-record
reference dump. Neither the independent Rust repository nor its multi-gigabyte dumps are
required to build or run this solver, so they are not included in this package.
