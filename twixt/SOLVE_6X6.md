# 6x6 TwixT strong solve: engine changes, validation, and live status

Work on branch `memory-review` in `/data1/twixt/work/smallgames` (uncommitted).
It follows `MEMORY_REVIEW_FINDINGS.md`: the review removed the transient
blow-ups; this round removes the remaining limits on a complete 6x6 solve
(the 2^32 node-ID space, whole-solve residency, and serial phases). The old
production run (`/data1/twixt/smallgames`) was stopped with the owner's
permission; its checkpoints and the reference archives are untouched.

## Live status (2026-11-01)

The solve runs unattended from `tables/6x6-store` (a per-layer store), one ply
per step, driven by `tools/solve_driver.py` (budget 680 GiB, kill floor 20 GiB
free). It started from the calibration checkpoint `tables/cal-6x6-ply16.twixtckp`.

| ply | positions | active positions | DFA states | layer nodes | step |
|----:|----------:|-----------------:|-----------:|------------:|-----:|
| 16 | 260,903,902,297 | 234,956,377,731 | 406,021,348 | 620,679,693 | calibration, 6,212 s, 4 workers |
| 17 | 461,450,079,600 | 394,512,820,224 | 492,446,687 | 1,101,275,373 | 4,722 s, 24 workers, peak 480 GiB |
| 18 | 725,644,273,904 | 627,477,740,940 | 768,998,575 | 1,216,142,832 | 4,487 s, 24 workers, peak 409 GiB |
| 19 | 1,039,807,960,756 | 855,334,370,300 | 888,257,395 | 2,015,313,865 | 4,555 s, 24 workers, peak 248 GiB |
| 20 | 1,305,435,473,076 | 1,090,461,941,846 | 1,171,882,468 | 1,885,186,261 | 4,817 s, 64 workers, peak 284 GiB |
| 21 | 1,482,386,688,972 | 1,187,773,976,784 | 1,261,094,008 | 2,858,690,552 | 5,457 s, 64 workers, peak 319 GiB |
| 22 | 1,459,416,598,728 | 1,191,700,494,980 | 1,415,608,180 | 2,295,371,646 | 3,011 s, 64 workers, peak 361 GiB (union 1,284 -> 428 s, GC 453 -> 71 s) |
| 23 | 1,276,844,193,844 | 1,012,147,240,776 | 1,382,974,519 | 3,103,931,620 | 2,636 s, 64 workers, peak 348 GiB |

Ply 20 started at 07:13 UTC with 64 workers in one wave of 96 tasks
(`--single-wave`): its waves took 1,170 s, against ~1,700 s for ply 19's 24
tasks on a 1.66x smaller layer. Layer growth alternates: 1.77x, 1.10x, 1.66x
(plies 17-19). Ply 19's serial census line took ~1,000 s; it is now parallel.

```sh
cd /data1/twixt/work/smallgames/twixt
python3 tools/solve_status.py tables/6x6-store   # per-ply sizes, steps, current RSS
tail tables/6x6-store/driver.log                 # driver decisions
touch tables/6x6-store/STOP                      # stop after the current step
python3 tools/solve_driver.py --store tables/6x6-store --binary tables/twixt-solve \
  --budget-gib 680 --min-avail-gib 20 --forward-workers 64 --single-wave \
  --level-threads 48 --retro-bps 120 --sample-check 2000   # resume (reads manifest.twxman)
```

Stop processes by PID (`tables/6x6-store/driver.pid`) or process group, never
with `pkill -f` (it matches the controlling shell).

## What changed

1. **40-bit node IDs** (`symbolic_dfa.hpp`): 3 x 40-bit children plus an 8-bit
   level in 16 bytes; five-byte memo values. Needed in practice: the transient
   pool reached 4.86B slots at ply 17 and 6.65B at ply 18, beyond 2^32.
2. **Per-layer store** (`LayerStore`, `TWXMAN01` manifest): each finished
   forward or result layer is its own file; only the layers a step needs are
   live. The driver runs one ply per process.
3. **Level-by-level engine.** The connection classifier, the n-ary union,
   intersection/difference, the fused image, and the fused preimage now
   build their result one DFA level at a time. Phase 1 expands the distinct
   states of a level in parallel chunks, deduplicates the next level in lock
   shards, and spills one record per state; phase 2 replays the spill
   bottom-up and interns, keeping one level. Memory follows the widest level
   instead of a memo of every state; one shared table per level replaces
   per-slice memos that recomputed common suffixes.
4. **Layer format v2** (`TWXLAY01`, version 2): nodes grouped by level with a
   chunk directory, so writing and loading run in parallel. Version 1 files
   remain readable; the live store's layers 0-18 were converted
   (`--convert-layers`, swapped in with `tools/swap_layers.py`).
5. **Single wave** (`--single-wave`): `thread_jobs` runs waves larger than
   the thread count, so 64 threads share ~96 smaller tasks with one union and
   GC per ply (before, every `workers` tasks ended a wave).
6. **Parallel census**: the per-ply position counts and DFA sizes
   (`layer_census`) run level by level in parallel; a resumed layer's census
   line is not repeated.
