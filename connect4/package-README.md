# Connect Four {W}×{H} WDL tables

This package is a strong solution of Connect Four on a board {W} columns wide and {H} rows
high. It holds the value (win, draw or loss for the side to move) of every position that can
come up in a game, and of a few that cannot (see *Positions*).

* **{W}×{H} Connect Four is a draw.** Numbering columns 0–6 from the left, the first player
  draws by starting in column 0, 2, 3, 4 or 6, and loses by starting in column 1 or 5.
* Stored positions: **{N}** in plies 0–{LAST}. Win / draw / loss for the side to
  move: {WIN} / {DRAW} / {LOSS}. Per-ply counts are in `STATISTICS.csv`.
* Size: {ZSIZE} bytes ({ZTB} TB) compressed. Decompressed, the tables are {RSIZE} bytes
  ({RTB} TB), {RATIO}× larger.

## Quick start

```sh
sha256sum -c SHA256SUMS                                  # verify the download
g++ -O2 -std=c++17 -o c4_wdl_query c4_wdl_query.cpp -lzstd
./c4_wdl_query . -                                       # the empty board and every first move
./c4_wdl_query . 3323                                    # after moves in columns 3, 3, 2, 3
```

`c4_wdl_query` reads the compressed files directly. It needs about 9 GB of RAM (6 GB for the ZDD once loaded, which takes ~20 s) and
decompresses only the 16 MiB block that holds the position, so you don't need to decompress
anything first. Moves are 0-indexed columns from the left, and the first player moves first.
The program checks the moves against the rules. It prints the board, the value for the side to
move, and the value of every legal move.

## Files

| file | contents |
|---|---|
| `slab_<k>.bin.zst` | values of all stored positions after k moves (k = 0 … {LAST}) |
| `zdd.bin.zst` | the ZDD that maps a position to its index in its slab |
| `PACK_BASE3` | marker: the slabs hold 5 values per byte (base 3) |
| `c4_wdl_query.cpp` | standalone query program; its header documents every format |
| `STATISTICS.csv` | per ply: positions, wins, draws, losses for the side to move |
| `MANIFEST.tsv` | per table file: original size and SHA-256, compressed size and SHA-256 |
| `SHA256SUMS` | SHA-256 of every other file, for `sha256sum -c` |

Each `.zst` file uses the zstd **seekable format**: independent zstd frames of 16 MiB of input
each, with content checksums, followed by a seek table in a skippable frame. Stock `zstd -d`
restores the original file byte for byte, and `MANIFEST.tsv` lists the original files' SHA-256.
The solver in the repository below reads the decompressed directory directly
(`c4 probe --board {W}x{H} --dir DIR`). That needs {RTB} TB of disk.

## Positions

A position after k moves is stored at ply k. Its index within the ply is its rank in the
lexicographic order of the ply's position set, computed with the ZDD. Each ply's set holds every
stacking of k stones, ⌈k/2⌉ of them the first player's, with no four-in-a-row anywhere. That
includes every position reachable in a game that is not yet over, and also a tiny fraction that
no move order produces. Their values are still correct as game-theoretic values. Positions with
a four-in-a-row and full boards are not stored: the game is already over, and the query program
detects them itself.

Slab values: 0 = draw, 1 = win, 2 = loss, for the side to move. In base 3, byte i/5 holds
Σ v_j·3^j for j = i mod 5.

## Provenance

The tables were computed with the ZDD-indexed retrograde solver `c4` in
<https://github.com/jh85/smallgames> (`connect4/`), in about 10.6 days on 2× AMD EPYC 9115
(64 threads), and finished on 2026-11-25. That repository's README describes the method and
the checks:

* the ZDD's per-ply counts equal an independent counting DP on all plies;
* random positions agree with an independent negamax search;
* the same code reproduces the reference BDD solver's per-ply tables exactly on 4×7, 5×7, 6×4,
  5×5 and 7×6.

The package was built with `c4pack` from the same repository. Every compressed frame was
decompressed and compared with its input before it was written. The query program was checked
against `c4 probe` on random positions.
