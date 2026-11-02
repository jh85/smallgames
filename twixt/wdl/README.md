# TwixT WDL packages

A WDL package is a `.tar.zst` archive holding the complete win/draw/loss
solution of one board, ready to query without this repository:

| file | |
|---|---|
| `result-K.twxlay` | positions after K moves: three minimal DFAs (Win, Loss, Draw for the side to move), TWXLAY01 version 2 |
| `layout.txt` | board geometry and the variable order of a DFA word (`twixt --export-layout`) |
| `manifest.twxman` | layer list with sizes and checksums, so `twixt --probe-dfa` can load the package |
| `wdl_query.cpp` | standalone example ([wdl_query.cpp](wdl_query.cpp)): replay moves, print the outcome |
| `README.md` | rules, result, and file formats ([5x6](package-README-5x6.md), [6x6](package-README-6x6.md)) |
| `STATISTICS.md`, `BOARD-wdl.csv`, `BOARD.json` | root value, opening moves, totals, per-ply W/D/L counts |
| `SHA256SUMS` | SHA-256 of every other file |

```sh
sha256sum -c twixt-6x6-wdl.tar.zst.sha256
tar -I zstd -xf twixt-6x6-wdl.tar.zst && cd twixt-6x6-wdl
sha256sum -c SHA256SUMS
g++ -O2 -std=c++17 -o wdl_query wdl_query.cpp
./wdl_query . C3 D4        # outcome for the side to move after White C3, Black D4
```

`wdl_query` rejects illegal moves and moves after a completed connection, and
decides membership with one sequential pass over a single layer file (one bit
per node of memory). It agreed with `twixt --probe-dfa` on random move
sequences: 88 positions on 5x6 (plies 1-26) and 34 on 6x6 (plies 1-12, 20, 26, 32).

## Packages built 2026-11-02

| board | root | reachable positions | archive | archive SHA-256 |
|---|---|---:|---:|---|
| 5x6 | Black wins | 11,101,540,840 | 1.4 GB | `07411c893f0511f2b738b3de420498f56e7ab6e0bcd6c2c1c747df183c2fab66` |
| 6x6 | White wins | 10,395,631,394,018 | 152 GB | `385635a29e604c8119017e35b2b0fe0937511ee7f8f06bbcf5fe8ca1860a48c4` |

The archives are kept under the git-ignored `tables/packages/` on the solving
machine; no external copy has been published. The 5x6 layers come from a
per-layer re-solve that matched the reference archive `5x6.twixtdfa` exactly;
the 6x6 layers are the store of the solve described in
[../SOLVE_6X6.md](../SOLVE_6X6.md).

## Rebuilding

```sh
python3 tools/make_wdl_package.py 6x6 tables/6x6-store tables/packages
```

The script hard-links the store's result layers, writes `layout.txt` with
`twixt --export-layout`, copies the README template, the query example and the
`results/` files, generates `STATISTICS.md` and `SHA256SUMS`, and compresses
the directory with zstd level 10. The archive hash depends on the tar and zstd
versions; the hashes in `SHA256SUMS` do not.