7. **Driver** (`tools/solve_driver.py`): forward steps at a fixed 64
   workers; retrograde steps sized by a model fitted to 5x6 runs (peak ~100
   bytes per node of forward layer + next result layer, scaled by worker
   count) and recalibrated each step; retries with fewer workers after a
   memory failure; STOP file. `tools/relaunch_driver.py` restarts it between
   steps (optionally swapping in converted layers).

## Measurements (6x6 unless noted)

| operation | before | after |
|-----------|--------|-------|
| classifier, ply 15 (705.6M pairs) | 446 s serial, 43 GiB | 77 s, 22 GiB (24 threads) |
| classifier, ply 16 (2.09B pairs) | 1,458 s serial | 234 s, 37 GiB |
| classifier, ply 17 (1.56B pairs) | 1,305 s serial | 200 s |
| active + terminal-draw sets, ply 17 | 449 s serial | 75 s |
| union, ply 17 / ply 18 | 1,096 s, 478 GiB peak (slice union, ply 17) | 627 s, ~180 GiB (level union, ply 18, 2.01B tuples) |
| layer write / load, ply 17 (1.10B nodes) | ~550 s / ~560 s (v1) | 29 s / 71 s (v2) |
| 5x6 retrograde 16->0, store mode | 1,851 s | 1,058 s (level union + layer ops + v2) |
| image of ply 17, move 24 (616.6M pairs) | 354 s, 14 GB memo per job | 410 s single job, memory of the widest level |
| preimage of ply-17 terminal losses, move 14 (175.4M sets) | 208 s (memo) | 145 s (level) |
| 5x6 retrograde, 16 workers | 996 s, 12.8 GiB (level preimage, then intersection) | 751 s, ~5 GiB per ply (preimage restricted to the active layer in one pass) |
| 5x6 retrograde, 32 workers | 751 s (16 workers) | 607 s, single wave |
| census line, ply 19 | ~1,000 s serial | level-by-level parallel |
| 5x6 retrograde, 32 workers, under load | 693 s (64 lock shards) | 549 s (1,024 shards; union replay 20.1 -> 8.8 s), 503 s (+ parallel GC marking; merge/GC per ply 11-15 -> 4-5 s) |

In the real ply-18 step the move-24 image took 1,294 s, against 354 s in
isolation: 24 concurrent memo images are bound by memory latency. Level images
spill sequentially instead.

## Validation

Each build was checked before deployment:

* `make test`: exhaustive DFA algebra, 64-thread parallel tests, the self-test
  (12,210 language checks, now including level union/intersection/difference,
  level classifier, level image and preimage with 1-7 threads), and
  exhaustive 3x3-4x5 integration.
* 5x5 strong solves exact against `tables/5x5.twixtdfa`: store mode with node
  IDs based above 2^32, forced 5-byte layer IDs, every level path forced on, and
  `TWIXT_UNION=check` (every union also computed the old way and compared).
* ThreadSanitizer (run under `setarch -R`): full 5x5 solves with each build,
  0 warnings.
* 5x6 strong solve exact against `tables/5x6-reference.twixtdfa` with each
  build.
* Full-scale 6x6 equivalence: classifier at plies 15, 16, 17 against stored
  sets (identical pair counts); active/terminal-draw at ply 17; v1->v2 layer
  round trip of ply 17 (identical roots); level vs memo image and preimage for
  ply 17 (identical state counts); every converted layer reloaded with
  identical roots.
* **In the live run** (`--sample-check 2000`): before a solved ply is
  committed, 2,000 positions drawn uniformly from it are relabelled with the
  explicit engine (its own move generation, link rules and connection test)
  from the stored labels of their successors; a mismatch aborts the step
  without committing. Exercised on 5x5 and 5x6 (every ply consistent) and
  shown to catch a deliberately swapped W/L ply.
* The whole driver pipeline (forward steps, the forward/retrograde switch,
  retrograde steps, one process per ply) on 5x5 with the deployed binary and
  options: final store exact against the reference.

## Deployed binaries

| binary | sha256 prefix | used for |
|--------|---------------|----------|
| `tables/twixt-solve2` | a7d26a0ddd677a01 | ply 18 (level classifier and union) |
| `tables/twixt-solve3` | 7e39520078dba1ea | ply 19 (adds v2 layers, level set ops, level images) |
| `tables/twixt-solve7` | 9e64c6151e76d645 | plies 20-21 (adds level preimages restricted to the active layer, `--level-threads`, `--single-wave`, parallel census) |
| `tables/twixt-solve8` | 662b412f8a9c9ea1 | ply 22 (1,024 lock shards for the registry and level tables, parallel GC marking) |
| `tables/twixt-solve9` | a17ef64279269fdb | ply 23 (adds `--sample-check`) |
| `tables/twixt-solve10` | d166a4b241e76a5a | ply 24 on (census and sampling count by rank: 8 bytes per reachable node, not per pool slot) |

## Remaining risks

* **Retrograde memory.** Calibrated on 5x6 only: the driver starts at 120
  bytes per node of (forward layer + next result layer), scaled by worker
  count, and recalibrates from each measured step. A step that exceeds the
  budget fails fast (the solver's cap) and is retried with fewer workers.
* **Disk.** Level images and preimages spill per job (up to ~10 GB each, deleted
  when the job ends); 2.5 TB free.
