# Results: WDL tables for 6x6, 7x7, and 8x8 Quoridor

All boards strongly solved by full layered retrograde analysis (every state in the
table, not just states reachable from the start). Values: winner from each state with
optimal play; unresolved (loopy/stalemate) states are draws. Machine: 2x AMD EPYC 9115
(64 threads), 723 GB RAM.

## Start-position outcomes (walls = walls per player)

```txt
W x H   0  1  2  3  4
6 x 6   1  1  1  1  1
7 x 7   2  2  2  2  2
8 x 8   -  -  1  1  -
```

- **6x6 is a first-player win at every wall count solved (0-4 walls per player).**
  Consistent with the reference repo's finding that even-height boards are
  first-player wins. From the start the win is by pawn moves only (e.g. at w=3:
  advance 1,3 or sidestep 0,2 win; the other sidestep 0,4 and *every* opening wall
  placement lose).
- **7x7 is a second-player win at 0-4 walls per player.** At w=4 every legal
  opening move by Player1 has a Player2-win value, including all 72 wall placements
  and the three initial pawn moves.
- **8x8 is a first-player win at both wall counts solved here (w=2 and w=3).**
  In both games, advancing from `(0,4)` to `(1,4)` is Player1's only winning opening;
  the other 100 legal openings have Player2-win values.

## Solve statistics

| Board | walls | states          | P1 wins          | P2 wins          | draws          | time  | table    |
|-------|-------|-----------------|------------------|------------------|----------------|-------|----------|
| 6x6   | 0     | 2,520           | 1,296            | 1,224            | 0              | 0.0 s | -        |
| 6x6   | 1     | 3,177,720       | 1,634,252        | 1,543,460        | 8              | 0.0 s | -        |
| 6x6   | 2     | 505,680,840     | 259,980,316      | 245,532,292      | 168,232        | 0.3 s | -        |
| 6x6   | 3     | 23,916,601,800  | 12,269,959,280   | 11,586,627,800   | 60,014,720     | 15 s  | 5.9 GB   |
| 6x6   | 4     | 446,051,566,800 | 227,150,725,600  | 214,406,395,120  | 4,494,446,080  | 298 s | 109 GB   |
| 7x7   | 0     | 4,704           | 2,401            | 2,303            | 0              | 0.0 s | -        |
| 7x7   | 1     | 12,253,920      | 6,254,601        | 5,999,311        | 8              | 0.1 s | -        |
| 7x7   | 2     | 4,357,291,680   | 2,223,999,463    | 2,133,222,553    | 69,664         | 2.7 s | 1.1 GB   |
| 7x7   | 3     | 501,660,690,720 | 256,027,043,283  | 245,575,778,893  | 57,868,544     | 327 s | 122 GB   |
| 7x7   | 4     | 24,821,119,282,848 | not aggregated | not aggregated | not aggregated | 17,935 s | 1.61 TB raw, sym4 |
| 8x8   | 2     | 26,850,257,280 | not aggregated | not aggregated | not aggregated | 13.5 s | 1.72 GB raw, sym4 |
| 8x8   | 3     | 6,346,552,383,360 | not aggregated | not aggregated | not aggregated | 4,124 s | 406 GB raw, sym4 |

States are all `(side to move, pawn1, pawn2, walls-in-hand split, wall config)` with
pawn1 != pawn2; counts include terminal positions. Per-layer breakdowns are in
`production.log`. The persistent symmetry solver stores every value but does not
aggregate the three value totals during its streamed solve.

## Dataset complexity metadata

The four existing tables are unreduced: **no symmetry reduction is used**. Their
reachable counts are exact enumerations from the standard initial position.

