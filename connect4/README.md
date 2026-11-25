# Connect Four (7×6, 7×7) strong solver — ZDD minimal perfect hash + retrograde WDL table

Strongly solves 7×6 and 7×7 Connect Four (and smaller boards) with the ZDD-MPH + retrograde
architecture of the [NOCCA×NOCCA](../nocca/README.md) and
[Dobutsu Shogi](../dobutsushogi/README.md) solvers in this repository, producing a packed
2-bit WDL table answering win/draw/loss for any position. The reference BDD solution
(Edelkamp & Kissmann-style, [markus7800/Connect4-Strong-Solver](https://github.com/markus7800/Connect4-Strong-Solver))
solves the same game in 47 h / 128 GB; this pipeline solves and *tabulates* it in ~9 h
(build 27 s + solve 8.9 h) on 2× EPYC 9115 / 64 threads. The same pipeline then solved
7×7, 34× larger, in about 10.6 days: **7×7 is a draw** (see *7×7 results*).

## 7×6 results (solved + validated 2026-07-31)

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

## 7×7 results (solved 2026-11-25)

* **7×7 Connect Four is a draw.** First-move values: columns 0, 2, 3, 4 and 6 (0-indexed)
  draw, and columns 1 and 5 lose. The center no longer wins on 7 rows; the two outer
  columns hold the draw, and the columns next to them lose.
* Position space: **90,623,793,528,223** pseudo-legal non-terminal positions across plies
  0–49. Ply 49 is the 14,321,537,220 full boards with no four-in-a-row. These are all draws,
  so they are not stored. The 49 stored slabs (plies 0–48) hold 90,609,471,991,003
  positions in **18,121,894,398,219 B (16.5 TiB)** base-3. Each slab is exactly ⌈n/5⌉ bytes.
  With `zdd.bin` (6.05 GB) the whole board directory is 18,127,945,461,751 B.
* Totals over the stored plies (side to move): W 68,873,880,489,593 / D 2,654,546,019,603 /
  L 19,081,045,481,807.
* Solve time: **about 10.6 days** on 2× EPYC 9115 / 64 threads. The log records 880,588 s
  (10.2 days) for plies 48 → 0. It does not count the work done on plies 41 and 36 before
  they were interrupted, which is roughly another 0.4 day. Wall clock was 2026-11-13 to 2026-11-25. That span includes one stop to attach
  the archive disk, one restart after an out-of-space abort, and one power outage. Each
  resumed from its in-ply checkpoint.

Validation:

* The ZDD root counts equal the independent counting DP on all 50 plies (`./c4 count --board 7x7`
  reproduces the totals above).
* Every ply satisfies n = W + D + L, and every slab file has the size the ZDD count predicts.
* `bfcheck 300` passed: 300 random positions sampled across the finished plies agree with
  an independent negamax.
* Earlier 7-row checks, made before the 7×7 run:
  * 4×7 (draw) and 5×7 (draw): the per-ply won/drawn/lost/terminal tables from `reachtally`
    match the reference BDD solver's `results_solve` CSVs **exactly on every row**. That is
    29 and 36 plies, and 135,385,909 and 14,171,315,454 positions. The reference counts
    values for the side to move, so W and L swap on odd plies. The counting DP equals
    brute-force enumeration on 4×7, and `selftest` and `bfcheck` pass on both boards.
  * Base-3 and 2-bit packing decode to identical values on 6×4 and 5×5.
  * In-ply checkpoints: 6×5 solves were SIGKILLed at random times, 15–17 kills per run.
    They produced slabs and per-ply tallies identical to an uninterrupted run.

Pipeline comparison:

| | 7×6 | 7×7 |
|---|---:|---:|
| pseudo-legal non-terminal positions | 2,637,477,442,337 | **90,623,793,528,223** |
| largest ply | ply 32: 2.90×10¹¹ | ply 38: 9.10×10¹² |
| ZDD forest | 55.7 M nodes, 27 s | 355,944,868 nodes (6.05 GB), 190 s |
| table | 615 GiB (2-bit) | 16.5 TiB base-3 (22.7 TB if 2-bit) |
| working pair on disk | 141 GB | 3.6 TB base-3 (plies 37+38) |
| solve | 8.9 h | 10.2 days |

The next ply's slab does not fit in RAM (723 GB). For a fixed move column, though, child ranks
increase with parent rank, so the child slab is read as 7 forward streams. The page cache
handles that, and there is no random I/O. The rate fell steadily, from 163 M/s at ply 47 to
about 100 M/s at plies 37–39 and 70–80 M/s below ply 30. The rates shown for plies 41 and 36
are too high: those plies resumed from a checkpoint, and their time covers only the resumed part.

Per-ply results (W/D/L for the side to move, 64 threads; ply 49 is not stored, all draws):

| ply | positions | W / D / L | time | rate |
|---:|---:|---|---:|---:|
| 49 | 14,321,537,220 | 0 / 14,321,537,220 / 0 | — | (full boards) |
| 48 | 76,185,111,344 | 24,467,558,434 / 51,717,552,910 / 0 | 82 s |  |
| 47 | 214,011,067,810 | 89,092,350,436 / 107,133,060,838 / 17,785,656,536 | 1,316 s | 163 M/s |
| 46 | 498,748,865,078 | 297,193,526,168 / 159,048,224,904 / 42,507,114,006 | 3,258 s | 153 M/s |
| 45 | 953,521,410,133 | 585,788,770,138 / 213,370,858,803 / 154,361,781,192 | 6,837 s | 139 M/s |
| 44 | 1,770,971,384,888 | 1,282,305,112,091 / 233,320,601,432 / 255,345,671,365 | 12,656 s | 140 M/s |
| 43 | 2,823,679,161,433 | 2,006,840,674,411 / 258,329,664,777 / 558,508,822,245 | 21,847 s | 129 M/s |
| 42 | 4,391,258,836,348 | 3,368,106,125,521 / 246,350,114,067 / 776,802,596,760 | 34,740 s | 126 M/s |
| 41 | 5,822,364,030,981 | 4,314,433,150,777 / 245,716,808,660 / 1,262,214,071,544 | 33,219 s | 175 M/s |
| 40 | 7,470,647,611,850 | 5,840,412,810,506 / 213,414,679,604 / 1,416,820,121,740 | 66,788 s | 112 M/s |
| 39 | 8,294,952,297,041 | 6,242,890,915,135 / 196,928,896,968 / 1,855,132,484,938 | 80,124 s | 104 M/s |
| 38 | 9,102,793,722,550 | 7,182,151,405,452 / 159,439,942,489 / 1,761,202,374,609 | 90,193 s | 101 M/s |
| 37 | 8,810,420,408,093 | 6,654,776,892,485 / 139,073,972,826 / 2,016,569,542,782 | 93,239 s | 94 M/s |
| 36 | 8,554,804,490,728 | 6,765,299,630,435 / 106,520,309,287 / 1,682,984,551,006 | 68,275 s | 125 M/s |
| 35 | 7,397,955,233,475 | 5,562,567,664,743 / 88,886,155,708 / 1,746,501,413,024 | 81,957 s | 90 M/s |
| 34 | 6,472,132,524,270 | 5,105,775,674,275 / 64,725,845,294 / 1,301,631,004,701 | 70,656 s | 92 M/s |
| 33 | 5,067,541,597,733 | 3,773,919,605,101 / 51,647,547,139 / 1,241,974,445,493 | 59,164 s | 86 M/s |
| 32 | 4,042,316,535,698 | 3,173,594,384,797 / 35,916,338,881 / 832,805,812,020 | 46,311 s | 87 M/s |
| 31 | 2,897,075,413,223 | 2,129,081,164,859 / 27,436,955,380 / 740,557,292,984 | 35,523 s | 82 M/s |
| 30 | 2,128,476,185,184 | 1,660,300,164,340 / 18,250,405,253 / 449,925,615,591 | 25,516 s | 83 M/s |
| 29 | 1,408,112,876,899 | 1,017,927,448,771 / 13,368,371,614 / 376,817,056,514 | 18,000 s | 78 M/s |
| 28 | 959,809,518,358 | 743,129,640,491 / 8,488,722,525 / 208,191,155,342 | 11,808 s | 81 M/s |
| 27 | 589,539,005,677 | 418,043,979,844 / 5,962,867,633 / 165,532,158,200 | 7,788 s | 76 M/s |
| 26 | 374,767,510,012 | 287,906,190,740 / 3,607,133,055 / 83,254,186,217 | 4,726 s | 79 M/s |
| 25 | 214,655,593,143 | 148,921,167,013 / 2,433,405,542 / 63,301,020,588 | 2,927 s | 73 M/s |
| 24 | 127,778,221,714 | 97,407,129,055 / 1,399,167,371 / 28,971,925,288 | 1,646 s | 78 M/s |
| 23 | 68,454,498,069 | 46,342,936,245 / 907,277,682 / 21,204,284,142 | 957 s | 72 M/s |
| 22 | 38,258,881,988 | 28,957,283,742 / 494,069,296 / 8,807,528,950 | 499 s | 77 M/s |
| 21 | 19,201,467,652 | 12,650,313,066 / 307,965,154 / 6,243,189,432 | 273 s | 70 M/s |
| 20 | 10,090,856,946 | 7,591,328,880 / 158,189,848 / 2,341,338,218 | 133 s | 76 M/s |
| 19 | 4,747,012,624 | 3,034,004,658 / 94,886,056 / 1,618,121,910 | 69 s |  |
| 18 | 2,347,189,364 | 1,757,901,433 / 45,784,367 / 543,503,564 | 31 s |  |
| 17 | 1,034,246,105 | 638,875,051 / 26,442,749 / 368,928,305 | 15 s |  |
| 16 | 480,828,346 | 359,253,462 / 11,912,828 / 109,662,056 | 7 s |  |
| 15 | 197,978,950 | 117,609,386 / 6,620,283 / 73,749,281 | 3 s |  |
| 14 | 86,367,214 | 64,540,126 / 2,761,337 / 19,065,751 | 2 s |  |
| 13 | 33,091,771 | 18,770,326 / 1,477,435 / 12,844,010 | 1 s |  |
| 12 | 13,497,518 | 10,118,891 / 564,293 / 2,814,334 | 0 s |  |
| 11 | 4,778,467 | 2,561,831 / 290,851 / 1,925,785 | 0 s |  |
| 10 | 1,810,196 | 1,366,137 / 99,571 / 344,488 | 0 s |  |
| 9 | 584,982 | 291,477 / 49,756 / 243,749 | 0 s |  |
| 8 | 203,588 | 155,127 / 14,952 / 33,509 | 0 s |  |
| 7 | 58,884 | 26,453 / 7,305 / 25,126 | 0 s |  |
| 6 | 18,480 | 14,328 / 1,777 / 2,375 | 0 s |  |
| 5 | 4,620 | 1,822 / 850 / 1,948 | 0 s |  |
| 4 | 1,260 | 1,002 / 155 / 103 | 0 s |  |
| 3 | 252 | 85 / 83 / 84 | 0 s |  |
| 2 | 56 | 45 / 9 / 2 | 0 s |  |
| 1 | 7 | 2 / 5 / 0 | 0 s |  |
| 0 | 1 | 0 / 1 / 0 | 0 s |  |

Storage: the run needs the working pair on fast storage, plus somewhere to put the finished
slabs. `--archive` moves each slab to a bulk disk as soon as it is no longer needed:

```
./c4 solve --board 7x7 --archive /bulk/c4_wdl_7x7      # resumable; re-run the same command
./c4 analyze --board 7x7 --dir /bulk/c4_wdl_7x7        # query the finished table
```

At the end, the archive directory is a complete board directory. Without `--archive`, the
4.1 TB free here lasts through about ply 42 (plies 42–48 are 2.15 TB), and `solve` refuses
to start a ply that cannot fit. The 7×7 tables are not published yet.

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
     solve flags: [--pack 2|3] [--ckpt-min M] [--archive D]
./c4 probe <moveseq>       # e.g. ./c4 probe 332   ('-' = initial position)
./c4 analyze               # initial position + per-move values
./c4 selftest | bfcheck [n] | reachtally | bigtally   # validation suite
```

Common flags: `--board WxH` (default 7×6; 4≤W≤7, 4≤H≤7), `--threads N`, `--dir D`,
`--archive D`. Tables are read and written under `c4_wdl_<W>x<H>/` **relative to the current
working directory**, so the solver tree can be moved without editing source; `--dir D`
overrides the location. Readers look for a slab in `--dir` first, then in `--archive`.

Solve-only flags:

* `--pack 2|3` — slab packing: 2-bit (4 values per byte) or base-3 (5 values per byte, 20%
  smaller). The default is 2-bit up to 42 cells (so 7×6 and smaller reproduce the published
  tables) and base-3 above. A base-3 directory is marked by a `PACK_BASE3` file, which every
  reader honours; a directory without it is 2-bit. Mixing packings in one directory is refused.
* `--ckpt-min M` — checkpoint progress *within* a ply every M minutes (default 15). Each
  checkpoint `msync`s the in-progress `.tmp` slab and then records which 327,680-position
  chunks are complete in `slab_{ply}.bin.ckpt`, so a killed solve loses at most M minutes,
  not the whole ply.
* `--archive D` — once ply k is solved, ply k+1 is no longer needed, so it is moved (copy +
  fsync + rename, then delete) to directory D in the background. This keeps only the working
  pair on fast storage. At the end, `slab_0.bin`, `zdd.bin` and the marker are added too, so
  D is a complete board directory usable with `--dir D`.

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
at the first missing ply, and from that ply's last in-ply checkpoint (see `--ckpt-min`).

**On reproducibility.** The slab files are bit-for-bit reproducible: a fresh solve
reproduces the `slab_*.bin` digests in `checksums/` exactly, regardless of thread count
(verified on 4×4). `zdd.bin` is not — the forest is built with lock-free hash-consing, so
racing inserts leave slightly different sets of equivalent nodes, and the file's size and
digest vary from run to run. That is harmless: rank is the lexicographic index of a position
within the represented set, which is canonical no matter how the nodes end up shared, and
that is precisely why the slabs still match. Every build also verifies its per-ply root
counts against the independent counting DP before saving. So compare rebuilt slabs against
`checksums/`, but expect a locally rebuilt `zdd.bin` to differ from the published one.

| board | state space (pseudo-legal non-terminal positions) | files | total size | notes |
|---|---:|---:|---:|---|
| 4×4 | 147,319 | 17 | 150,944 B | instant; `--brute` cross-check feasible |
| 6×4 | 70,538,337 | 25 | 22,212,456 B | second-player win |
| 5×5 | 52,213,470 | 26 | 20,637,575 B | draw |
| 7×6 | 2,637,477,442,337 | 43 | 660,138,684,135 B (615 GiB) | build 27 s + solve 8.9 h on 64 threads |
| 7×7 | 90,623,793,528,223 | 51 | 18,127,945,461,751 B (16.5 TiB) | base-3; build 190 s + solve ~10.6 days; draw |

For the full 7×6 run:

| | |
|---|---|
| Disk | ~683 GiB free: the 615 GiB table plus the in-progress `.tmp` slab (largest is 72.5 GB) |
| RAM | The ZDD forest (~1 GB) stays resident. Slabs are **file-backed** `mmap`s, so the OS pages them on demand — it will run with modest RAM but becomes page-cache bound; two adjacent plies are live at once (the largest pair, plies 31+32, is ~141 GB) |
| Threads | `--threads N`, default = hardware concurrency |

## Downloading and verifying the tables

The tables are hosted outside this repository. Each board is published as one
`c4_wdl_<W>x<H>.tar.zst` archive holding the whole board directory. The WDL data compresses
about 5.1×, so 7×6 is a 121 GiB download that expands to 615 GiB.

| board | archive | expands to | SHA-256 of archive | download |
|---|---|---|---|---|
| 4×4 | 60,410 B | 150,944 B | `3a821d960a03cac28733a2cd3d409729e14156401131cc360b2623d848ab57f5` | [Hugging Face](https://huggingface.co/datasets/eii/connect4/resolve/main/c4_wdl_4x4.tar.zst?download=true) |
| 5×5 | 9,181,148 B | 20,637,575 B | `3dfef541c9a6d0106b3ede3a9c5759501368af5b210ea3a54316631191e30743` | [Hugging Face](https://huggingface.co/datasets/eii/connect4/resolve/main/c4_wdl_5x5.tar.zst?download=true) |
| 6×4 | 8,878,048 B | 22,212,456 B | `89175d720be47d09205fd8f2aa06c08a8724221ea343ee073dfc579c11dcc6a9` | [Hugging Face](https://huggingface.co/datasets/eii/connect4/resolve/main/c4_wdl_6x4.tar.zst?download=true) |
| 7×6 | 129,585,865,603 B | 660,138,684,135 B | `b677bf3afa874f29d99ae1c2613d2dfe9854b3f6b13cceadd403cc0fad0866ab` | [Hugging Face](https://huggingface.co/datasets/eii/connect4/resolve/main/c4_wdl_7x6.tar.zst?download=true) |

Extract into this directory, which recreates `c4_wdl_<W>x<H>/` where the solver expects it:

```
echo "b677bf3afa874f29d99ae1c2613d2dfe9854b3f6b13cceadd403cc0fad0866ab  c4_wdl_7x6.tar.zst" \
  | sha256sum -c -
tar --zstd -xf c4_wdl_7x6.tar.zst
```

Or stream it, which never stores the archive — useful for 7×6, since it needs only the
615 GiB of extracted output rather than 615 + 121:

```
curl -fL 'https://huggingface.co/datasets/eii/connect4/resolve/main/c4_wdl_7x6.tar.zst?download=true' \
  | tar --zstd -xf -
```

The published archives unpack with stock `tar --zstd -xf` and `zstd -d`.

After extracting, `checksums/` holds a `sha256sum`-format manifest per board that verifies
the files individually — worth running on the 7×6 set, and the way to tell *which* slab is
bad rather than just that something is:

```
cd c4_wdl_7x6 && sha256sum -c ../checksums/c4_wdl_7x6.SHA256SUMS
```

The manifests cover `zdd.bin` too, so they fully verify an extracted download. They will not
match a locally rebuilt `zdd.bin` — see the note above.

Slab format: one value per position indexed by the ply's ZDD rank (0 = draw, 1 = win,
2 = loss, from the side to move's perspective). The published tables are 2-bit: value i is
bits `2·(i mod 4)` of byte `i/4`, and the file is padded to whole 8-byte words. Base-3
tables (directories containing `PACK_BASE3`, the default above 42 cells) hold 5 values per
byte: byte `i/5` = Σ v_j·3^j with j = i mod 5, file size ⌈n/5⌉ bytes.

### Rebuilding the archives

```
cd <parent of c4_wdl_7x6>
tar --zstd -cf c4_wdl_7x6.tar.zst c4_wdl_7x6
```

To check an archive's contents without unpacking 615 GiB to disk, stream each member
through `sha256sum` and diff the result against the manifest:

```
zstd -dc c4_wdl_7x6.tar.zst \
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
| `c4.cpp`, `Makefile` | complete standalone solver and build recipe (~1,600 lines C++20) |
| `README.md` | results, format documentation, build instructions, and command examples |
| `checksums/c4_wdl_*.SHA256SUMS` | per-file SHA-256 manifests for the externally published tables |
| `crosscheck.py` | random-position cross-check driver for the reference solver above |

The compiled `c4` binary, run logs, the generated `c4_wdl_*/` directories, and the cloned
reference repository are excluded by `.gitignore`.
