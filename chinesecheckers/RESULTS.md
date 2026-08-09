# 6×6 Chinese Checkers, 6 pieces per player — complete WDL solve

Solved 2026-08-05 on a 2×16-core AMD EPYC 9115 (64 threads), 723 GB RAM.
Rules exactly as published in Sturtevant, *On Strongly Solving Chinese
Checkers* (ACG 2019) — see README.md for the full rule statement and the
documented 6-piece rule discrepancy.

## Headline result

**The initial position is a win for the first player**, in agreement with
the paper ("All games that we have solved have been a first-player win").

## Counts (full state space, both sides to move, ignoring symmetry)

| Quantity | This solve | Paper (Table 1) | Difference |
|---|---:|---:|---:|
| Positions | 2,313,100,389,600 | 2,313,100,389,600 | 0 |
| Wins (first player) | 1,152,969,765,114 | 1,153,000,938,173 | −31,173,059 (−0.0027%) |
| Losses (second player wins) | 1,152,969,765,114 | 1,153,000,938,173 | (= wins, color symmetry) |
| Draws | 5,181,409,122 | 5,199,820,604 | −18,411,482 (−0.35%) |
| Illegal | 1,979,450,250 | 1,898,692,650 | +80,757,600 (+4.25%) |

Canonical (player-1-to-move, weighted for left-right symmetry):
win = 698,682,822,479; loss = 454,286,942,635; draw = 2,590,704,561;
illegal = 989,725,125. The four classes partition the state space exactly
(sum × 2 = 2,313,100,389,600). No stuck states (mover without a legal
move) exist anywhere in the game.

## Why the counts differ from the paper, and why this solve is defensible

The differences are confined to the part-2 illegal rule for 6-piece games,
whose published statement my implementation follows literally and whose
published counts the paper's own implementation does not match (details
and the full investigation in README.md — "Known discrepancy"). Evidence
that this solve is correct under the published rules:

1. All four smaller Table 1 games without the 6-piece rule (7×7 with
   1, 2, 3, 4 pieces) reproduce the paper's wins/draws/illegal counts
   **digit for digit** with this exact code path.
2. The 6-piece static rule used here reproduces the published 4×4/6
   illegal count exactly and is provably the complete Definition 3 for six
   pieces (with 6 opponent pieces only a goal tip can be made permanently
   unreachable).
3. An independent brute-force solver agrees with the optimized solver on
   every state of two complete games (4×4/6 and 7×7/2).
4. 100,000 randomly sampled 6×6 states pass local consistency checks
   (every WIN has a losing successor; every LOSS has only winning
   successors; every DRAW has no losing successor and at least one drawn
   successor).
5. The paper notes its own 6×6 row was solved once and "should not be
   considered correct until … verification has been completed"; no such
   verification was ever published.

The signature of the discrepancy matches the smaller boards: the paper's
illegal set is smaller than the literal rule produces (+0% on 4×4, +0.49%
on 5×5, +4.25% on 6×6 relative to mine — growing with the amount of empty
space), with correspondingly small shifts in wins/draws.

## Solve statistics

* Stored states: 578,946,590,400 (both symmetries; ≈3.996× reduction) at
  2 bits = 144.7 GB, RAM-resident, checkpointed hourly to disk.
* Static classification (illegal + terminal): 78 s.
* Retrograde value iteration: 24 passes to the fixpoint, 19.8 h wall
  clock on 64 threads (~8.1 M state-expansions/s sustained in the heavy
  passes). Passes 4 and 8 proved 183 G and 187 G states respectively;
  the last proof arrived in pass 23.
* Win back-propagation supplied ~366 G of the ~699 G canonical wins.
* Peak RSS ≈ 160 GB; checkpoint writes 144.7 GB in ~30 s (page cache).

## Reproducing

```
make solve-6x6                       # ~20 h on 64 threads, 145 GB RAM
./src/query runs/m6p6 --m 6 --p 6 --initial
./src/query runs/m6p6 --m 6 --p 6 --stats
./src/query runs/m6p6 --m 6 --p 6 --verify 100000
```
