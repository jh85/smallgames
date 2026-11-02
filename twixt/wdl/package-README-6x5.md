# TwixT 6x5 WDL package

Strong solution of TwixT on a 6-column by 5-row board: the game-theoretic
value (Win, Draw, Loss for the side to move) of every reachable position.

* Result: **White wins (the first player)**; all 20 of White's opening moves win.
* Reachable positions: 11,768,029,829 in 27 plies (0-26 moves).
* Rules: White moves first and joins top and bottom; Black joins left and
  right. Pegs may not go on the opponent's border lines; corners are unused.
  A new peg is linked automatically to every own peg a knight's move away
  unless the link would cross an existing link. No pie rule, no link removal.

## Contents

| file | |
|---|---|
| `result-K.twxlay` | positions after K moves: three minimal DFAs (Win, Loss, Draw for the side to move) |
| `layout.txt` | board geometry and the variable order of a DFA word |
| `manifest.twxman` | layer list with sizes and checksums (lets the full solver load the package) |
| `wdl_query.cpp` | standalone example: play moves, print the outcome |
| `STATISTICS.md`, `6x5-wdl.csv`, `6x5.json` | root value, totals, per-ply W/D/L counts |
| `SHA256SUMS` | SHA-256 of every other file |

## Query

```sh
sha256sum -c SHA256SUMS
g++ -O2 -std=c++17 -o wdl_query wdl_query.cpp
./wdl_query . C3 D4        # outcome for the side to move after White C3, Black D4
```

Cells are named by column letter and row number (A1 is a corner). The program
replays the moves with the rules above, rejects illegal moves and moves after a
completed connection, and decides membership with one sequential pass over a
single layer file (one bit per node of memory). With the source tree,
`twixt --probe-dfa manifest.twxman --moves C3,D4` gives the same answer and also
lists the value of every move.

## Formats

A position is a word with one symbol per level of `layout.txt`: `turn` (0
White, 1 Black, to move), `cell c` (0 empty, 1 White peg, 2 Black peg), `link e`
(0 absent, 1 present). `cross e ...` lists the links that cross link e.

`result-K.twxlay` (TWXLAY01, version 2, little-endian): magic `TWXLAY01`; u32
version, width, height, order, length, ply, kind, root count, ID bytes; u64
node count; u64 roots (local IDs of Win, Loss, Draw). Local ID 0 is the empty
language, 1 the accepting terminal, nodes 2 and up. Node records follow,
grouped by level from the deepest level up, each group in chunks; a record is
a LEB128 local-ID delta (0 for a chunk's first record), an arc mask (bit s:
symbol s has an arc), and the children's local IDs (ID-bytes each). After the
records a directory gives, per level, the chunks' offsets, first local IDs and
record counts. The file ends with the directory offset (u64) and a checksum
(u64) of everything before it.

## Provenance and checks

Solved with the symbolic solver in this repository (`twixt/`), source
tables/6x5-store (one store-mode run, 19 minutes on 64 cores). Every layer's
W+D+L count equals its forward reachable count. Before each ply was committed,
2,000 positions drawn uniformly from it were relabelled with the explicit
rules from their successors' stored labels, all consistently. Not
cross-checked by an independent exhaustive solver. `wdl_query` was compared
with the solver's probe on random move sequences.
