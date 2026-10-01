# Oware 6×2

A C++20 retrograde solver for Oware (Abapa-style rules as implemented by OpenSpiel v1.5 and
used by Neumann & Gros, arXiv:2412.11979), covering all **889,063,398,405** positions that
can arise after a move (Romein & Bal's 889,063,398,406 minus the initial position, which is
resolved by a one-ply lookup). The rules solved are specified in
[oware_6x2_paper_rules.md](oware_6x2_paper_rules.md). The seed count is a run-time parameter
and the row length a compile-time one, so the same solver also handles the 36-seed (3 per
pit) game and 7×2 boards; see [Results](#results).

## What the table means — read this first

Oware under these rules is **not** a game whose value is a function of the board alone.
A repeated position ends the game and each player collects the seeds in their own row, so the
result of a loop depends on *which* position happens to repeat first, i.e. on the history.
Small-layer experiments (`src/explore.cpp`, `src/explore2.cpp`) show this is not a corner
case: with 10 or more seeds on the board, 60–95 % of boards have a score that forced,
self-terminating play only pins down to within a few seeds, and the exact history-dependent
value is exponential to compute (brute force is already impractical at 4 seeds). This is the
obstacle Blanvillain (2012) describes as "Oware will not be solved by computers".

The table therefore stores, for every board, the two quantities that *are* well defined
without any convention:

* `gM` — the number of remaining seeds the **side to move can force**, and
* `gN` — the number the **other side can force**,

where "force" means by play that ends on its own (someone reaches 25, or a no-legal-move
sweep) along a line that never repeats a position. `gM + gN <= n`; equality means the board's
score is exact. From these, every state (board, captured scores, side to move) gets one of:

| Value | Meaning |
| --- | --- |
| `WIN` / `LOSS` | Forced under the real rules, starting from an empty repetition history. The winning line never repeats a position, so the repetition rule cannot interfere. |
| `DRAW` | Both sides can force exactly 24. |
| `DRAW*` | Neither side can force a win by self-terminating play. Under the real rules the game would be decided by the repetition sweep (or the 1,000-ply cap), which depends on history; the table does not adjudicate it. |

This is the same convention as chess or Nine Men's Morris tablebases (a cycle is nobody's
forced win), stated explicitly because rules section 11 forbids adopting it silently.

Further caveats:

* **1,000-ply cap.** Not modelled. A `WIN` is a forced win of the uncapped game; the solver
  does not track distance-to-win, so it does not certify that a particular win fits inside
  the remaining ply allowance of a particular trajectory.
* **History.** `WIN`/`LOSS` hold for a fresh history. With a non-empty history the forcing
  line could run into an already-visited position. Any capture clears the history.
* The table certifies values, not a policy: repeatedly choosing *any* value-preserving move
  can walk in circles. Turning a `WIN` into play needs a short search for a line that makes
  progress.

## Method

**Layers.** Captured seeds never return, so positions are layered by the number `n` of seeds
on the board (n = 0…46 and 48; 47 is impossible). A capture leads to a smaller layer. Layers
are solved in increasing `n`, all in RAM.

**Scores, not per-score-split WDL.** With `c` seeds already captured by the mover, the mover
wins iff `c + gM >= 25`. One pair `(gM, gN)` per board therefore answers every score split,
which replaces 5.1 × 10¹² (board, score) states by 8.9 × 10¹¹ boards. For n > 24 only
thresholds that some legal score split can ask about matter, so the pair is clamped to
`51 - n` levels (3 levels at n = 48) and stored as a triangular index, 1/3 byte to 2 bytes
per board.

**Fixpoint inside a layer.** `gM(b) = max_moves (captured + gN(child))`,
`gN(b) = min_moves gM(child)`, least fixpoint from 0. Two methods, `fix=counter` (default)
and `fix=sweep`:

* *counter* (retrograde analysis with counters): the clamped thresholds t = V−1 … 1 are
  processed from high to low, each as the boolean game W^t = {gM ≥ t}, N^t = {gN ≥ t}.
  Every board keeps a 4-bit counter of its non-capturing children not yet in W, carried
  over between thresholds (W^t ⊇ W^(t+1)). A board enters N^t when its counter reaches
  zero and its capture ceiling allows t; its same-layer predecessors (incremental
  un-sowing, O(1) per extra seed) then enter W^t, their predecessors' counters drop, and so
  on in generations. Each board enters W and N exactly once, so the whole layer costs two
  visits of every same-layer edge whatever V is. Measured on 7x2/28 layer 28 (16.7 × 10⁹
  boards, 32 threads, same machine load): 584 s against 2718 s for the sweeps, with
  bit-identical output. The number of generations is the longest forced line inside the
  layer (171 plies at the top threshold of that layer). Costs ~1 extra byte per board
  (counters plus four two-level frontier bitmaps); `fix=auto` falls back to sweeps when
  that does not fit the memory budget, which is the case for the 7×2/42 layers above ~36
  on this machine. Validation: full solves of 7×2/28 (ram and stream) and 6×2/36 with
  `fix=counter` reproduce every layer file of the sweep solves byte for byte; 6×2/36 layers
  34 and 36 took 324 s and 379 s (32 threads, machine shared) against 943 s and 1150 s
  (64 threads, idle machine) with sweeps.
