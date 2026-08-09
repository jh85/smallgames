# Strong Solver for Two-Player Chinese Checkers (m×m diamond boards)

A production C++ implementation that strongly solves two-player Chinese
Checkers on the m×m diamond gameplay area, producing a complete
win/draw/loss/illegal (WDL) table at 2 bits per stored state, following

> N. R. Sturtevant, *On Strongly Solving Chinese Checkers*, Advances in
> Computer Games (ACG), 2019.

Supplementary sources used to pin down implementation details:
* N. R. Sturtevant and A. Saffidine, *A Study of Forward versus Backwards
  Endgame Solvers with Results in Chinese Checkers*, CGW\@IJCAI 2017.
* N. R. Sturtevant, *An Efficient Chinese Checkers Implementation: Ranking,
  Bitboards, and BMI2 pext and pdep Instructions*, ACG 2022.

The primary target is the 6×6 board with 6 pieces per player
(2,313,100,389,600 positions); the code is fully configurable for boards
up to 7×7 and 1–6 pieces per player, including the 7×7/6 game (see
"Scaling to 7×7 with 6 pieces" below).

## Build & test

```
make            # builds solver, bruteforce, query, crosscheck, rulecount, zddbench, unit tests
make test       # unit tests (geometry, ranking, symmetry maps, movegen, index scheme)
make validate-small   # solves 7x7/1, 7x7/2, 7x7/3, 4x4/6 and checks Table 1
make validate-mid     # solves 5x5/6 and 7x7/4 (rhombus areas)
make solve-6x6        # the full 6x6/6 solve (~145 GB RAM, several hours)
```

Requirements: Linux, g++ ≥ 11 (C++20), ~145 GB RAM for the 6×6 solve
(smaller games need proportionally less; 7×7/4 needs ~8 GB).

## Tools

* `src/solver` — the optimized parallel solver.
  `--m M --p P [--areas-sym] [--dir D] [--threads N] [--no-lr]
   [--no-backprop] [--null-moves] [--ckpt-mins M] [--resume] [--order 0|1|2]`
  Writes `table.bin` (2-bit WDL table), `groups.bin`, `meta.txt`, `log.txt`
  into the run directory; validates against the paper's Table 1 when the
  game is one of the published rows.
* `src/query <run_dir> --m M --p P [--areas-sym] ...` — query utility:
  `--initial`, explicit states (`--p1 c,c,... --p2 c,c,... --stm 1|2`),
  `--stats` (full recount), and `--verify N` (random-sample local
  consistency check of the solved table: WIN ⇒ ∃ LOSS successor, LOSS ⇒ all
  successors WIN, DRAW ⇒ no LOSS successor ∧ ≥1 DRAW successor).
* `src/bruteforce M P [dump] [flags]` — an *independent* reference solver
  (its own board representation, move generator, ranking, and full-space
  value iteration with no symmetry reduction) used to validate the
  optimized solver state-by-state; also implements the rule variants used
  in the rule-discrepancy investigation (`--null`, `--nogoalexit`,
  `--part2soft`, `--nostartentry[2]`, `--passalways`, `--originoccupied`,
  `--layers`).
* `src/crosscheck M P <bf.val> <run_dir>` — compares every full-space state
  value from a brute-force dump against the canonical table (exercises both
  symmetry maps and the ranking on every state).
* `src/rulecount M P <spec>` — fast static illegal-state counter for
  arbitrary "blocked goal" patterns (used to reverse-engineer the paper's
  part-2 rule from its published illegal counts).
* `src/zddbench M P <run_dir>` — the ZDD-vs-dense benchmark (see below).

## Tables: what to download, what to rebuild

Solve outputs live in `runs/<name>/` (`table.bin`, `groups.bin`, `meta.txt`, `log.txt`) and
are **not** stored in this repository. Only the 6×6/6 table is published, because it is the
only one that is expensive to produce. Everything else rebuilds in under 20 minutes on 64
threads, and the solver is deterministic — a fresh solve reproduces the original `table.bin`
byte for byte (verified for 4×4/6, 7×7/2 and 7×7/3).

