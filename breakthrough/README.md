# Breakthrough strong solver (ZDD minimal perfect hash + single-pass backward solve)

Strongly solves small Breakthrough boards, producing a complete win/loss table at **1 bit
per state**. Breakthrough has no draws, and the game is acyclic in a simple *advancement*
measure, so no fixpoint iteration is needed: one backward sweep per slab in the right order
values every state exactly once.

Boards are written `WxH` = **width × ranks**, with `2W` pieces per side filling the two home
ranks. Side 0 moves first, advancing toward rank `H-1`; side 1 advances toward rank 0.

## Results

| board | pieces/side | slabs | configurations (×2 stm) | states | table | solve time | initial position |
|---|---:|---:|---:|---:|---:|---:|---|
| 4×4 | 8 | 771 | 1,633,655 | 3,267,310 | 423 KB | 1.5 s | **second player wins** |
| 4×5 | 8 | 1,417 | 109,605,519 | 219,211,038 | 27 MB | 8.3 s | **second player wins** |
| 4×6 | 8 | 2,047 | 5,068,267,595 | 10,136,535,190 | 1.2 GB | 5.3 min | **first player wins** |
| 6×5 | 12 | 4,612 | 1,366,509,897,411 | 2,733,019,794,822 | 319 GB | 13.7 h | **second player wins** |
| 5×6 | 10 | 3,899 | 1,664,989,639,141 | 3,329,979,278,282 | 388 GB | 17.2 h | **first player wins** |

Times are wall clock on 2× EPYC 9115 (64 threads). Across this set the number of *ranks*
decides the outcome: every 6-rank board is a first-player win and every board with 5 or
fewer ranks is a second-player win — including 6×5, which has *more* pieces and more cells
than 5×6 yet flips the result. Five boards is too small a sample to call it a law, but the
6×5 / 5×6 pair is a clean controlled comparison: same 30 cells, opposite winner.

## Rules

Side 0 starts on ranks 0–1 and advances toward rank `H-1`; side 1 mirrors it. A piece moves
one rank forward, either straight ahead (only onto an empty square) or diagonally (onto an
empty square, or capturing an enemy piece). Reaching the far rank wins immediately; a player
with no legal move loses. There are no draws. Rules were checked against an independent
implementation (`BreakthroughGame.cpp` from a separate `dfa-games` tree, not included here).

## Build & commands

```
make                                   # g++ -O3 -march=native, C++20 + pthreads
./bt count     [--board WxH]           # ZDD forest + per-band state/byte counts, no solve
./bt solve     [--board WxH] [--threads N] [--dir D]    # full solve, writes slabs
./bt selftest  [--board WxH]           # solve, then compare sampled states to an
                                       #   independent negamax written from the rules
./bt spotcheck [--board WxH]           # re-check on-disk slabs (<=7 pieces) vs negamax
```

Defaults: `--board 5x6`, `--threads 64`, `--dir data`. Slabs are read and written under
`data/` **relative to the current working directory**, so the tree can be moved without
editing source; `--dir D` overrides the location.

`count` is worth running before a large solve — it reports the peak memory the solve will
need (two adjacent bands must be resident at once).

## State space and method

* **Slabs.** A state is (side-0 mask, side-1 mask, side to move). Configurations are grouped
  into slabs keyed by `(A, B, adv)`: piece counts per side and the *advancement* total
  `adv = Σ side-0 ranks + Σ (H-1 − side-1 ranks)`.
* **Acyclicity.** Every move either increases `adv` by exactly 1 (a quiet move, piece counts
  unchanged) or moves to a smaller piece-count partition (a capture). So processing `(A,B)`
  ascending by `A+B`, and `adv` descending within, means every successor is already solved:
  a single sweep per slab, no iteration, no distance bookkeeping.
* **Indexing.** One shared ZDD forest over all `(A,B,adv)` roots gives a minimal perfect hash
  per slab. Items are `cell*2 + side`; the frontier is (remaining A, remaining B, remaining
  adv, mid-cell overlap flag). Rank walks the ZDD accumulating 0-child path counts; unrank
  inverts it. The forest is small — 62,532 nodes for 5×6 — and is rebuilt in well under a
  second rather than stored.
* **Storage.** 1 bit per state (`0` = LOSS, `1` = WIN for the side to move), indexed by
  `configuration_rank * 2 + stm`. Terminal positions (goal reached, or a side with no
  pieces) are excluded from the space and recognized inline during the sweep.
* **Memory.** A state's successors lie in its own band (quiet moves) or the one below it
  (captures), so only two adjacent bands need to be resident: after band `T` finishes, band
  `T-1` is evicted. Peak memory is therefore the largest adjacent pair, which is what
  `./bt count` reports as `[peak] two adjacent bands`. Every slab is written to disk as soon
  as it is computed.