* *sweep* (the original method): chaotic iteration driven by a dirty bitmap; when a
  board's value rises, its same-layer predecessors are generated by un-sowing (and confirmed
  by replaying the move forward) and marked dirty. Forced lines inside a layer are 80–170
  plies long, so this takes 60–90 sweeps per layer.

In both methods a final full pass re-evaluates every inexact board (and a sample of exact
ones) and aborts unless the layer is a fixpoint; values only rise from a sound start, so a
fixpoint reached this way is the least one.

**Index (ZDD-style perfect hash).** Following Takeda & Hoki's first ZDD, positions are ranked
by a layered decision diagram over the pits with node label *(pit, seeds left to place, an
empty opponent pit was seen)*, each node storing its number of accepting paths; a position's
index is the sum of the path counts of the branches it skips. The constraint "the opponent's
row has an empty pit" holds after every move (the pit just sown from is empty and skipped) and
is what reduces 1.40 × 10¹² raw boards to the 889 billion indexed ones. Because the two rows
interact only through their sums the diagram factors into two six-pit diagrams, so ranking is
twelve table lookups. Unlike N Men's Morris there is no board symmetry to quotient out
(normalising the side to move already uses the only one), so a second ZDD is not needed.

## Build and run

```sh
make            # build/solve, build/probe, build/tests, build/xcheck, build/explore*
                # and the 7x2 builds build/solve7, probe7, tests7, xcheck7 (OWARE_ROW=7)
make test
numactl --interleave=all build/solve <outdir> [seeds=48] [threads=all] [maxN=seeds] [mode=auto|ram|stream] [fix=auto|counter|sweep]
build/probe <outdir> s0 s1 ... s11 captured0 captured1 side_to_move
```

`solve` needs about 600 GB of RAM for the full 48-seed run and writes one `oware_nNN.lh`
file per layer plus `stats.txt` (per layer and score split: win / loss / forced draw /
`DRAW*` counts). It restarts from the finished layers found in `<outdir>`.

`seeds` is the total number of seeds (even, at most 48): 48 is standard Oware with 4 seeds
per pit, 36 is the 3-per-pit variant, and so on. The rules are otherwise unchanged and a
majority (`seeds/2 + 1`) of the seeds wins; the value clamp and the layer skipped for "a
single seed can never be captured" follow from `seeds`. Each layer file records the seed
count in its header (files from before this field was added are 48-seed files), so a table
cannot be read as the wrong game. Use a separate `<outdir>` per variant.

The number of pits per player is the compile-time constant `OWARE_ROW` (default 6); the
Makefile also builds every tool with `OWARE_ROW=7` under a `7` suffix (`build/solve7 <outdir>
28` solves 7×2 with 2 seeds per pit). Sowing, the skipped origin pit, captures and the index
generalise unchanged; `MAX_SEEDS` is 6 per pit for wider boards.

`probe` takes the OpenSpiel pit numbering (P0 owns pits 0–5, P1 owns 6–11; `probe7` takes 14 pit counts), infers the
game's seed count from the state (pits plus captured seeds), memory-maps only the layers it
needs, and prints the value of the position and of every legal move.

### File format

`oware_nNN.lh`: a 56-byte header (`FileHeader` in `src/layer.hpp`) followed by one triangular
pair index per board in index order, packed as base-P digits (3 or 2 boards per byte), as
`w`-bit little-endian bit fields, or as `uint16`, as recorded in the header's `mode`.

## Compact tables: gM-only, block-compressed (`.gmz`)

The `.lh` files store the pair (gM, gN) per board, but gN is redundant for
queries: `gN(b) = min over the legal moves of gM(child)` (a capturing move's
child lies in a lower layer), or the opponent's row sum when the mover has no
legal move. Because every layer's clamp interval contains the intervals of the
layers above it, taking the minimum of the children's clamped gM and re-clamping
gives exactly the stored gN code. `build/gmz` therefore writes a second table
format that keeps only the clamped gM code, one byte per board inside blocks of
65,536 boards, each block compressed independently with zstd (level 19), with a
block offset table at the end of the file so that a probe decompresses a single
64 KB block per lookup:

