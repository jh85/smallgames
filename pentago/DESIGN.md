# Pentago Two-Tier Strong Solver — Design & Feasibility (self-compute edition)

Constraint: **no external data** (author unreachable; GCS bucket `gs://pentago-us-central1`
is private — verified 403; old backend server is dead; no mirrors exist).
Everything below is verified against `paper.pdf` (arXiv:1404.0743), the cloned repo, and
the author's `pentago/data/counts-*.npy` files.

## 1. Facts that constrain the design

- State space: 3,009,081,623,421,558 (3.0e15) symmetry-removed states.
- Storage unit: one *node* = a board with all 256 quadrant rotations abstracted into two
  256-bit tables (win, not-loss) = **64 B/node** → W/D/L for 256 rotated positions.
  nodes ≈ 1.054 × states / 256.
- DAG stratified by stone count ("slice n", n = 0..36). Slice n depends only on slice n+1.
  No cycles → single backward pass, no fixpoint iteration.
- Divergent: slice size grows to n = 24. From `counts-*.npy`:

  | slice(s) | states | uncompressed size |
  |---|---:|---:|
  | 17 | 2.9e13 | ~8.2 TiB |
  | 18 | 6.0e13 | ~14.5 TiB |
  | 23 + 24 (peak pair) | 9.0e14 | ~236 TB |
  | all 0–35 | 3.3e15 | ~850 TiB |

- Author's run: 49,152 cores, ~1.8 h compute for slices 35→19; peak memory 213 TB
  uncompressed → ~55 TB Snappy (measured ratio 0.26); kept only slices 0–18 = 3.7 TB
  on disk (zlib/LZMA per block; slices 17/18 measure ratio 0.12–0.16).
- Branching factor: avg 97.3 raw, **12.2 after rotation abstraction**; effective
  inter-slice branching after section/block-line structure: 4.
- Out-of-core I/O estimate from the paper: each block written once, read 4× → 3.6 PB
  uncompressed, 0.94 PB at Snappy 0.26.

## 2. Can we self-compute on 750 GB RAM + 6 TB disk? — **No**

The wall is storage at the middle slices, not time:

- While computing slice n, **all of slice n+1 must be randomly accessible** (each block is
  consumed by up to 4 parent block lines in pseudorandom order). Therefore at least two
  adjacent slices must be materialized at all times. The peak pair (23, 24) is:

  | encoding | peak pair size |
  |---|---:|
  | uncompressed | ~236 TB |
  | Snappy (measured 0.26) | ~61 TB |
  | zstd (est. 0.15–0.20) | ~35–47 TB |
  | LZMA (measured 0.12–0.13 on slices 17/18) | ~28–31 TB |

  Even at the best measured ratio, **~30 TB must coexist on disk** vs 6 TB available.
  LZMA is also too slow for bulk compression (weeks of CPU); zstd is the practical choice.
- Scheduling tricks cannot beat this by more than ~2×: storing every 2nd slice and
  recomputing intermediate slices on demand halves peak storage (~16 TB) for 2–4× compute —
  still 2.7× over budget, and engineering complexity explodes. No known encoding gets near
  6 TB: the WDL content of the middle slices is high-entropy (author's wavelet/low-rank
  compression experiments all failed — `pentago/notes`, `data/filter.h`).
- **ZDD / decision diagrams: no.** Same reason, sharpened by divergence: the graph is
  widest exactly where we'd need compression, the values lack the regularity DDs exploit,
  and DD random access is slow. The exploitable structure is symmetry (2048×) and rotation
  abstraction (branching 97.3→12.2), both already captured by the section/supertensor layout.
- Per-root downstream DP ("midengine") cannot substitute for bulk retrograde: computing
  slice 18 via per-root DPs is ~5e10 roots × ~30 s ≈ 50,000 core-years. Bulk retrograde is
  O(total states); root-DP multiplies by subtree size.
- RAM is **not** the bottleneck: an out-of-core engine needs only working buffers plus a
  block cache; 750 GB is generous (a ~600 GB LRU input cache absorbs most of the 4×
  re-reads).

## 3. What we need (pick one)

### Option A — this machine + ~50 TB HDD (recommended, cheapest)
- HDD is viable **only if the engine is redesigned for locality** — the "2 years" figure
  comes from the author's pseudorandom access pattern (~1.5e10 seeks × 10 ms), which exists
  for MPI load balance across 2048 nodes. A single machine doesn't need it:
  - Process **section by section**: input sections are then read near-sequentially, and
    each is reused ≤4× (one per quadrant placement). Largest peak section ≈ 1.8 TB
    uncompressed ≈ ~250 GB compressed → fits entirely in the 600+ GB RAM cache; most
    sections are far smaller. Net disk traffic ≈ write once + read once sequentially.
  - Sequential throughput: ~250 MB/s per drive; ~7–8 drives striped ≈ 1–1.5 GB/s.
    Total I/O ≈ 2 × (850 TiB × 0.15) ≈ 250–400 TB → **3–5 days of sequential I/O**,
    fully overlapped with compute. Random I/O must be engineered to ≈0 — any residual
    seek-heavy pattern (10 ms/seek) is fatal at this scale.
- Wall clock: compute-bound, **~1–2 months** (Edison ~9e4 core-hours; 64 modern cores with
  AVX-512 ≈ 2–3× per core). Same as NVMe — HDD only changes the I/O design, not the CPU work.
- Risks: months-long run on HDDs → need redundancy (RAID6) or per-slice checkpoints +
  checksums (already in the design) so a drive failure costs days, not the run.
  Do NOT fill beyond ~44 TB (peak pair ~35–47 TB zstd) — keep margin.