## Tables: what to download, what to rebuild

Slabs are written to `data/bt<W>x<H>_s<A>_<B>_<adv>.bin` and are **not** stored in this
repository — 708 GB across all five boards. They compress extraordinarily well, though: the
values are one bit each with no draws, and large regions are uniformly WIN or LOSS. All five
boards are therefore published, even the ones that are quick to rebuild, because the whole
set is a 1.75 GiB download.

| board | archive | expands to | ratio | SHA-256 of archive | download |
|---|---:|---:|---:|---|---|
| 4×4 | 33,203 B | 433,115 B | 13× | `06b86c3e127614202af60138c8a02ce050e0cc7e60e06dff878eecff7b29e8c4` | not published yet |
| 4×5 | 521,976 B | 27,446,765 B | 53× | `0b528c2cf885d974f5f4d8b635acff567ad7312d3d1e5962f0f3564a2c5fa033` | not published yet |
| 4×6 | 10,064,902 B | 1,267,132,465 B | 126× | `002ce04b580f57cbd855cc650abfc6a6f2a073699dbbcfa74ef8ba8cc215e932` | not published yet |
| 6×5 | 512,616,809 B | 341,627,622,858 B | 666× | `4401d67404b2da89a45415b36bdf175134c7599000d0621b6e55e60d8c67a5df` | not published yet |
| 5×6 | 1,353,357,885 B | 416,247,535,937 B | 308× | `60bf28b79ac06faa08231d58ba6a8db12c4aef83f07827e114ade6d1d1af225c` | not published yet |

All five archives together are **1.75 GiB** — the whole 708 GB of tables. The ratio climbs
with board size because larger boards have proportionally more deep, uniformly-decided
regions; 6×5 hits 666×.

Extract from this directory, which recreates `data/` where the solver expects it:

```
tar --zstd -xf bt5x6.tar.zst        # or: curl -sL <url> | tar --zstd -xf -
cd data && sha256sum -c ../checksums/bt5x6.SHA256SUMS
```

The archives are made with `zstd -12 --long=27 -T0`; the 128 MiB window is exactly zstd's
default decoder limit, so stock `tar --zstd -xf` works with **no extra flags**. Per-file
digests for every slab are in `checksums/bt<W>x<H>.SHA256SUMS`.

Rebuilding instead of downloading is cheap for the three small boards and expensive for the
two large ones. The solver is deterministic — a fresh solve reproduces the published slabs
byte for byte (verified for 4×4, 4×5 and 4×6: 771, 1,417 and 2,047 files, all identical):

```
make
./bt solve --board 4x4      # 1.5 s,   <1 MB RAM
./bt solve --board 4x5      # 8.3 s,   ~19 MB RAM
./bt solve --board 4x6      # 5.3 min, ~0.5 GB RAM
./bt solve --board 6x5      # 13.7 h,  ~101 GB RAM
./bt solve --board 5x6      # 17.2 h,  ~143 GB RAM
```

The RAM figures are the `[peak] two adjacent bands` value from `./bt count` — 108,126,433,735
bytes for 6×5 and 154,051,125,002 for 5×6. Those two boards need a large-memory machine;
everything else is comfortable on a laptop.

A solve is **resumable**: each slab is written as it completes, and a re-run loads any slab
already on disk instead of recomputing it. So an interrupted solve continues by re-issuing
the same command, and deleting a slab forces just that one to be redone. (Measured on 4×5:
8.0 s cold, 0.02 s with every slab present, 1.2 s after deleting 200 slabs — and the result
is byte-identical to the original either way.) Run `./bt count --board WxH` first to see the
peak memory requirement.

## Validation

* `selftest` solves a board and then compares sampled states, both sides to move, against an
  independent negamax written directly from the rules — no ZDD, no slabs, no ordering
  assumptions. 4×4: 61,680 states, 0 mismatches. 4×5: 113,360 states, 0 mismatches.
* `spotcheck` re-reads the finished slabs from disk for all partitions with ≤ 7 pieces and
  compares them to the same negamax, which exercises the on-disk format and the rank/unrank
  round trip rather than just the in-memory tables.
* The ZDD slab counts are cross-checked against the forest's own path counts at build time,
  and the per-band cumulative totals in the solve log sum to the configuration count.

## Files

| file | contents |
|---|---|
| `bt.cpp`, `Makefile` | complete standalone solver and build recipe |
| `README.md` | results, rules, method, formats, and command reference |
| `checksums/bt*.SHA256SUMS` | per-slab SHA-256 manifests for the externally published tables |

The compiled `bt` binary, run logs, and the generated `data/` slabs are excluded by
`.gitignore`.
