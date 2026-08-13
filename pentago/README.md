# Pentago strong solver (two-tier, single-machine, out-of-core)

A WDL (win/draw/loss) strong solver for Pentago, designed to run on a single
memory-tight machine (target: 750 GB RAM, 50–60 TB HDD, 64 cores), as opposed
to the original 2014 solution by Geoffrey Irving which required ~80 TB of
cluster memory (see `paper.pdf`, arXiv:1404.0743, and `DESIGN.md` for the full
feasibility analysis).

Pentago is divergent: the state space grows until 24 of 36 stones are placed
(peak slice pair ≈ 236 TB uncompressed, ≈ 30 TB with the best measured
compression).  This makes the middle slices the bottleneck; see `DESIGN.md`
for why ZDDs and alpha-beta do not help, and why the machine needs the HDD
upgrade.

## Architecture

- **Tier 1: WDL database.** Retrograde-computed slice files (`slice-N.pgs`,
  one per stone count N = 0..35; slice 36 is trivially terminal).  Values are
  stored rotation-abstracted (256 quadrant rotations per node, 64 B/node) in
  8×8×8×8 blocks, zlib-compressed, with per-block CRC32 — see
  `src/slice_file.h` for the format.  Only symmetry-minimal sections are
  stored (full symmetry group |G| = 2048).
- **Tier 2: on-demand exact solver.** Positions with more stones than the
  highest computed slice are solved per-root by downstream retrograde DP
  (girving's midengine, ~0.5 GB RAM, seconds for 18 stones).  Alpha-beta
  cannot do this exactly: from 18 stones the tree is ~12^18 ≈ 1e19 leaves.

The engine (`src/engine.cc`) is locality-aware for HDDs: sections are computed
in a fixed order, each output section touches only its ≤ 4 child sections of
the slice above (so the input block cache stays hot and disk reads are
near-sequential), and slices are checkpointed one file at a time (automatic
restart on rerun).

Compute kernels, symmetry/section machinery, and the midengine are reused from
Irving's BSD-licensed code (the `girving/` submodule, pinned commit); the
out-of-core engine, file format, and tools here are new.

## Build

Requires g++ (C++20), zlib, pthreads.  No Bazel/MPI needed.

```sh
git submodule update --init   # fetch girving/pentago
make -j
```

## Tools

- `build/retro --dir DIR --hi 35 --lo 18` — compute slices hi..lo (resumable).
  Options: `--cache GB` (input block cache), `--line-memory GB`,
  `--section-memory GB`, `--level` (zlib), `--threads N`.
- `build/wdl --dir DIR BOARD_HEX [--moves]` — exact value of a position
  (and optionally all moves) from the database.
- `build/midsolve BOARD_HEX` — exact value of a ≥18-stone position from
  scratch (tier 2).
- `build/verify_counts SLICE_PGS girving/data/counts-N.npy` — validate a
  computed slice against the author's Pólya-enumeration counts (structure +
  per-section win/loss totals).
- `build/verify_forward ...` — independent brute-force WDL cross-check
  (`board HEX` | `sample PGS N` | `exhaustive PGS`; exponential — small
  slices / high slices only).
- `build/verify_sparse SLICE_PGS SPARSE_NPY` — validate against sparse
  samples in the author's format (for use if his sample files are obtained).

`BOARD_HEX`: 16 hex digits, four radix-3 quadrants (quadrant 0 in the low
bits; digits empty/white/black = 0/1/2).

## Status and roadmap

Working and validated (see "Verification" below): the full engine pipeline on
the highest slices (35, 34, ...), the slice file format, restart logic, and
the verification tools.

Current limitations / next milestones:

1. **Whole-section RAM buffer.**  v1 holds each uncompressed output section in
   RAM (`--section-memory`, default 200 GB).  The few monster sections near
   the peak (largest ≈ 1.8 TB uncompressed, slices ~23–27) need the planned
   slab/chunked mode (process output sections in chunks; child sections cached
   compressed).  Everything else fits.
2. **Compression.**  v1 uses zlib.  Switching block compression to zstd
   (better ratio at much higher speed) is a small, localized change.
3. **AVX2/AVX-512 kernels.**  The SSE `rmax`/`transform_super` kernels work;
   widening them is worth ~2×.
4. **I/O overlap.**  Input block fetching is currently on the main thread;
   moving it to I/O threads with readahead will hide HDD latency.

## Verification

- `verify_counts` checks every computed slice against the author's
  independently computed `counts-N.npy` (per-section black-wins / white-wins /
  total, Pólya-weighted).  Slices 35 and 34 match exactly (all sections,
  grand totals identical), which exercises both the terminal path (35) and
  the full child-slice input machinery (34).
- `verify_forward mid` cross-checks random nodes against girving's
  independent midengine (500/500 on slice 35); `verify_forward board` is a
  plain recursive solver for single positions.
- `midsolve` (tier 2) and `wdl --moves` agree on overlapping inputs.

## License

New code here: same license as the smallgames repo.  `girving/` is BSD
licensed (Geoffrey Irving).  `third_party/`: tinyformat (Boost license),
Random123 (BSD).