```sh
make                                       # adds build/gmz, gmz7, probez, probez7
tools/pack_all.sh <lhdir> <seeds> <outdir> row6|row7 [threads]
                                           # pack every layer, then verify every layer
build/gmz pack   <lhdir> <seeds> <n> <outdir> [level=19] [blockBoards=65536] [pack=0] [threads]
build/gmz verify <lhdir> <gmzdir> <seeds> <n> [threads]   # gM equal and gN re-derived equal, every board
build/gmz stats  <lhdir> <seeds> <n>                      # entropy of the gM codes
build/probez <gmzdir> s0 ... s11 captured0 captured1 side_to_move   # same output as probe
```

`verify` checks the stored gM of every board against the `.lh` file, and gN
re-derived from the children against the stored gN for every board when the layers
a probe can reach fit in memory decoded, otherwise for 10 million random boards.
`probez` agrees with `probe` on random states, and `tools/oware_probe.py` (pure
Python, `zstandard` module or the `zstd` command) agrees with `probez`.

`tools/make_package.sh <gmzdir> <seeds> <row> <stats.txt> <outdir>` assembles a
distributable package: `tables/*.gmz`, `SHA256SUMS`, `stats.txt`, the query programs
with their sources and a package `Makefile`, and a README explaining the numbering,
the values and the file format. The three published games were packaged this way
(6.3 GB, 10.1 GB and 203 GB instead of 19.5 GB, 26.9 GB and 554 GB).

Sizes: zstd on the byte-per-board blocks gets below the zeroth-order entropy of
the gM codes (about 3.5 bits per board in the middle layers, 1.4 bits at the
7x2/28 top layer), because neighbouring boards in index order share their
mover row and have correlated values. Measured on the finished tables (every layer verified):

| Game | `.lh` tables | `.gmz` tables | Ratio |
| --- | ---: | ---: | ---: |
| 7×2/28 (28 layers) | 19.5 GB | 6.26 GB | 3.1× |
| 6×2/36 (36 layers) | 26.9 GB | 10.1 GB | 2.7× |
| 6×2/48 (48 layers) | 553.6 GB (516 GiB) | 202.9 GB (189 GiB) | 2.7× |

Per layer the ratio is 2.8–3.0× where the `.lh` file has one byte per board, 5.6–5.9×
where it has two (layers 22–28 of 6×2/48), 3.0× at the 6×2/48 top layer (0.110 bytes per
board against 1/3) and 3.5× at the 7×2/28 top layer; the 7×2/42 table (2.36 TB) should
come out at roughly 850 GB. For 6×2/48 the gM check covered every board of every layer
and the re-derived gN check every board of layers 0–30 and 10 million random boards of
each layer above. The `.gmz` files of the three finished games
are in `/data1/oware2/gmz/`.

## Validation

* `build/tests`: the rule examples of rules section 10, sowing laps, index round trips, closure
  of the indexed set under moves, the published position count 889,063,398,406, and the value
  clamps for 48 and 36 seeds. `build/tests7` checks 7×2 captures and laps and the 7×2/42
  position count 4,161,983,837,529 against the closed form.
* `python tools/openspiel_trace.py 20000 7 | build/xcheck` (needs `pip install open_spiel==1.5`): 2,078,040 transitions from 20,000 random OpenSpiel 1.5
  games replayed through `play()`/`legalMask()` with zero mismatches (this caught a real bug:
  a sowing of exactly 11 or 22 seeds must end on the pit before the origin). For 7×2,
  `openspiel_trace.py 20000 7 7 2 | build/xcheck7` (1,649,966 transitions, 151,619 with
  captures) and `openspiel_trace.py 5000 11 7 3 | build/xcheck7` (546,168 transitions) also
  give zero mismatches.
* `src/explore.cpp` is an independent, single-threaded, unclamped implementation of the same
  fixpoint; it agrees with `solve` on the number of inexact boards in every layer up to 17.
* Clamping: layers 25 and 26 solved with and without clamping give identical statistics.
* Soundness against the real rules: for every inexact board with up to 3 seeds, an exhaustive
  history-aware search of the real game (repetition sweep included) lies inside `[gM, n-gN]`.
* Every layer passes the built-in fixpoint verification before it is written.

## Results

All solved variants at a glance. "Positions" counts the indexed boards — every state
that can arise after a move (for 6×2/48, plus the initial position this is the
889,063,398,406 of Romein & Bal 2003). "Top layer exact" is the share of the
largest layer's boards whose score is pinned down exactly (`gM + gN = n`).