| run | game | flags | `table.bin` | solve time | how to get it |
|---|---|---|---:|---:|---|
| `m7p2sym` | 7×7, 2 | `--areas-sym` | 164,584 B | 5 s | rebuild |
| `m7p2` | 7×7, 2 | — | 317,816 B | 4.5 s | rebuild |
| `m4p6` | 4×4, 6 | — | 213,256 B | 7 s | rebuild |
| `m7p3` | 7×7, 3 | — | 35,304,888 B | 9 s | rebuild |
| `m5p6` | 5×5, 6 | — | 602,737,384 B | 5.9 min | rebuild |
| `m7p4sym` | 7×7, 4 | `--areas-sym` | 3,958,834,400 B | 9.7 min | rebuild |
| `m7p4` | 7×7, 4 | — | 7,892,116,160 B | 17.1 min | rebuild |
| **`m6p6`** | **6×6, 6** | — | **144,736,218,904 B** | **19.8 h, ~145 GB RAM** | **download** |

To rebuild the small ones:

```
make validate-small        # 7x7/1, 7x7/2, 7x7/3, 4x4/6   (~20 s total)
make validate-mid          # 5x5/6 and 7x7/4 --areas-sym  (~16 min)
./src/solver --m 7 --p 4 --dir runs/m7p4 --ckpt-mins 0     # any single run
```

The two brute-force dumps used by `crosscheck` are likewise regenerated rather than
published (seconds each):

```
./src/bruteforce 4 6 runs/bf_4_6.val
./src/bruteforce 7 2 runs/bf_7_2.val
./src/crosscheck 4 6 runs/bf_4_6.val runs/m4p6
```

`runs/m5p6b` is not published or listed above: it is a checkpoint/resume exercise of the
5×5/6 game (killed mid-pass, restarted with `--resume`) whose table came out **bit-identical**
to `runs/m5p6`, which is the evidence that the resume path is correct.

### Downloading the 6×6 table

Published as one `m6p6.tar.zst`: a 19.2 GiB download that expands to 134.8 GiB (7.0×).

| archive | expands to | SHA-256 of archive | download |
|---|---|---|---|
| 20,577,339,884 B | 144,744,204,005 B | `14494913ebcaee98c198d010b3de9c0114b3bfbd7febde98e1f021e8ecf70dcf` | not published yet |

Extract from this directory, which recreates `runs/m6p6/` where the tools expect it:

```
tar --zstd -xf m6p6.tar.zst          # or: curl -sL <url> | tar --zstd -xf -
cd runs/m6p6 && sha256sum -c ../../checksums/m6p6.SHA256SUMS
./src/query runs/m6p6 --m 6 --p 6 --initial
./src/query runs/m6p6 --m 6 --p 6 --verify 100000
```

The archive is made with `zstd -12 --long=27 -T0`; the 128 MiB window is exactly zstd's
default decoder limit, so stock `tar --zstd -xf` works with **no extra flags**. Per-file
digests are in [`checksums/m6p6.SHA256SUMS`](checksums/m6p6.SHA256SUMS).

Rebuilding the 6×6 table instead of downloading it needs ~145 GB of RAM (the table is held
resident and checkpointed hourly), 64 threads for ~20 h, and ~135 GB of free disk:

```
make solve-6x6             # resumable: re-run with --resume after an interruption
```

## State representation and algorithm

**Board.** Cells of the m×m skewed grid are numbered row-major along the
diagonal rows of the drawn diamond (row k = x+y, x ascending). The six hex
neighbors of (x,y) are (x±1,y), (x,y±1), (x+1,y−1), (x−1,y+1) — verified
against Figure 2 of the paper (exactly six adjacent moves from the start
position, and 10 total moves including the four single-hops). The
180-degree rotation maps id → N−1−id (a bit-reversal on the board mask);
the left-right mirror maps (x,y) → (y,x).

