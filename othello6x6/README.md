# 6×6 Othello — exact WDL/score oracle over the semi-strong tablebase

`wdl6x6.cpp` answers **every legal 6×6 Othello position exactly** — win/draw/loss and the
final disc differential — by combining a lookup in Takizawa's semi-strongly-solved tablebase
with a built-in exact endgame solver for whatever the table does not decide.

This directory contains **only that tool**. The tablebase itself is a separate, third-party
dataset and is not redistributed here — see *Getting the tablebase* below.

## Why a second tool

The tablebase ships with its own query utility (`query_6x6.py`), which performs an exact
lookup and reports an error when the position is not present. That leaves gaps: positions
outside the certified region, and records that store only bounds rather than an exact value.

`wdl6x6` closes them:

* **Tier 1** — look the position up in the tablebase. It reads the auxiliary bounds records
  (`node_kind` without an exact bit) as well as exact ones.
* **Tier 2** — if the table does not decide the position, run an exact alpha–beta endgame
  search. The search probes the tablebase as it goes: exact hits terminate branches and
  bounds produce cutoffs, and a persistent in-RAM transposition table is kept across
  queries so batch evaluation stays fast.

Concretely, on the example position from the dataset's own readme
(`000000000020011120022220000000000000`):

```
$ python3 query_6x6.py --pos012 000000000020011120022220000000000000
ERR 'not found'

$ ./wdl6x6 --data-dir . --wdl --pos012 000000000020011120022220000000000000
WDL (side to move): WIN
score bounds: [2, 36]  (WDL decided; exact score not requested)
source: tablebase record, node_kind=16 (auxiliary bounds / proof certificate)
```

The record was there all along, as a bounds-only certificate.

## Build

```
make            # g++ -O2 -march=native -std=c++17
```

No dependencies beyond a C++17 compiler; the tool is a single translation unit and does not
use the Python utility or any part of the dataset's tooling.

## Usage

```
./wdl6x6 [options] --pos012 <36 chars>            value of one position
./wdl6x6 [options] --pos012 <36 chars> --moves    exact value of every legal move
./wdl6x6 [options] --server                       one query per stdin line
```

`--server` mode reads `<pos012>` or `moves <pos012>` per line and replies
`<W|D|L> <lower> <upper> <kind> [pass] [terminal] [solved]` or `M <idx>:<score> ...`;
`ERR <msg>` on bad input, `exit`/`quit` to stop.

| option | meaning |
|---|---|
| `--data-dir DIR` | directory holding `optimal_reopening_ab_table_all_{n}.txt` (default `.`) |
| `--wdl` | decide win/draw/loss only, via null-window search — much faster on hard positions; the reported bounds are then one-sided |
| `--tt-mb N` | transposition table size in MiB (default 4096) |
| `--probe-min N` | probe the tablebase inside the search only at nodes with ≥ N empty squares (default 8) |
| `--no-table` | disable all table use; pure search (for testing) |

**Position format (`pos012`)** — 36 characters for the inner 6×6 board (B2..G7) in row-major
order: `0` empty, `1` side to move, `2` opponent. Index `i` is square (`i/6`, `i%6`).

Scores are final disc differentials from the side to move, with empty squares awarded to the
winner: `v > 0` win, `v == 0` draw, `v < 0` loss.

## Getting the tablebase

The tablebase is **not part of this repository** and is not ours to redistribute. It is
published by Takizawa on Zenodo, with its own README, checksums and license terms:

* Dataset: **Zenodo record [18843225](https://zenodo.org/records/18843225)** —
  `postprocess3.tar.zst` split into 89 parts of ~1.5 GiB (~129 GiB compressed, ~545 GB
  extracted).
* Paper: Takizawa, *"Semi-Strongly Solved"*,
  arXiv:[2411.01029](https://arxiv.org/abs/2411.01029).

Download and extract per the record's own instructions — streaming extraction avoids keeping
the intermediate archive:

```
for n in {00..88}; do
  wget "https://zenodo.org/records/18843225/files/postprocess3.tar.zst.part-$n?download=1"
done
sha256sum -c SHA256SUMS
cat postprocess3.tar.zst.part-* | zstd -d | tar -xf -
```

Then point the tool at the extracted directory:

```
./wdl6x6 --data-dir /path/to/postprocess3 --pos012 <36 chars>
```

Budget ≥ 600 GB free for extraction. The per-disc-count tables are
`optimal_reopening_ab_table_all_{n}.txt` for `n` = 4..35, 13 bytes per record: a 9-character
base-81 position key, a `node_kind` char, and lower/upper bound chars. Records are sorted by
the solver's `encode_bb()` code of the canonical (symmetry-minimal) position, not by the
ASCII key bytes — `wdl6x6.cpp` documents the format in full at the top of the file.

## What is not here

Nothing in this directory is third-party. Deliberately excluded, and gitignored:

| excluded | why |
|---|---|
| `postprocess3/` and `postprocess3.tar.zst` | Takizawa's dataset — download from Zenodo above |
| `query_6x6.py`, `count_positions.py`, `readme.md` | shipped inside that dataset, not ours |
| `edax-reversi/`, `Egaroucid/`, `reopening-alphabeta-experiment/` | upstream engines cloned for reference ([abulmo](https://github.com/abulmo/edax-reversi), [Nyanyan](https://github.com/Nyanyan/Egaroucid), [eukaryo](https://github.com/eukaryo/reopening-alphabeta-experiment)) |
| `paper.pdf` | the published paper; see the arXiv link |

## Files

| file | contents |
|---|---|
| `wdl6x6.cpp`, `Makefile` | the oracle and its build recipe (~900 lines C++17) |
| `README.md` | this file |
