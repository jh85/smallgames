# Connect Four (7×6) strong solver — ZDD minimal perfect hash + retrograde WDL table

Strongly solves 7×6 Connect Four (and smaller boards) with the ZDD-MPH + retrograde
architecture of the [NOCCA×NOCCA](../nocca/README.md) and
[Dobutsu Shogi](../dobutsushogi/README.md) solvers in this repository, producing a packed
2-bit WDL table answering win/draw/loss for any position. The reference BDD solution
(Edelkamp & Kissmann-style, [markus7800/Connect4-Strong-Solver](https://github.com/markus7800/Connect4-Strong-Solver))
solves the same game in 47 h / 128 GB; this pipeline solves and *tabulates* it in ~9 h
(build 27 s + solve 8.9 h) on 2× EPYC 9115 / 64 threads.

## Results (solved + validated 2026-07-31)

* **First player wins**, and the per-first-move values are the classical ones: only the
  center column wins; the two adjacent columns draw; the four outer columns lose.
* Position space: **2,637,477,442,337** pseudo-legal non-terminal positions across plies
  0–42 (vs Tromp's 4,531,985,219,092 total states — positions with a four-in-a-row need
  no storage). Packed table: **615 GiB** in `c4_wdl_7x6/slab_{ply}.bin`.
* The shared ZDD forest (one root per ply) has just **55,744,075 nodes** (~1 GB file); the
  exact node count varies by a hair between builds (see *Generating the WDL tables*).

## Validation

* ZDD root counts equal an independent counting DP for all 43 plies; the counting DP is
  itself validated by full brute-force enumeration on 4×4 and 6×4.
* Small boards, record-level: my per-ply won/drawn/lost/terminal tables match the
  reference BDD solver's CSVs **exactly on every row** for 6×4 (second-player win) and
  5×5 (draw); 4×4 (draw) matches full negamax on all positions.
* 7×6, record-level: a full reachability sweep (BFS over all 2.64 T table positions)
  reproduces the reference solution table **exactly for all 43 plies** — reachable
  non-terminal counts and first-player-perspective W/D/L splits to the unit
  (e.g. ply 30: 199,698,237,436 / 6,071,049,190 / 53,916,883,330).
* 60 random game positions (plies 4–40) agree with the reference repo's independent
  `full_ab_search` (no BDD, no ZDD); 3,000 random deep positions agree with my own
  independent negamax; the repo README's example position (moves `332`) reproduces its
  documented evaluation exactly (WIN; winning moves = columns 1 and 4).

## Design

* **Space** (per ply k): gravity stackings of k stones with ⌈k/2⌉ first-player stones and
  **no four-in-a-row**. This is a tight superset of the reachable non-terminal positions
  (a position without a 4-row never passed through one — stones are never removed; the
  only pseudo-only members are those with no alternating-color placement order, ~10⁻⁵ at
  ply 42). Terminal positions are recognized directly by the probe; full boards are draws.
* **ZDD**: columns are swept left to right; each column is one of 127 stack states
  (encoded as 7 binary items, MSB first). The frontier is (last 3 column states — enough
  to check every horizontal/diagonal window when its last column is placed; vertical runs
  are excluded from the state list) plus (remaining stones r, remaining first-player
  stones rx). Parametrizing by *remaining* counts lets all 43 ply roots share one node
  pool. Construction: forward liveness bitmaps over the 32-bit (window, r, rx) keys, then
  backward level-by-level node construction with lock-free hash-consing (27 s on 64
  threads). Rank/unrank via per-node path counts; children reuse the parent's per-column
  walk prefix.
* **Solve**: Connect Four is acyclic in the stone count, so a single backward sweep
  (ply 41 → 0) computes every value with only two ply slabs resident; each position takes
  the best over ≤7 moves (immediate-win bitboard check, else child lookup at ply+1).
  No fixpoint iteration, no distance bookkeeping needed.

## Build & usage

```
make                       # g++ -O3, C++20
./c4 count   [--board WxH] [--brute]     # pseudo-space counting DP (+ brute check)
./c4 buildzdd|solve [--board WxH]        # build forest (verifies vs DP), solve slabs
./c4 probe <moveseq>       # e.g. ./c4 probe 332   ('-' = initial position)
./c4 analyze               # initial position + per-move values
./c4 selftest | bfcheck [n] | reachtally | bigtally   # validation suite
```

Common flags: `--board WxH` (default 7×6; 4≤W≤7, 4≤H≤6), `--threads N`, `--dir D`. Tables
are read and written under `c4_wdl_<W>x<H>/` **relative to the current working directory**,
so the solver tree can be moved without editing source; `--dir D` overrides the location.

Probe input is a move sequence in 0-indexed columns from the initial position. Output:
board, side to move, WDL for the side to move, and per-move values.

## Generating the WDL tables

The tables are **not** stored in this repository — 7×6 alone is 615 GiB. Everything needed
to regenerate them is here.

```
make
./c4 count    --board 4x4 --brute    # optional: DP vs brute-force enumeration
./c4 solve    --board 4x4            # buildzdd runs automatically if zdd.bin is absent
./c4 bfcheck  100                    # spot-check against independent negamax
```

Replace `--board 4x4` with `6x4`, `5x5`, or `7x6`. `solve` writes `zdd.bin` plus one
`slab_{ply}.bin` per ply into the board directory; it is **resumable** — each ply is written
to `slab_{ply}.bin.tmp` and renamed on completion, so re-running the same command continues
at the first missing ply.

**On reproducibility.** The slab files are bit-for-bit reproducible: a fresh solve
reproduces the `slab_*.bin` digests in `checksums/` exactly, regardless of thread count
(verified on 4×4). `zdd.bin` is not — the forest is built with lock-free hash-consing, so
racing inserts leave slightly different sets of equivalent nodes, and the file's size and
digest vary from run to run. That is harmless: rank is the lexicographic index of a position
within the represented set, which is canonical no matter how the nodes end up shared, and
that is precisely why the slabs still match. Every build also verifies its per-ply root
counts against the independent counting DP before saving. So compare rebuilt slabs against
`checksums/`, but expect a locally rebuilt `zdd.bin` to differ from the published one.

| board | files | total size | notes |
|---|---|---|---|
| 4×4 | 17 | 150,944 B | instant; `--brute` cross-check feasible |
| 6×4 | 25 | 22,212,456 B | second-player win |
| 5×5 | 26 | 20,637,575 B | draw |
| 7×6 | 43 | 660,138,684,135 B (615 GiB) | build 27 s + solve 8.9 h on 64 threads |

For the full 7×6 run:

| | |
|---|---|
| Disk | ~683 GiB free: the 615 GiB table plus the in-progress `.tmp` slab (largest is 72.5 GB) |
| RAM | The ZDD forest (~1 GB) stays resident. Slabs are **file-backed** `mmap`s, so the OS pages them on demand — it will run with modest RAM but becomes page-cache bound; two adjacent plies are live at once (the largest pair, plies 31+32, is ~141 GB) |
| Threads | `--threads N`, default = hardware concurrency |

## Downloading and verifying the tables

The tables are hosted outside this repository. Each board is published as one
`c4_wdl_<W>x<H>.tar.zst` archive holding the whole board directory. The WDL data compresses
7.4×, so 7×6 is an 83 GiB download that expands to 615 GiB.

| board | archive | expands to | SHA-256 of archive | download |
|---|---|---|---|---|
| 4×4 | 58,914 B | 150,944 B | `9c78714b6b42b2591c5ea08bc086d9f5d237368ac019da991d436b8a0605fe7c` | not published yet |
| 5×5 | 8,465,218 B | 20,637,575 B | `7fe5840723bdd538845a17ba88ee8cf1dccad41db3b67d3a72dc73160e18a312` | not published yet |
| 6×4 | 7,524,475 B | 22,212,456 B | `242787a3f393685d0202f7787534643edcde2c08db0f3c48d0b1c660a4824567` | not published yet |
| 7×6 | 89,216,767,476 B | 660,138,684,135 B | `2a79d7a429d59a346c2bbf3fdb84afd9620fe70236d363f5f42f87b37064a79e` | not published yet |

Extract into this directory, which recreates `c4_wdl_<W>x<H>/` where the solver expects it:

```
echo "2a79d7a429d59a346c2bbf3fdb84afd9620fe70236d363f5f42f87b37064a79e  c4_wdl_7x6.tar.zst" \
  | sha256sum -c -
tar --zstd -xf c4_wdl_7x6.tar.zst
```

Or stream it, which never stores the archive — useful for 7×6, since it needs only the
615 GiB of extracted output rather than 615 + 83:

```
curl -sL <url> | tar --zstd -xf -
```

The archives are made with `zstd -12 --long=27 -T0`. The 128 MiB window is exactly zstd's
default decoder limit, so stock `tar --zstd -xf` and `zstd -d` work with **no extra flags**.
`--long=31` compresses ~10% better, but its 2 GiB window exceeds that limit: every downloader
would have to pass `--long=31` or `--memory=2048MB`, and plain `tar --zstd -xf` fails with
"Frame requires too much memory for decoding". Not worth 10%.

After extracting, `checksums/` holds a `sha256sum`-format manifest per board that verifies
the files individually — worth running on the 7×6 set, and the way to tell *which* slab is
bad rather than just that something is:

```
cd c4_wdl_7x6 && sha256sum -c ../checksums/c4_wdl_7x6.SHA256SUMS
```

The manifests cover `zdd.bin` too, so they fully verify an extracted download. They will not
match a locally rebuilt `zdd.bin` — see the note above.

Slab format: 2 bits per position indexed by the ply's ZDD rank (0 = draw, 1 = win, 2 = loss,
from the side to move's perspective).

### Rebuilding the archives

```
cd <parent of c4_wdl_7x6>
tar -cf - c4_wdl_7x6 | zstd -q -12 --long=27 -T0 -o c4_wdl_7x6.tar.zst
```

7×6 takes ~34 min on 64 threads. To check an archive's contents without unpacking 615 GiB to
disk, stream each member through `sha256sum` and diff the result against the manifest:

```
zstd -dc --long=27 c4_wdl_7x6.tar.zst \
  | tar -xf - --to-command='sh -c "sha256sum | sed \"s|-\$|\$TAR_FILENAME|\""' \
  | sed 's|  .*/|  |' | sort -k2 -V | diff - checksums/c4_wdl_7x6.SHA256SUMS
```

## Reference solver (not included)

`crosscheck.py` and the `bigtally` / `reachtally` commands compare against
[markus7800/Connect4-Strong-Solver](https://github.com/markus7800/Connect4-Strong-Solver),
the Edelkamp & Kissmann-style BDD solver. That repository and its paper are not
redistributed here; clone it alongside this directory to re-run those checks:

```
git clone https://github.com/markus7800/Connect4-Strong-Solver
# build its src/connect4 targets, then:
python3 crosscheck.py 60      # 60 random positions vs its full_ab_search
```

Neither the reference repository nor any downloaded table is required to build this solver
or to generate the tables from scratch.

## Files

| file | contents |
|---|---|
| `c4.cpp`, `Makefile` | complete standalone solver and build recipe (~1,500 lines C++20) |
| `README.md` | results, format documentation, build instructions, and command examples |
| `checksums/c4_wdl_*.SHA256SUMS` | per-file SHA-256 manifests for the externally published tables |
| `crosscheck.py` | random-position cross-check driver for the reference solver above |

The compiled `c4` binary, run logs, the generated `c4_wdl_*/` directories, and the cloned
reference repository are excluded by `.gitignore`.