**Canonical states & symmetry.** Every position is stored with the side to
move normalized to "player 1" (top player, moving down); a
player-2-to-move position maps through rotation + color swap, which is an
exact value-preserving isomorphism (factor 2). When the start area is
mirror-symmetric (p ∈ {1,3,6}, and p=4 with `--areas-sym`), left-right
symmetry drops every piece-placement block whose mirror rank is smaller;
self-symmetric blocks store both mirror images of the opponent placement
(≈0.2% redundancy on 6×6, matching the paper's reported factor 1.998).
The 6×6 table stores 578,946,872,400 states × 2 bits ≈ 144.7 GB.

**Perfect ranking.** Colex combinatorial ranking:
index = base[rank(moverSet)] + rank(opponentSet within the remaining
cells); both ranks are O(p) with popcount tricks; unranking is used only
per block/group. `base[]` is a prefix-sum table over stored blocks.

**Move generation.** Bitboard steps (empty neighbors) plus a DFS over hop
chains with a visited mask; the origin is vacated during the chain (a chain
may cross its origin square) but a move must end on a different cell.

**Solving (cyclic retrograde value iteration).**
1. *Init:* every state is statically classified: illegal (Def. 2 part 1 /
   Def. 3 part 2), terminal loss (opponent's win condition holds), else
   unknown. Zero-initialized memory doubles as "unknown", so the 145 GB
   table needs no init writes for the ~99.9% unknown states.
2. *Passes:* iterate to a fixpoint. A state becomes WIN when some legal
   successor is LOSS, and LOSS when all legal successors are WIN (moves
   into illegal states do not exist; a state with no legal moves is a LOSS
   under the normal-play convention, per the 2017 paper). When a LOSS is
   proven, all its predecessors are immediately marked WIN
   (win back-propagation; predecessors are generated by applying the move
   generator to the opponent's pieces — the move relation is symmetric).
   Unproven states at the fixpoint are draws (Def. 4).
3. *Ordering / locality:* passes iterate **opponent-placement-major**: for
   a fixed opponent set b, *every* successor of every state (a,b) lies in
   the single block rank(rot(b)) (the mover's move changes only a), so all
   successor lookups for a whole group hit one ~148 KB, L2-resident window
   instead of missing to DRAM. Group scan direction alternates per pass.
   Converged groups (no unknown states) are skipped via per-group counters.
4. *Values* are 2-bit fields updated with lock-free CAS; threads pull
   groups from an atomic queue.

**Checkpoint/restart.** The table (plus group counters and a metadata
file) is written atomically (tmp + rename) at a configurable interval,
both between passes and *mid-pass* (workers park at group boundaries).
Because updates are monotone (unknown → proven, never back), re-running an
interrupted pass from its checkpoint is sound. `--resume` continues from
the last checkpoint; SIGINT/SIGTERM trigger a final checkpoint and clean
exit. (Validated by killing a 5×5 solve mid-pass and resuming: final
counts are identical to an uninterrupted run.)

## Rules implemented, and every assumption/ambiguity found

The paper defines the rules in prose; the following decisions were needed.
Each is validated against the published Table 1 counts wherever possible.

1. **Positions** = 2 × C(N,p) × C(N−p,p) (both sides to move, ignoring
   symmetry) — matches every Table 1 row exactly.
2. **Start/goal areas.** Start = p cells in the mover's corner, goal = the
   180-degree rotation. For p ∈ {1,2,3,6} the areas are the first p cell
   ids (p=2 is *asymmetric*: tip + left cell of row 1 — confirmed because
   7×7/2 matches Table 1 exactly with this shape and *fails* with the
   symmetric row-1 pair). For p=4 the area is the **rhombus** {tip, row 1,
   center of row 2} (`--areas-sym`): with first-4 cells 7×7/4 fails
   (wins 31,540,458,780 vs 31,532,340,944), with the rhombus it matches
   **exactly**; the rhombus is also the only shape consistent with the
   symmetric-state count 15,822,357,347 reported for 7×7/4 in the 2022
   bitboard paper. (p=5 is untested against any published number; the
   solver defaults to first-5.)
3. **Win (Def. 1).** Player n wins iff n's goal area is completely filled
   with pieces (of any color) and at least one of them is n's. A state
   where the *non*-mover's win condition holds is terminal (LOSS for the
   mover): the winner "must make the last move" (2022 paper). Verified by
   the exact part-1 illegal counts and full-solve matches below.
4. **Illegal part 1 (Def. 2).** A state where the *mover's* win condition
   holds is illegal, and moves leading to illegal states do not exist.
   This reproduces the illegal counts of all p ≤ 4 games exactly
   (96 / 10,810 / 576,840 / 20,561,310).
5. **Illegal part 2 (Def. 3, 6 pieces only).** Implemented as: a goal is
   blocked iff its tip cell is empty and all four outer-edge cells of the
   goal triangle (the two cells adjacent to the tip and the two outer-row
   corner cells) are occupied by the *goal owner's opponent*; a state is
   illegal (for either side to move) if either goal is blocked. This is
   exactly Figure 4(c), and it is provably the *complete* Def. 3 for six
   pieces under this move geometry: with only 6 opponent pieces the tip is
   the only goal cell that can be made permanently unreachable (blocking
   any other cell needs ≥ 8 blockers). Part 2 is applied only for p=6
   (paper: "these rules are only necessary … with more pieces"; the p ≤ 4
   illegal counts confirm no part-2 states there). This rule gives
   **exactly** the published 4×4/6 illegal count (405,420) but *not* the
   5×5/6 one — see "Known discrepancy" below.
6. **Stuck states** (mover has no legal move, including "all moves lead to
   illegal states"): LOSS for the mover (normal-play convention, stated in
   the 2017 paper). No such state exists in any game solved here (counted:
   zero in all runs).
7. **Draws (Def. 4).** States unproven at the retrograde fixpoint are
   draws. This matches the paper's own methodology statement, reproduces
   the exact published draw counts for 7×7/3 and 7×7/4, and classifies the
   paper's concrete drawn example (Fig. 5a, a 7×7/3 position) as a draw.
8. **Moves.** Steps to the six adjacent cells and chained hops over single
   adjacent pieces (any color), any chain length, stop anywhere; a chain
   may cross its vacated origin but a move must end on a different cell
   (no null moves). Pieces may leave any area (goal included) and may
   re-enter their own start area (only the suicidal completion is excluded
   — by rule 4 automatically). Movement through the star's other corners
   does not exist on the m×m gameplay area (the paper's footnote 1 states
   their two-player implementation does not allow it; even boards have no
   such corners at all).

## Validation results

| Game | Positions | Wins | Draws | Illegal | vs. paper Table 1 |
|---|---|---|---|---|---|
| 7×7, 1 pc | 4,704 | 2,304 | 0 | 96 | **exact** |
| 7×7, 2 pc | 2,542,512 | 1,265,851 | 0 | 10,810 | **exact** |
| 7×7, 3 pc | 559,352,640 | 279,297,470 | 180,860 | 576,840 | **exact** |
| 7×7, 4 pc (rhombus) | 63,136,929,240 | 31,532,340,944 | 51,686,042 | 20,561,310 | **exact** |
| 4×4, 6 pc | 3,363,360 | 1,436,159 | 85,622 | 405,420 | illegal exact; wins/draws differ (paper: 1,205,441 / 547,058) |
| 5×5, 6 pc | 9,610,154,400 | 4,749,512,556 | 46,956,270 | 64,173,018 | close but not exact (paper: 4,749,618,788 / 47,056,118 / 63,860,706) |
| 6×6, 6 pc | 2,313,100,389,600 | 1,152,969,765,114 | 5,181,409,122 | 1,979,450,250 | close but not exact (paper: 1,153,000,938,173 / 5,199,820,604 / 1,898,692,650) — see RESULTS.md |

("Wins" uses the paper's convention: the number of positions in the full
state space that are a win for the *first* player — equal to the number of
canonical win-or-loss states, since wins = losses by the color symmetry.)

Additional validation beyond Table 1:
* An independent brute-force solver (separate board representation, move
  generator, ranking; no symmetry) agrees with the optimized solver on
  **every state** of 4×4/6 (3,363,360 states) and 7×7/2 (2,542,512
  states).
* `query --verify` random-sample consistency checks pass on every solved
  table (each sampled WIN has a losing successor; each LOSS has only
  winning successors; each DRAW has no losing successor and a drawn one).
* All solved games are first-player wins from the initial position, as the
  paper states.
* Interrupted-and-resumed runs produce bit-identical results.

## Known discrepancy on the 6-piece rows (documented, unresolved)

With the rules exactly as published, the 6-piece games do not reproduce
the paper's counts, while every p ≤ 4 row matches exactly. The evidence:

* My part-2 illegal rule ("tip empty + 4 edge cells opponent-occupied")
  matches the published 4×4/6 illegal count *exactly* (405,420), but gives
  64,173,018 on 5×5/6 vs the published 63,860,706 (+0.49%), and
  ~2.009×10⁹ on 6×6/6 vs the published 1,898,692,650 (+5.8%).
* The published 5×5 count cannot be produced by *any* rule of the form
  "tip empty + k opponent-occupied cells near the corner (+ forced-empty /
  own-cell side conditions)": an exhaustive sweep of ~200 such rules shows
  they are all count-equivalent on both boards (position-blind
  combinatorics) or miss both targets. Whatever their implementation
  checks, it is not a static local pattern of this family — yet on 4×4 it
  has exactly the same *size* as mine while (per the draw counts)
  different *membership*.
* The stated 6-piece check ("two outer edges occupied, tip unoccupied")
  is provably the complete Def. 3 for 6 pieces (see rule 5), so the
  paper's implementation deviates from the paper's own stated rule in
  some unpublished detail; the 2019 paper itself notes the possibility of
  missing cases and that the 6×6 win count "should not be considered
  correct until … verification has been completed", verification which
  (to my knowledge) was never published.
* Dynamic rule variants were also tested against the 4×4/6 published
  wins/draws (547,058 draws): null moves (hop cycles as passes),
  unconditional passing, no-goal-exit, no-start-re-entry, part-2 states
  as unprovable-but-reachable successors, origin-occupied hop chains, and
  combinations — the closest reaches 197,510 draws. None comes near;
  meanwhile the same dynamics reproduce all p ≤ 4 rows exactly.

Consequently this solver treats the **published rules as normative** and
reports its own counts for the 6-piece games, with the paper's numbers
quoted alongside. All machinery needed to explore alternative rules is
included (`rulecount`, brute-force variant flags).

## ZDD evaluation (decision: not used)

`zddbench` builds ZDDs (hash-consed, zero-suppressed, one per WDL class,
variables = per-cell-per-player occupancy) from fully solved tables:

| Game | dense 2-bit | ZDD (12 B/node, packed) | ratio | query time |
|---|---|---|---|---|
| 4×4/6 (853,020 states) | 0.21 MB | 1.0 MB | 4.9× larger | 30 ns vs 449 ns |
| 7×7/2 (1,271,256 states) | 0.32 MB | 0.8 MB | 2.5× larger | 8 ns vs 621 ns |

The solved classes are near-incompressible for decision diagrams (~0.6–1.4
ZDD nodes' worth of storage per state vs 0.25 bytes/state dense), queries
are 15–75× slower, and single-threaded construction alone took 9–14 s per
~1M states — extrapolating to weeks for the 6×6 game before any solving
work. ZDDs show no advantage on any axis, so the solver uses the dense
2-bit table with perfect ranking. (Caveat: the benchmark builds ZDDs of
final solved sets; a fully symbolic solver could in principle behave
differently, but the near-random structure of the win/loss partition —
visible in the per-state node densities — makes symbolic blowup a
certainty rather than a risk.)

## 6×6 results

Solved to the fixpoint in 24 passes / 19.8 h on 64 threads (144.7 GB
table in RAM). **The initial position is a first-player win.** Full
counts, the comparison against the paper, and solve statistics are in
`RESULTS.md`; the raw log is `runs/m6p6/log.txt`, and the finished table
(`runs/m6p6/table.bin`) can be queried with `src/query`.

## Scaling to 7×7 with 6 pieces (not attempted — estimate only)

The code is fully configurable for `--m 7 --p 6` (geometry, areas, part-2
masks, and both symmetries are generic), but the resources exceed this
machine:

* Positions: 170,503,381,976,928. Canonical (color symmetry):
  85,251,690,988,464; with left-right reduction: 42,646,584,632,446
  states ≈ **10.7 TB at 2 bits/state** (this machine: 723 GB RAM, 4.1 TB
  free disk — insufficient even for one copy on disk).
* Work scales ≈74× over the 6×6 solve (same per-state cost), i.e. months
  of CPU on this hardware, plus the table no longer fits in RAM, so the
  solver needs the external-memory extension the paper alludes to
  (structured-duplicate-detection-style block scheduling; the b-major
  group structure here is the right unit: successor locality stays intact,
  but self-value updates must be turned into sorted delta streams merged
  per pass).
* Recommendation: ≥16 TB of fast NVMe (two table copies + checkpoints),
  ≥256 GB RAM for group caching, and expect ~10–30 full-table passes.

Do not start it casually: at 60 MB/s of pass-1 proof progress observed on
6×6, a straightforward port would take on the order of **2–4 months**.

## Repository layout

```
src/cc_core.hpp      geometry, rules, ranking, index scheme, 2-bit table
src/solver.cpp       optimized parallel solver (init/passes/backprop/checkpoints)
src/bruteforce.cpp   independent reference solver + rule variants
src/query.cpp        query / stats / verification sampler
src/crosscheck.cpp   full per-state comparison brute-force vs optimized
src/rulecount.cpp    static illegal-pattern counter (rule investigation)
src/zddbench.cpp     ZDD vs dense benchmark
tests/unit_tests.cpp unit tests
checksums/           SHA-256 manifest for the externally published 6x6 table
runs/                solve outputs (table.bin, groups.bin, meta.txt, log.txt) — gitignored
RESULTS.md           the 6x6 solve: counts, paper comparison, statistics
SOLUTION-6x6.md      narrative walkthrough of the 6x6 solve
```

Compiled binaries, `runs/`, and the source paper are excluded by `.gitignore`; the papers
cited above are not redistributed here.
