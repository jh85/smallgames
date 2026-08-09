# Results: WDL tables for 6x6 and 7x7 Quoridor

All boards strongly solved by full layered retrograde analysis (every state in the
table, not just states reachable from the start). Values: winner from each state with
optimal play; unresolved (loopy/stalemate) states are draws. Machine: 2x AMD EPYC 9115
(64 threads), 723 GB RAM.

## Start-position outcomes (walls = walls per player)

```txt
W x H   0  1  2  3  4
6 x 6   1  1  1  1  1
7 x 7   2  2  2  2*        (* = w=3 solved; w>=4 exceeds RAM on this machine)
```

- **6x6 is a first-player win at every wall count solved (0-4 walls per player).**
  Consistent with the reference repo's finding that even-height boards are
  first-player wins. From the start the win is by pawn moves only (e.g. at w=3:
  advance 1,3 or sidestep 0,2 win; the other sidestep 0,4 and *every* opening wall
  placement lose).
- **7x7 is a second-player win at 0-3 walls per player** (mirror defense; e.g. after
  P1's advance 1,3 with w=3, P2's winning reply is the mirror advance 5,3). Whether
  7x7 flips to a first-player win at higher wall counts — as 5x5 does at w=5 —
  remains open here; the 5x5 flip suggests it might.

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

States are all `(side to move, pawn1, pawn2, walls-in-hand split, wall config)` with
pawn1 != pawn2; counts include terminal positions. Per-layer breakdowns are in
`production.log`.

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
