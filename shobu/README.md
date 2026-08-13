# Mini-Shobu strong solver — retrograde WDL tables for 2-board Shobu variants

[Shobu](https://www.smirkandlaughter.com/shobu) is a two-player abstract game played on
four 4×4 boards with 4 stones per color per board. A turn is a *passive* move (one of your
stones on one of your two home boards, 1–2 squares queen-wise, pushing nothing) followed by
an *aggressive* move (same heading, on a board of the other color, may push at most one
enemy stone by the same heading vector; a stone pushed off the board is captured). You win
by eliminating all enemy stones from any one board.

Full Shobu cannot be strongly solved: stones never migrate between boards, so the state
space is an exact product of per-board configurations — ≈ 2.27×10⁶ per board, i.e.
**≈ 2.6×10²⁵ positions** (about 50,000× checkers). This repo therefore solves
**Mini-Shobu**, the smallest variant that preserves the full rule set: **2 boards** (one
home board per player) of **W×H** squares with **K** stones per color per board on the back
rows. Black moves first; a player with no legal turn loses (such stalemates exist and are
counted in the summaries).

All results produced by exhaustive retrograde analysis over the full product state space
(no ZDD needed at these sizes; see *Design*), rules validated against a Java reference
engine (see *Validation*).

## Results (solved + validated 2026-08-13)

Every solved variant is a **first-player (Black) win**.

| Variant  | State space (product) | Reachable from start | Initial value | WDL file | Size | SHA-256 |
|----------|----------------------:|---------------------:|---------------|----------|-----:|---------|
| 3×3, K=2 |             4,049,858 |            1,963,316 | **Win in 11** | `mini3x2.wdl` | 12 MB | `940493a7…` |
| 3×3, K=3 |            92,452,802 |           22,439,236 | **Win in 17** | `mini3x3.wdl` | 277 MB | `489df36c…` |
| 4×3, K=3 |         3,749,606,802 |        1,608,108,724 | **Win in 13** | `mini4x3.wdl` | 11 GB | `a4c52049…` |
| 3×4, K=3 |         3,749,606,802 |        1,608,108,724 | **Win in 17** | `mini3x4.wdl` | 11 GB | `92359725…` |

Full SHA-256 sums in [checksums/](checksums/). Per-variant statistics (W/D/L splits,
max depths, stalemate counts) in [summaries/](summaries/).

* Per-board configuration counts: 1,423 (3×3 K=2), 6,799 (3×3 K=3), 43,299 (12-square
  K=3). Total space = 2 × (per-board)² (two boards × side to move).
* 4×3 and 3×4 K=3 share one isomorphic reachable universe (their initial positions are
  mutually reachable via a 90° rotation — the rotated 4×3 start sits in the 3×4 table with
  the same value, Win in 13), hence identical aggregate W/D/L statistics.
* Draws are genuine (cycles under optimal play): ~10–14% of reachable positions.
* Longest forced win: 99 plies (12-square variants).
* 4×3 with **K=4** (full back rows) is 5.8×10¹⁰ states — needs ≳ 0.5 TB RAM with this
  dense-table approach; implemented but stage-gated as infeasible on one 128 GB machine.

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
  neither) plus distance-to-terminal bookkeeping — **0 errors** on all four tables
  (1.6 B states each for the 12-square tables).

## Design

* **State**: 2 bits/square × 2 boards + side to move in a `uint64` (≤ 12-square boards).
* **Indexing**: combinatorial (colex) ranking of each board config; state id =
  `(rank(board0) · MAXB + rank(board1)) · 2 + turn`. No hash tables; the whole product
  space is addressable as dense arrays (value 1 B + depth 2 B + out-degree 2 B per state).
* **Solve**: seed terminals and stalemates as losses, then queue-based retrograde
  propagation with reverse move generation; every reverse candidate is re-verified by
  forward application (catches capture/un-push corner cases), and predecessor ids are
  deduplicated per successor before out-degree decrements. States unresolved at fixpoint
  are draws. Unreachable states are marked in a final BFS and stored as `UNREACHED`.
* **Why no ZDD**: the solved spaces (≤ 3.75×10⁹ states) fit in RAM as flat arrays, and the
  retrograde fixpoint touches essentially every state — a decision diagram cannot compress
  per-state values that dense. (For full Shobu, ≈ 10²⁵ states, neither approach helps.)

## Build & usage

```
make                                        # g++ -O3 -march=native -fopenmp
./minishobu solve  W H K outprefix             # full solve -> outprefix.wdl + .summary.txt
./minishobu verify W H K file.wdl              # independent consistency check
./minishobu query  W H K file.wdl <board> w|b  # probe a position + per-move values
./minishobu selftest W H K                     # rank bijection + movegen cross-check
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
```

Timings from a 64-core EPYC-class machine (retrograde propagation is single-threaded;
state enumeration/verification use OpenMP).

## Files

* `solver.cpp` — the solver (solve / verify / query / selftest).
* `minishobu.py` — independent Python reference engine (perft, random playouts).
* `replay_validate.py` — replays the 104 k-game dataset against the rule reimplementation.
* `statespace.py` — state-space size estimates (full Shobu and variants).
* `checksums/` — SHA-256 sums of the WDL tables.
* `summaries/` — per-variant solve statistics.