| dataset | unreduced states | symmetry-reduced states | symmetry scheme | reachability status |
|---|---:|---:|---|---|
| 6x6 w=3 | 23,916,601,800 | not used | none | 18,654,053,548 reachable (77.996%) |
| 6x6 w=4 | 446,051,566,800 | not used | none | 322,608,610,016 reachable (72.325%) |
| 7x7 w=2 | 4,357,291,680 | not used | none | 3,677,348,176 reachable (84.395%) |
| 7x7 w=3 | 501,660,690,720 | not used | none | 418,124,712,600 reachable (83.348%) |
| 7x7 w=4 symmetry table | 24,821,119,282,848 | 6,205,280,412,387 exact state orbits; 6,205,411,261,056 stored states | four-way D2; see below | full Cartesian table, not reachable-only; about 82.3% (~20.43T) estimated reachable |
| 8x8 w=2 symmetry table | 26,850,257,280 | 6,712,564,320 exact state orbits; 6,741,060,480 stored states | four-way D2; see below | full Cartesian table, not reachable-only; roughly 85-90% estimated reachable |
| 8x8 w=3 symmetry table | 6,346,552,383,360 | 1,586,638,095,840 exact state orbits; 1,587,253,256,064 stored states | four-way D2; see below | full Cartesian table, not reachable-only; roughly 85-90% estimated reachable |

The symmetry tables use left/right reflection directly, and top/bottom reflection
or 180-degree rotation together with swapping Player1 and Player2 (including turn,
walls in hand, and winner). The stored count is slightly larger than the mathematical
orbit count because a representative wall layout retains a full pawn-state block,
including a small number of reflection-equivalent pawn states.

The state definition is `(side to move, Player1 square, Player2 square, Player1 walls
left, Player2 walls left, wall configuration)`, with distinct pawn squares. Counts
include both sides to move and terminal positions. The unreduced counts are the full
Cartesian tables over all non-overlapping wall configurations and legal wall-count
splits, not merely positions reachable from the standard start. The 7x7 w=4 reachable
percentage and the wider 8x8 ranges are rough extrapolations from the exact smaller
datasets; they are not enumerated counts.

## Wall-configuration counts (exact, via ZDD)

6x6 has 50 wall slots, 7x7 has 72. Exact counts of non-overlapping configurations
(ZDD frontier construction; the paper's product estimate shown for comparison):

```txt
6x6:  k:      0    1     2      3       4        5         6          7          8
      exact:  1   50  1160  16590  163906  1188186  6,552,684 28,132,274 95,439,775
      paper:  1   50  1150  16100  152950  1040060  5,200,300 19,315,400 53,117,350

7x7:  k:      0    1     2      3       4        5          6
      exact:  1   72  2460  53088  812594  9,391,856 85,204,184
```

The whole ≤8-wall 6x6 family (131,494,626 configurations) is represented by a ZDD of
4,195 nodes; the ≤6-wall 7x7 family (95,464,255 configs) by 8,165 nodes. The next
step up, 6x6 with 5 walls per player, would be 3.8x10^12 states (~890 GB at
2 bits/state) — beyond this machine's RAM without mirror-symmetry reduction or
disk-streamed layers.

## Validation

- 36/36 start-position outcomes reproduced from the reference repo's README table
  (`validate_readme.sh`), including the 8x3 w=3 forced draw and the 4x7
  2→1→2→1 alternating pattern.
- Full-table equality (every reachable state) against an independent naive engine on
  six small boards (`selftest` mode).
- Enumeration counts cross-checked against ZDD counts on every solve.
- Probe spot checks: parent/child value consistency at the start positions.
- The completed 7x7 w=4 table has `complete 1`, all 18 layer files have their exact
  metadata-derived sizes (1,611,374,418,659 bytes total), and full SHA-256 digests.
  The symmetry solver matched the independent engine on 3x3 w=2 (6,218 reachable
  states), 4x4 w=2 (685,416), and 8x8 w=0 (7,056), with zero mismatches.
- The completed 8x8 w=2 and w=3 tables both have `complete 1`; all 24 layer files
  match their metadata-derived sizes, and full SHA-256 manifests were generated.