- Cost: ~$1–1.5K for 50 TB HDD. Middle option: **used enterprise NVMe** (e.g. PM9A3
  7.68 TB ≈ $240–575 each on Chinese markets → ~46 TB for $1.5–3.5K) removes the
  locality-engineering risk; needs U.2 ports/PCIe lanes.

### Option B — cluster or cloud, in-core (fastest wall clock, expensive)
- In-core needs the full ~61 TB (Snappy) peak pair in aggregate RAM, so cost scales with
  TB·hours regardless of node size: ~61 TB × ~3 days ≈ 4,400 TB·h.
- On-demand: user-verified GCP quote $340/h for 48 × 2 TB nodes → **~$14–16K** for 2–3 days.
  (An earlier $2–5K estimate was spot-pricing optimism — retracted.) Alibaba Cloud list
  prices for big-memory ECS are the same ballpark (r7 ≈ $0.009/GB·h → similar total);
  the savings are in spot/preemptible instances (advertised up to 90% off → potentially
  ~$4–6K) but 2 TB-RAM spot capacity is scarce and interruptible; per-slice checkpointing
  makes interruptions survivable, not painless. Chinese clouds do not change the order of
  magnitude for RAM-heavy on-demand work.
- Least new code (author's MPI engine nearly as-is), but still writes 3.7 TB back.

### Option C — one fat cloud instance
- 64–128 vCPU with ~60 TB local NVMe (storage-optimized families): ~1–2 months wall clock,
  ~$3–8K depending on provider/spot — only worth it if local hardware purchase is impossible.

Common requirement, all options: the two peak slices need ~30–47 TB of storage (or ~61 TB
RAM in-core). There is no configuration of the current 6 TB machine that avoids this.

## 4. Architecture (C++, reusing the BSD-licensed repo)

Reuse the hard, unit-tested parts unchanged: `pentago/base` (board/super/symmetry/section),
`pentago/data/supertensor.*`, `pentago/end` kernels (`compute.cc`, block/line structure),
`pentago/mid/*`. Do **not** use `pentago/mpi` (assumes in-core cluster).

New components:

1. **`retro-ooc`** — out-of-core backward engine (the big piece):
   - Per slice: enumerate sections (`end/sections`), partition block lines pseudorandomly
     (`end/random_partition` logic, single-machine variant) for cache-friendly ordering.
   - Async I/O scheduler (io_uring): request input block lines from slice n+1 (zstd blocks
     on NVMe), compute with the existing SSE kernel upgraded to AVX2/AVX-512 (process two
     256-bit supers per instruction), combine ≤4 line contributions per output block,
     compress, append to slice-n file. 64 worker threads + I/O threads.
   - Checkpoint per slice (the author's restart mechanism: `--restart slice-N.pentago`).
   - Slices 35→19: scratch on NVMe, delete slice n+1 once slice n is complete.
     Slices 18→0: final zstd/LZMA output to the 6 TB disk (3.7 TB).
2. **`wdl-db`** (tier 1): lookup over `slice-0..18.pentago`; standardize board under G,
   range-read + decompress one block, LRU cache of uncompressed blocks in RAM.
   µs warm / ms cold. (Unchanged from before.)
3. **`mid-solve`** (tier 2): parallelized port of `pentago/mid` for >18-stone positions
   (~0.5 GB, ~3 s single-core → sub-second on 64 cores). Not alpha-beta — exact search from
   18 stones is ~12^18 ≈ 1e19 leaves; the author tried and abandoned forward search.
   Alpha-beta survives only as an optional heuristic front-end.
4. **`verify`** — first-class, since we compute everything ourselves:
   - per-slice section counts vs Pólya enumeration (`base/count.h` → `counts-*.npy`);
   - `end/check.cc`-style forward-search cross-checks on random boards per slice;
   - paper's slice-4/5 randomization test comparing backward vs forward engines;
   - tier-2 vs tier-1 agreement on random 18-stone positions;
   - per-block CRCs in every file.

## 5. Milestones (Option A)

1. Build repo; reproduce `bin/analyze counts`; port/verify kernels + AVX-512 variants
   against existing unit tests. (~1 wk)
2. `retro-ooc` engine skeleton; validate slices 35→30 on current hardware (these are small:
   slice 35+34 ≈ 14 TiB uncompressed, ~2 TB compressed — fits today's disk). (~2 wks)
3. Storage upgrade; run slices 35→19 (the long haul, ~3 wks–2 mo).
4. Slices 18→0 → final 3.7 TB DB on the 6 TB disk; full verification harness. (~1 wk)
5. `wdl-db` + `mid-solve` + CLI; boundary cross-validation at 18 stones. (~2 wks)

Interim win: after milestone 2–3 we can validate the engine against the author's published
counts and sparse samples (`sparse-*.npy`, in-repo) for every slice — strong independent
evidence of correctness before the run even finishes.

## 6. Bottom line

- 750 GB / 6 TB as-is: **cannot** strongly solve Pentago. Storage wall: ~30 TB minimum
  (best measured compression) for the two peak slices vs 6 TB; no algorithmic trick
  (ZDD, rescheduling, per-root DP) closes a 5× gap.
- Cheapest sufficient upgrade: **+50 TB HDD on the same machine (~$1–1.5K)** with a
  locality-aware (section-sequential) out-of-core engine — ~1–2 months, compute-bound;
  the 750 GB RAM caches whole input sections. Used enterprise NVMe (~$1.5–3.5K via Chinese
  markets) buys engineering simplicity, not more speed. Cloud in-core is ~$14–16K on-demand
  (GCP or Alibaba — same ballpark), ~$4–6K only with aggressive spot and interruption risk.
- The two-tier product is unchanged: tier 1 = slices 0–18 (3.7 TB, fits the 6 TB disk),
  tier 2 = per-root retrograde DP for >18 stones (never alpha-beta for exact values).
