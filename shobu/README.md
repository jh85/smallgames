# Mini-Shobu strong solver — retrograde WDL tables for 2-board Shobu variants

[Shobu](https://www.smirkandlaughter.com/shobu) is a two-player abstract game played on
four 4×4 boards with 4 stones per color per board. A turn is a *passive* move (one of your
stones on one of your two home boards, 1–2 squares queen-wise, pushing nothing) followed by
an *aggressive* move (same heading, on a board of the other color, may push at most one
enemy stone by the same heading vector; a stone pushed off the board is captured). You win
by eliminating all enemy stones from any one board.

Full Shobu is far beyond this explicit solver: stones never migrate between boards, so the state
space is an exact product of per-board configurations — ≈ 2.27×10⁶ per board, i.e.
**≈ 2.6×10²⁵ positions** (about 50,000× checkers). This repo therefore solves
**Mini-Shobu**, the smallest variant that preserves the full rule set: **2 boards** (one
home board per player) of **W×H** squares with **K** stones per color per board on the back
rows. Black moves first; a player with no legal turn loses (such stalemates exist and are
counted in the summaries).

All results use exhaustive retrograde analysis over the full state space or its exact
symmetry quotient (no ZDD needed at these sizes; see *Design*), with rules validated
against a Java reference engine (see *Validation*).

## Results (solved + validated through 2026-10-14)

The 4×4, K=2 result is the first draw among the solved starting positions; the other
seven are **first-player (Black) wins**. The three largest results are strong WDL
solutions without distance-to-mate or a reachable-state pass.

| Variant  | Mathematical states | Reachable from start | Initial value | WDL artifact | Size | SHA-256 |
|----------|--------------------:|---------------------:|---------------|--------------|-----:|---------|
| 3×3, K=2 |             4,049,858 |            1,963,316 | **Win in 11** | `mini3x2.wdl` | 12 MB | `940493a7…` |
| 3×3, K=3 |            92,452,802 |           22,439,236 | **Win in 17** | `mini3x3.wdl` | 277 MB | `489df36c…` |
| 4×3, K=3 |         3,749,606,802 |        1,608,108,724 | **Win in 13** | `mini4x3.wdl` | 11 GB | `a4c52049…` |
| 3×4, K=3 |         3,749,606,802 |        1,608,108,724 | **Win in 17** | `mini3x4.wdl` | 11 GB | `92359725…` |
| 4×4, K=2 |           437,665,698 |          318,765,768 | **Draw** | `mini4x4_k2_sym.wdl` | 82 MB | `6a7216d…` |
| 4×3, K=4 |        57,812,920,722 |                    — | **Win (WDL)** | 17 packed layers | 1.81 GB | [manifest](checksums/minishobu_4x3_k4_layered.SHA256SUMS) |
| 4×4, K=3 |       154,560,216,098 |                    — | **Win (WDL)** | 13 packed layers | 2.42 GB | [manifest](checksums/minishobu_4x4_k3_layered.SHA256SUMS) |
| 4×4, K=4 |    10,280,872,332,018 |                    — | **Win (WDL)** | 17 packed layers | 160.64 GB | [manifest](checksums/minishobu_4x4_k4_layered.SHA256SUMS) |

Full SHA-256 sums in [checksums/](checksums/). Per-variant statistics (W/D/L splits,
max depths, stalemate counts) in [summaries/](summaries/).

* Per-board configuration counts: 1,423 (3×3 K=2), 6,799 (3×3 K=3), 43,299 (12-square
  K=3), and 14,793 (4×4 K=2). Total mathematical space = 2 × (per-board)².
* 4×3 and 3×4 K=3 share one isomorphic reachable universe (their initial positions are
  mutually reachable via a 90° rotation — the rotated 4×3 start sits in the 3×4 table with
  the same value, Win in 13), hence identical aggregate W/D/L statistics.
* Draws are genuine (cycles under optimal play): ~10–14% of reachable positions.
* Longest forced win: 99 plies (12-square variants).
* The 4×4 result uses the symmetry solver below: 437,665,698 mathematical states become
  27,368,761 orbits (15.991× reduction). Its 19,929,651 reachable orbits represent the
  318,765,768 reachable positions in the table above.

## Symmetry-reduced solver

The `*-sym` modes quotient two exact automorphisms while retaining a dense perfect index:

1. If Black is to move, swap the players, their colors, and their home boards. This
   normalizes every state to the current player being White and removes the turn bit.
2. Apply the same board geometry to both boards: the four-element rectangle group for
   non-square boards, or all eight square symmetries for square boards.

For a pair of board ranks `(a,b)`, the ranker first chooses the canonical `a`. It then
canonicalizes `b` only under the stabilizer of `a`. Prefix sums over those stabilizers
give a collision-free contiguous orbit rank; no hash table or representative list over
the product space is needed. Symmetric successor and predecessor ids are deduplicated
before retrograde counters are updated.

Exact sizes for the now-solved larger targets are:

| Variant | Mathematical states | Symmetry orbits | Reduction | Monolithic arrays |
|---|---:|---:|---:|---:|
| 4×3, K=4 | 57,812,920,722 | 7,227,534,041 | 7.999× | 36.1 GB |
| 4×4, K=3 | 154,560,216,098 | 9,660,420,637 | 15.999× | 48.3 GB |
| 4×4, K=4 | 10,280,872,332,018 | 642,560,395,977 | 16.000× | 3.21 TB |

### Material-layered WDL solver

`solve-layered-sym` implements the next architectural step. A Shobu turn either preserves
the total number of stones or captures exactly one, so every edge from material layer `T`
ends in `T` or `T-1`. The solver processes layers in increasing material order: it holds
the current layer's one-byte value and two-byte residual out-degree arrays, reads the
finished lower layer in packed form, and performs retrograde propagation only within the
current layer. Each completed layer is Bellman-verified, packed to two bits per orbit, and
published by atomic rename before its work arrays are released. Re-running the same command
automatically resumes from those completed layer checkpoints. Layer-local ids also let the
live queue use 32-bit entries whenever possible.

This is an exact strong WDL solution over the full state universe, but deliberately omits
distance-to-mate and the final reachable/unreachable marking. Those require separate
passes; they are not needed to establish the starting position's game-theoretic value.

| Variant | Monolithic 5-byte arrays | Peak layer | Layered 3-byte arrays | Core reduction |
|---|---:|---:|---:|---:|
| 4×4, K=2 | 136.8 MB | 14,909,892 | 44.7 MB | 3.06× |
| 4×3, K=3 | 2.34 GB | 146,381,312 | 439 MB | 5.34× |
| 4×3, K=4 | 36.14 GB | 1,692,451,008 | 5.08 GB | 7.12× |
| 4×4, K=3 | 48.30 GB | 3,497,982,528 | 10.49 GB | 4.60× |

The queue and symmetry-index metadata are additional, so the last column is not a peak-RSS
claim. The live queue costs four bytes per id for all layers in these variants. As an
end-to-end check, the new solver completed 4×4 K=2 in 43.6 seconds here, returned **Draw**,
Bellman-verified all 27,368,761 orbits, and matched all 19,929,651 reachable entries in the
existing independently verified table with zero discrepancies. This makes 4×3 K=4 a
realistic memory target; it was subsequently solved above.

`solve-parallel-sym` is the large-layer engine. It reduces the residual out-degree to one
byte, replaces the potentially enormous id queue with two one-bit frontier maps, and runs
bulk-synchronous retrograde waves across all OpenMP threads. Each wave propagates losing
successors first, then winning successors after a barrier, so a predecessor win always
takes precedence over residual exhaustion. Atomic compare/exchange protects both value
publication and residual decrements.

The in-memory cost is 2.25 bytes/orbit plus the packed previous layer. On 4×4 K=4, the
180,360,853,200-orbit peak layer therefore needs about 406 GB for active arrays and 43 GB
for the preceding packed layer, versus an unsafe 541 GB plus a potentially terabyte-scale
queue in the serial engine. Its complete packed output is about 161 GB. Three independent
64-thread 4×4 K=2 runs produced byte-identical layers to the serial solver; each also
matched all 19,929,651 reachable entries in the older table. A parallel run took 6.8 seconds
with 32 threads versus 43.1 seconds for the serial layered solver on this machine.

The production runs completed in about 54 minutes for 4×3 K=4 (including the initial
serial layers before switching engines), 45.5 minutes for 4×4 K=3, and 72.96 hours for
4×4 K=4. Every one of their 47 material layers passed its full Bellman check before being
atomically published. A post-run audit re-read all headers and lengths, then generated and
independently rechecked SHA-256 hashes for all 164.9 GB of layer files.

## Validation

* **Rules fidelity**: `replay_validate.py` re-implements the move/win rules of the
  reference Java engine ([Shobu AI Playground](https://github.com/JayWalker512/Shobu))
  from scratch and replays all **104,396** recorded games: every intermediate board
  matches exactly; only 6 files (0.006%) carry a recorded winner the rules never produce
  (reproduced against the real engine via its JSON pass-through mode — dataset noise).
* **Move generation**: per-depth reachability counts from the C++ solver match an
  independent Python reference (`minishobu.py`) exactly, for every geometry
  (e.g. 4×3: 26 / 500 / 5,463 / 56,508 / 435,851).
* **Table consistency**: `verify` re-checks the local WDL condition of **every
  reachable state** (win ⇔ ∃ losing successor; loss ⇔ all successors winning; draw ⇔
  neither) plus distance-to-terminal bookkeeping — **0 errors** on all four legacy tables
  (1.6 B states each for the 12-square tables).
* **Symmetry quotient**: exhaustive rank/unrank and automorphism checks pass; all
  1,963,316 reachable 3×3 K=2 entries match the legacy table in both WDL and depth.
  The new 4×4 K=2 table independently passes the same local verifier on all
  19,929,651 reachable orbits (0 errors).
* **Parallel layered tables**: three repeated 64-thread 4×4 K=2 solves were byte-identical
  to the serial layered result and matched the older table on every reachable orbit. The
  4×3 K=4, 4×4 K=3, and 4×4 K=4 production runs Bellman-verified every orbit in every
  layer; their W/D/L totals sum exactly to 7,227,534,041, 9,660,420,637, and
  642,560,395,977 symmetry orbits respectively.

## Design

* **State**: 2 bits/square × 2 boards + side to move in an `unsigned __int128`; the
  widened transient representation is required by 4×4 (65 bits). Dense ids remain 64-bit.
* **Indexing**: combinatorial (colex) ranking of each board config; state id =
  `(rank(board0) · MAXB + rank(board1)) · 2 + turn`. No hash tables; the whole product
  space is addressable as dense arrays (value 1 B + depth 2 B + out-degree 2 B per state).
* **Solve**: seed terminals and stalemates as losses, then queue-based retrograde
  propagation with reverse move generation; every reverse candidate is re-verified by
  forward application (catches capture/un-push corner cases), and predecessor ids are
  deduplicated per successor before out-degree decrements. The queue periodically drops
  its consumed prefix, so memory follows the live frontier rather than all resolved states.
  States unresolved at fixpoint are draws. Unreachable states are marked in a final BFS
  and stored as `UNREACHED`.
* **Why no ZDD yet**: symmetry, material streaming, packed output, and bitmap frontiers made
  the 642.6-billion-orbit two-board space tractable while retaining direct indexing. For
  full four-board Shobu, a constant-factor symmetry quotient is still nowhere near enough;
  a successful solve would need genuinely sublinear symbolic sets or a proof-search/tablebase
  hybrid.
* **DFA pilot**: on normalized 3×3 K=2 positions, minimal board-major DFAs used 51,470
  nodes for WIN and 39,650 for LOSS over 2,024,929 legal positions. That is meaningful
  compression, but a sparse encoding of the two DFAs is about 1.65 MB versus 0.5 MB for
  a packed explicit two-bit WDL plane before geometric symmetry. Shobu moves also change
  four to six squares across two coupled boards, producing thousands of fixed move cases.
  DFA set-based retrograde is therefore an experimental second track, not yet a replacement
  for symmetry plus explicit tables.

## Build & usage

```
make                                        # g++ -O3 -march=native -fopenmp
./minishobu count-sym W H K                 # exact orbit and memory counts
./minishobu count-layered-sym W H K         # material distribution + peak core memory
./minishobu solve  W H K outprefix             # full solve -> outprefix.wdl + .summary.txt
./minishobu solve-sym W H K outprefix          # solve the exact symmetry quotient
./minishobu solve-layered-sym W H K prefix     # packed WDL-only layer files
./minishobu solve-parallel-sym W H K prefix    # parallel bitmap-frontier layered WDL
./minishobu compare-layered-sym W H K old.wdl prefix  # cross-check reachable WDL
./minishobu verify W H K file.wdl              # independent consistency check
./minishobu verify-sym W H K file.wdl          # verify a quotient table
./minishobu query  W H K file.wdl <board> w|b  # probe a position + per-move values
./minishobu query-sym W H K file.wdl <board> w|b
./minishobu query-layered-sym W H K prefix <board> w|b
./minishobu selftest W H K                     # rank bijection + movegen cross-check
./minishobu selftest-sym W H K                 # also check symmetry invariance
```

Probe board string: board 0 then board 1, row-major from White's side, `o` = white,
`x` = black, `.` = empty. Example (4×3 initial position):

```
./minishobu query 4 3 3 mini4x3.wdl "ooo.....xxxooo.....xxx" b
```

## Generating the WDL tables

The tables are **not** stored in this repository (two of them are 11 GB each); only their
SHA-256 sums are. Regenerate with:

```
./minishobu solve 3 3 2 mini3x2     # seconds
./minishobu solve 3 3 3 mini3x3     # ~3 min
./minishobu solve 4 3 3 mini4x3     # ~4 h, ~35 GB RAM
./minishobu solve 3 4 3 mini3x4     # ~4 h, ~35 GB RAM
./minishobu solve-sym 4 4 2 mini4x4_k2_sym  # ~100 s, <0.4 GB RSS here
./minishobu solve-layered-sym 4 4 2 mini4x4_k2_layers # ~44 s here, WDL only
./minishobu solve-parallel-sym 4 3 4 mini4x3_k4_layers # ~54 min incl. early serial work
./minishobu solve-parallel-sym 4 4 3 mini4x4_k3_layers # 45.5 min
./minishobu solve-parallel-sym 4 4 4 mini4x4_k4_layers # 72.96 h
```

Timings are from this 64-core EPYC-class machine. `solve-parallel-sym` parallelizes
enumeration, retrograde propagation, and verification with OpenMP.

## Files

* `solver.cpp` — the solver (solve / verify / query / selftest).
* `minishobu.py` — independent Python reference engine (perft, random playouts).
* `replay_validate.py` — replays the 104 k-game dataset against the rule reimplementation.
* `statespace.py` — state-space size estimates (full Shobu and variants).
* `checksums/` — SHA-256 sums of the WDL tables.
* `summaries/` — per-variant solve statistics.