| Board | Seeds | Positions | Layer files | Tables | Top layer exact | Initial position | Solve time |
| --- | --- | ---: | ---: | ---: | ---: | --- | ---: |
| 6×2 | 48 (4/pit) | 889,063,398,405 | 48 | 516 GB | 30.3 % | Draw, no forced win (both sides force ≥ 23) | 17.5 h |
| 6×2 | 36 (3/pit) | 47,581,435,824 | 36 | 26.9 GB | 41.7 % | Draw, no forced win (both sides force ≥ 17) | 36 min |
| 7×2 | 28 (2/pit) | 39,080,213,240 | 28 | 19.5 GB | 54.6 % | Draw, no forced win (first player forces ≥ 13, second ≥ 11) | 31 min |
| 7×2 | 42 (3/pit) | 4,161,983,837,529 | 42 | ~2.36 TB | — | solving in progress | est. 4–8 days |

File counts already skip the one impossible layer in each game (a single seed can
never be captured, so layer `seeds − 1` does not exist: 47, 35, 27 and 41
respectively). The 7×2/42 row will be filled in when its run finishes (top-layer
exact %, verdict, actual time).

### 6×2, 48 seeds (standard Oware)

Full run of layers 0–46 and 48 (64 threads on a 2 × EPYC 9115, peak 566 GB RSS,
17.5 h of solve + verify time): **the initial position is a draw with no forced win.**

```
initial position: first player forces >= 23 (or fewer), second player forces >= 23 (or fewer): DRAW (no forced win)
```

Neither player can force even 24 of the 48 seeds by self-terminating play; every line that
avoids losing runs into a repetition, whose outcome depends on the history (see
[What the table means](#what-the-table-means--read-this-first)). The tables are 516 GB in
48 files (digests in `checksums/`); they are not in git.

Share of boards whose score is exact (`gM + gN = n`, i.e. clamped to the levels that matter):

| n | boards | exact | solve + verify |
| ---: | ---: | ---: | ---: |
| 4 | 1,365 | 72.8 % | |
| 8 | 75,504 | 61.1 % | |
| 12 | 1,339,702 | 18.7 % | |
| 16 | 12,685,179 | 6.5 % | |
| 20 | 80,214,915 | 3.8 % | |
| 24 | 382,628,610 | 2.4 % | |
| 28 | 1,482,519,324 | 1.3 % | |
| 32 | 4,897,012,197 | 1.3 % | 8.5 min |
| 36 | 14,257,671,649 | 3.0 % | 25 min |
| 40 | 37,475,421,060 | 7.0 % | 65 min |
| 44 | 90,517,649,586 | 13.5 % | 124 min |
| 48 | 203,648,015,935 | 30.3 % | 109 min |

An inexact score does not mean an undecided state: the mover still wins whenever
`captured + gM >= 25`. `stats.txt` records win / loss / draw / `DRAW*` counts for every layer
and score split.

### 6×2, 36 seeds (3 per pit)

`build/solve <outdir> 36`: 47,581,435,824 boards in layers 0–34 and 36, 26 GB of tables,
36 min. **Draw with no forced win** as well: neither side can force more than 17 of the 36
seeds (18 are needed for a draw by score, 19 to win). 41.7 % of the boards of the top layer
are exact, against 30.3 % for the 48-seed game.

A plan for the 7×2 game with 42 seeds (3 per pit; 4.16 × 10¹² boards, 2.36 TB of tables,
more than fits in RAM) is in [7x2_42_out_of_core_plan.md](7x2_42_out_of_core_plan.md).

### 7×2, 28 seeds (2 per pit)

`build/solve7 <outdir> 28`: 39,080,213,240 boards in layers 0–26 and 28 (the count matches
the closed form), 19 GB of tables, 31 min. **Draw with no forced win**: the first player can
force at least 13 of the 28 seeds and the second at least 11 (15 win, 14–14 draws). Unlike
the 6×2 games the opening already matters: `probe7` reports that three of the seven first
moves (pits 1, 2 and 3) lose by force, pit 6 captures 6 seeds at once, and the other three
keep the draw. 54.6 % of the top layer's boards are exact.

SHA-256 digests of all three table sets (48 layer files + `stats.txt` each) are in
[checksums/](checksums/).

## References

* O. Neumann, C. Gros, *AlphaZero Neural Scaling and Zipf's Law*, arXiv:2412.11979 (rules).
* J. W. Romein, H. E. Bal, *Solving the Game of Awari using Parallel Retrograde Analysis*,
  IEEE Computer 36(10), 2003 (position count).
* X. Blanvillain, *Oware: The Oldest Game of the World Will Not Be Solved by Computers*, 2012.
* D. Takeda, K. Hoki, *Analysis of the Number of Piece Configurations in N Men's Morris*,
  IPSJ SIG-GI 2020 (ZDD perfect hash).
