# Amazons TITLE WDL package

Strong solution of the Game of the Amazons on BOARD_PHRASE, with 4 queens
per side in the standard small-board setup: the value (win or loss for the side to move;
Amazons has no draws) of every position reachable from the standard start.  Results and
per-ply counts are in `STATISTICS.md`.TABLE_NOTECOMBINED_NOTE

* Rules: a move is a queen move followed by an arrow shot from the queen's new square, both
  along queen lines over empty squares.  The arrow burns its square for good.  A player who
  cannot move loses.
* Setup: the first player's queens on b1, the second column from the right in row 1, a2
  and the last column of row 2; the second player's mirrored at the far edge.  Columns are
  letters from the left, rows numbers from the first player's side.

## Query

```sh
sha256sum -c SHA256SUMS
g++ -O2 -std=c++17 -mbmi2 -o wdl_query wdl_query.cpp -lzstd
./wdl_query . QBOARD --moves              # the start, and the value of every first move
./wdl_query . QBOARD b1-b4xd4 --moves     # after one move
```

Moves are from-to x arrow, as in Song and Mueller.  The program replays them with the rules,
rejects illegal ones, and reads one bit; plies the package does not store are answered by one
ply of search into the next.  Values are exact for positions reachable from the standard
start, which is everything a replayed game can reach.

## Contents

| file | |
|---|---|
| `k<K>/c<C>.tiles`, `k<K>/c<C>.idx` | positions after K moves (K arrows), class C: zstd frames of loss bits, and their index |
| `config.txt` | board of the files, classes, canonical-set rule, `answers` |
| `wdl_query.cpp` | standalone query; its header describes the file format |
| `STATISTICS.md`, FILES | root value, opening moves, reachable positions and wins per ply |
| `SHA256SUMS` | SHA-256 of every other file |

## Provenance and checks

Solved with `slabsolve` in `smallgames/amazons/dfa`, a bit-parallel retrograde solver over
every placement of queens and arrows.  Every solved layer had a sample relabelled from the
rules, and whole games were played against the table.  `slabsolve reach` then computed the
positions reachable from the start with the same kernel run forwards, checked samples
against the rules, counted every layer twice by two different symmetry paths, and wrote this
package by masking the solved layers: bits of unreachable positions are zero.  The package
was compared with the full table on random games (`slabsolve pkgcheck`).
