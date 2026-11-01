# TwixT symbolic solver: independent memory review — findings

Reviewer branch: `memory-review` in `/data1/twixt/work/smallgames` (based on
`bd214ff`; the baseline build is byte-identical to the production executable
`tables/twixt-21721f4`). The production run and its checkpoints were not
touched: every experiment used reflink copies (SHA-256 checked), separate
output paths, an address-space cap, and a guard (`tools/guard.py`) that
kills the experiment if host `MemAvailable` falls below 110–160 GiB.

## 1. Answer in brief

* **The temporary pool is legitimate intermediate-graph growth, not a leak.**
  Each placement image in the production code runs one filter pass, one
  `conditional_bit` pass per incident link, and a turn-flip pass. Every pass
  interns a complete result; at most a third of the nodes a job interns are
  reachable from its result. Nothing can reclaim the rest until the wave
  barrier, so the pre-GC pool reaches 4–6× the retained graph. The pairwise
  union then rebuilds the accumulated layer up to three times per wave.
  Reserved-ID gaps are < 1%.
* **RSS has owners the pool cap does not show.** Per pool slot, the intern
  registry (`std::unordered_map`) costs ~44 B on top of the 16 B node. The
  serial connection classifier `won()` keeps a ~104 B/entry frontier memo:
  705.6M entries at ply 15, i.e. ~73 GB in the production code. At ply 14
  (3–4 entries per node × 171M nodes ≈ 60–75 GB) that is consistent with the
  serial 113→188 GiB climb after ply 14 in the production log. Dense
  filter scratch is sized by the largest root ID per worker (12.8–13.5 B per
  ID of capacity), i.e. by pool size × workers rather than by work. glibc keeps
  freed memory in the freeing thread's arena between phases (up to 8.4 GiB
  free vs 1.4 GiB in use in a small run).
* **No classic leak.** At every quiescent census, glibc's in-use bytes equal
  the sum of the identified structures; after each collection the pool equals
  the retained graph; `malloc_trim` returns RSS to in-use.
* **Fixes implemented, all exact.** Fused placement image, fused preimage,
  compact classifier memo, compact intern registry, n-ary union, collection
  ordering, scratch/memo release, allocator trimming.
  * **The failed stage now completes** (6x6 ply 14→15, 4 workers, preserved
    checkpoint). With the final build, the transient pool peaks at **1.36B**
    slots, under the 2B cap that stopped all four production attempts, at
    53.9 GiB peak RSS, in 54:47 including the load. Production's classic code
    needed **3.43B** slots for the same ply (its 2B attempts failed; the 3.8B
    attempt's waves took 4:40 at 273 GiB) (§6 W3).
  * **5x6 retrograde at full scale** (32 workers): the official run's
    201.5 GiB / 1:41:06 becomes 22.5 GiB / 24:57 with the final build, with
    exact W/L/D equivalence to the official archive (§6 W4).
* **The retained graph itself will outgrow 32-bit IDs** a few plies later
  (retained pool ×1.81, ×1.82 at plies 14, 15; §7). A complete 6x6 solve needs
  per-layer storage or wider IDs regardless of the transient fixes.

## 2. Method

* `TWIXT_PROFILE=path` (opt-in; one branch when unset) logs:
  * the exact number of nodes interned by each step of each placement job
    (per-context `created` counters);
  * new nodes reachable from each step result (`TWIXT_PROFILE_REACH=1`);
  * a quiescent census at every wave barrier. It covers pool slots, unused
    all-zero slots (`TWIXT_PROFILE_SCAN=1`), discarded/unclaimed reserved IDs,
    registry entries/buckets/bytes, filter scratch, apply-memo and
    product-cache sizes, `mallinfo2` in-use/free (`TWIXT_PROFILE_MALLINFO=1`),
    and `/proc/self/statm`;
  * classification steps with memo sizes, and every `malloc_trim`.
* `tools/guard.py` runs an experiment under `RLIMIT_AS` and samples its
  process group's `/proc/*/status` (never `smaps`). It stops the experiment if
  `MemAvailable` drops below a floor. Whole-run peaks come from `/usr/bin/time -v`.
* `tools/analyze_prof.py` turns a trace into per-wave pool sources and an RSS
  owner table.

| ID | Workload | Workers | Reference for equivalence |
|:--|:--|--:|:--|
| W1 | 6x6 forward ply 8→10 from the preserved ply-8 checkpoint | 4 and 32 | baseline ply-10 checkpoint |
| W2 | 5x5 full strong solve | 8 | `5x5.twixtdfa` (exhaustively verified) |
| W3 | 6x6 forward ply 14→15 from `6x6-ply14-before-cap38` (the failed stage) | 4 | — (production has not finished this ply) |
| W4 | 5x6 retrograde plies 16→0 from the official resume checkpoint `5x6-before-buffer-release` | 32 | official `5x6.twixtdfa`, official run log |

## 3. Where the pool slots go (production code path)

W1, ply 8→9, 4 workers, 6 waves of 4 placement jobs (millions of slots):

| wave | retained | image filter | image cbit | image flip | image total | reachable from job results\* | union | gaps | pre-GC pool |
|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 0 | 3.3 | 0.0 | 0.8 | 2.5 | 3.3 | 7.2 | 0.1 | 0.01 | 6.6 |
| 1 | 5.5 | 0.0 | 3.5 | 2.5 | 5.9 | 2.6 | 0.3 | 0.01 | 11.8 |
| 2 | 6.5 | 0.4 | 10.6 | 4.9 | 15.8 | 5.0 | 1.0 | 0.01 | 23.2 |
| 3 | 7.3 | 2.9 | 18.3 | 6.4 | 27.6 | 6.5 | 2.1 | 0.01 | 37.0 |
| 4 | 7.5 | 6.7 | 14.2 | 7.6 | 28.4 | 7.6 | 3.9 | 0.31 | 40.1 |
| 5 | 7.5 | 9.1 | 15.5 | 9.4 | 34.1 | 9.4 | 3.6 | 0.00 | 45.2 |

\* per-job counts summed, so a node shared by concurrent jobs counts once per
job: an upper bound on what survives.

Over the ply: filter 16.5%, `cbit` 54.5%, flip 28.9% of image-created slots;
at most 33% are reachable from job results. A late move shows why. Ply 8,
move 20 creates per step: filter 0.59M; `cbit` 0.24, 0.27, 0.46, 1.56, 1.49,
1.49M; flip 1.61M. That is 7.71M created against 1.62M live. Each
`conditional_bit` whose link variable sits deep in the order rebuilds almost the
whole prefix. Only the flip's output survives.

The flip is not the waste: it creates exactly the image's genuinely new
nodes (move 20: flip 1.61M, live 1.62M). `--turn-sharing-images` on the
ply-10 checkpoint measures a turn-first variable order:

* image creation −5% (181.2M → 171.7M new nodes over 24 moves). The full-copy
  cost of the last-position turn bit is paid once per layer; later images
  reuse those suffixes.
* retained graph −20% (22.86M → 18.19M), from sharing between opposite-turn
  layers.

Moving the turn variable is therefore a modest, format-breaking gain.

Union, measured at W3 scale with the pairwise reduction (v3): union-created
nodes grow 15.8 → 26.5 → 75.7 → 171.3 → 320.1 → 363.2M over the six waves,
up to 42% of a wave's new slots. Yet the retained graph after wave 4's GC is
the same as after wave 3's (625.1M vs 625.4M). Almost all of them are
intermediates: `parallel_union` sorts each tuple by ID, so the old
accumulated union comes first and is rebuilt in every round of
`balanced_union`. With the n-ary union (F9), union-created nodes equal the new
nodes of the final result. W1 ply 8, wave 5: 0.4M created, 0.4M reachable
from the result, vs 3.6M created by the pairwise reduction.

Root cause in code (`21721f4`):

* `image()` (`twixt_symbolic.hpp:145`) and `preimage()` (`:164`) run
  2 + (incident links) separate DFA passes inside one job.
* GC may only run at `parallel_forward`'s wave barrier (`:405`), after
  `parallel_union` (`:326`) and `balanced_union` (`:309`).

## 4. RSS owners and lifetimes (production code path)

| Owner | Code (`21721f4`) | Size | Lifetime | Measured |
|:--|:--|:--|:--|:--|
| Node store | `NodeStore` | 16 B/slot, 1 MiB blocks | until GC | |
| Intern registry | `Manager::Bucket` (`symbolic_dfa.hpp:141`), `unordered_map<Children,Id>` | 32 B glibc chunk + 8–16 B buckets per slot | until GC | 42.4–44.3 B/slot in every W1 wave |
| Classifier memo | `won()` (`twixt_symbolic.hpp:184`), `unordered_map<FrontierKey,DfaId>` | 68 B key → 96 B chunk + bucket ≈ 104 B/entry. Entries per layer node: 2.3 (ply 9), 4.1 (ply 10), 3.3 (ply 15) | one serial `add_layer` | ply 10: 55.5M entries; 9.8 GiB RSS after the call vs 1.4 GiB in use. ply 15: 705.6M entries ⇒ ~73 GB |
| Filter scratch | `Context::filter_values/tags`, resized to `root+2` in `filter()` (`symbolic_dfa.hpp:372`) and `conditional_bit()` (`:414`), even for identity filters | 8 B/ID resident, 12.8–13.5 B/ID capacity, per worker | worker thread (since `21721f4`); coordinator's until GC | 767 MiB/worker at root 62.6M (W1 ply 9) ⇒ ~16 GB resident per worker at a 2B pool. Coordinator: 4.6 GB at ply 15 |
| Apply memo | `Context::apply_cache`, `apply()` (`:295`) | ~40 B/product pair | until a later call finds > 32,768 entries (`clear()` keeps buckets, `:301`) or GC | |
| Shared product cache | `ProductBucket` | ≤ 4,096 × 256 × 3 entries | until GC | ≤ 0.04 GiB |
| GC temporaries | `collect()` (`:504`) | live 1 B + mapped 4 B per slot, plus fresh graph and fresh registry | one GC; the old graph, old registry, and old contexts stay allocated during the copy | |
| Allocator retention | glibc arenas | — | until reused by the same arena | W1 post-GC: 0.4 GiB in use, 3.1 GiB free-but-resident. After ply-10 classification: 1.4 / 8.4 GiB |

**Leak check.** At every census in every trace, `mallinfo2` in-use equals the
sum of the identified structures, e.g. W1 ply 8 wave 5: estimate 2.9 GiB,
in use 2.9 GiB pre-GC; 0.4 / 0.4 GiB post-GC. RSS above in-use is free
allocator memory. `malloc_trim(0)` returns it: W3 34.3 → 18.7 GiB in 1.9 s.

**Reserved-ID gaps** (`nodes_.claim(64, …)`, `symbolic_dfa.hpp:260`):
0.2–0.3% of slots in the classic path; 0–4% per wave with the fused image,
which creates more shared suffix nodes concurrently. Not worth a change.

## 5. Fixes (exact; branch `memory-review`)

| # | Change | Where (branch) | Effect |
|:--|:--|:--|:--|
| F1 | **Fused placement image**: one memoized pass with a 3-bit state per incident link: condition still possible / false / "wrote 1, later crossings must be 0" / "wrote 0, a later crossing must be 1" / done. Applies the peg cube directly. No intermediate DFA is interned. | `fused_image()`; `parallel_forward`, serial forward | transient −55% (ply 8), −44% (ply 9); image phase 3.5× faster |
| F2 | **Fused preimage**: subset construction over (target node, link state) pairs. The two branches of an undecided link are disjoint, so no union/difference is interned. Cube applied to target symbols exactly as `filter(target, child cube)`. Retrograde jobs intersect with the unfiltered active layer, which is exact because the preimage already lies in the cube. | `fused_preimage()`; `parallel_retrograde`, serial retrograde | W4 vs the official run, per ply: time −66–72% and worker RSS −70–80% (with F3–F7); final build −81–84% and −86–90% |
| F3 | **Compact classifier memo**: intern frontier label arrays (7,918 / 40,317 / 103,387 distinct at plies 9 / 10 / 15) and key a flat table by (node, frontier ID), 12 B/slot. | `compact_won()` | ply 15: 705.6M entries in a 12.9 GB table instead of ~73 GB; 3× faster at ply 10 |
| F4 | **Compact intern registry**: open addressing over node IDs, keys compared in the node store, 4 B/slot. | `IdTable` | registry 44 → ~9 B/slot; GC 4–5× faster; ply-14 checkpoint loaded and counted in 229 s at 9.7 GiB (production binary: 536 s, 38.0 GiB) |
| F5 | **Collection order**: release the old registry, contexts, and product caches right after marking; copy nodes; free the old store; then build the fresh registry. | `collect()`, `release_registry_and_caches()` | GC no longer holds two registries plus scratch |
| F6 | Release apply memos with filter scratch at worker exit. Flat apply memo; free (not `clear()`) a memo above the reuse threshold. | `release_worker_filter_buffers()`, `apply()` | |
| F7 | `malloc_trim(0)` after each collection and classification (`TWIXT_NO_TRIM=1` disables). | `trim_heap()` | RSS returns to in-use between phases; ≤ 1.9 s at ply-15 scale |
| F8 | `filter()` returns early for an identity cube, so no root-sized scratch; coordinator scratch released after classification. | `filter()`, `add_layer()` | |
| F9 | **n-ary union**: one product traversal over sorted operand tuples. Interns only the final union's nodes. Pairs are memoized in a flat 64-bit table; longer tuples exactly in an arena `TupleMap` (each tuple stored once: 16 B entry + 4 B slot + 4 B per ID). | `Manager::unite_many()`, `TupleMap`; used by `balanced_union()` | vs v5 on W1, 4 / 32 workers: transient −15% / −46%; union −53% / −83%; wall −28% / −58%; RSS ±0 / −21%. A first version memoized tuples through a trie of prefix IDs (~4.2 entries per tuple): it raised RSS 73% on W1 and to ~88 GiB at W3 scale, so the arena matters. |
| F10 | Opt-in instrumentation and the turn-order diagnostic (`--turn-sharing-stats`, `--turn-sharing-images`). | `Profile`, `census()`, `turn_sharing_stats()` | |

The classic paths remain selectable with `TWIXT_CLASSIC_IMAGE=1`,
`TWIXT_CLASSIC_PREIMAGE=1`, `TWIXT_CLASSIC_WON=1`, and
`TWIXT_CLASSIC_UNION=1`. Archive and checkpoint formats are unchanged. The
patched binary resumes production checkpoints, and the production binary
resumes patched checkpoints; both directions were checked exactly.

Tried and rejected, with measurements on W1 at 4 workers:

* Merging the accumulated union last: union −15% but transient +22%,
  because the union of the new images becomes the intermediate.
* Eight union jobs per worker with dynamic scheduling: no faster, and +49%
  CPU from duplicated work across finer jobs.

## 6. Results

Builds:

* **baseline**: `21721f4`.
* **v2**: F1, F3, F5–F7.
* **v3**: v2 + F4.
* **v5**: everything except F9.
* **final**: all fixes (`tables/twixt-v10`).
* v9: F9 with a trie memo; superseded, shown where measured.

### W1: 6x6 forward ply 8→10

| Build, workers | Peak RSS | Peak VSZ (sampled) | Wall | Max transient pool (ply 8 / 9) | ply-9 union |
|:--|--:|--:|--:|--:|--:|
| baseline, 4 | 12.58 GiB | 12.74 GiB | 562.5 s | 45.4M / 86.1M | 111 s |
| F1 only, 4 | 10.40 | 10.58 | 416.4 | 20.4M / 48.1M | 133 |
| v2, 4 | 4.09 | 5.55 | 314.6 | 20.6M / 48.3M | 116 |
| v3, 4 | 2.80 | 3.69 | 191.4 | 20.5M / 48.2M | 84 |
| v5, 4 | 2.85 | 3.84 | 166.3 | 20.4M / 47.7M | 65 |
| v9 (trie memo), 4 | 4.93 | 5.15 | 122.2 | 17.3M / 40.7M | 39 |
| final, 4 | **2.83** | **2.84** | **120.2** | **17.3M / 40.7M** | **31** |
| baseline, 32 | 31.13 | 37.61 | 512.9 | 148.9M / 275.6M | 180 |
| v5, 32 | 5.67 | 6.50 | 155.3 | 68.9M / 156.8M | 75 |
| v9 (trie memo), 32 | 8.49 | 8.37 | 91.2 | 37.0M / 84.1M | 33 |
| final, 32 | **4.48** | **5.50** | **65.1** | **37.8M / 84.0M** | **13** |

Identical output every time: ply 10 has 1,095,842,760 positions in 13,461,035
states, and the retained pool is 22,855,962.

### W2: 5x5 full strong solve, 8 workers

| Build | Peak RSS | Peak VSZ (sampled) | Wall | Exact W/L/D vs reference |
|:--|--:|--:|--:|:--|
| baseline | 4.89 GiB | 5.27 GiB | 420.4 s | pass (8,388,375 nodes) |
| F1 only | 4.84 | 5.23 | 406.4 | pass |
| v2 | 3.88 | 4.75 | 388.4 | pass |
| v3 | 2.98 | 3.75 | 188.2 | pass |
| v3 + F2 | 1.39 | 1.64 | 150.1 | pass |
| v5 | 1.01 | 1.22 | 156.2 | pass |
| v9 (trie memo) | 1.14 | 1.19 | 128.1 | pass |
| final | **1.02** | **1.31** | **116.1** | pass |

### W3: 6x6 forward ply 14→15, 4 workers — the stage that failed

| Run | Outcome | Max transient pool | Peak RSS / VSZ | Wall |
|:--|:--|--:|--:|--:|
| production, 2B cap | pool cap hit, status 3 | > 2,000,000,000 | 240.5 GiB / 256.3 GiB (whole attempt) | failed after ~1:35 in this ply |
| production, 3.8B cap | waves completed 22:13 UTC; classification then running | **3,427,585,354** (90% of the cap) | 272.9 GiB worker phase; 326 GiB at the start of classification | 4:40:15 parallel phase alone |
| v3 (F1, F3–F7) | **complete** | **1,652,195,722** | **69.0 / 73.4 GiB** | **1:29:02** incl. 229 s load |
| final | **complete** | **1,363,086,986** | **53.9 / 63.7 GiB** | **54:47** incl. 233 s load |

v3 per wave: images 118, 116, 237, 337, 439, 664M slots; union 15.8, 26.5,
75.7, 171.3, 320.1, 363.2M; pre-GC pool 560, 673, 874, 1106, 1385, 1652M;
union 2,859 s and GC 328 s of the 4,160 s parallel phase. The largest image
jobs are the last cells (moves 31–34): ~165M nodes and ~175M memo entries
each, 180–210 s.

Final build per wave:

* union: 15.2, 18.5, 44.8, 83.6, 87.4M created (wave 4: 263 s vs 851 s for
  v3).
* pre-GC pool: 557, 665, 843, 1019, 1152, 1363M.
* parallel phase: 2,364 s (union 1,145 s, GC 273 s), vs 4,160 s for v3.

Classification of ply 15: `won()` 705.6M memo entries, 103,387 distinct
frontier states, 421–471 s; the `active` difference took 111 s (final) vs
244 s (v3). The estimate for the classic memo (705.6M × ~104 B ≈ 73 GB =
68 GiB) checks out: production's single-threaded ply-15 classification raised
its RSS from 252.1 to ~318 GiB (+66 GiB) between 22:16 and 22:48 UTC.

Both builds, and production's classic run, produce the same ply-15 line:
**ply 15 has 133,946,268,420 positions in 216,282,955 states
(119,721,473,528 active); retained pool 767,064,641.** The languages are
exactly equal (§6, Equivalence evidence). Checkpoints: `tables/v3-6x6-ply15.twixtckp` and
`tables/v10-6x6-ply15.twixtckp` (5.15 GB each, in the reviewer clone).

### W4: 5x6 retrograde plies 16→0, 32 workers

| Run | Peak RSS | Peak VSZ (sampled) | Wall | User CPU | Result |
|:--|--:|--:|--:|--:|:--|
| official (classic, 32 workers, dedicated) | 201.5 GiB | 252.9 GiB | 1:41:06 | 41,568 s | Black win |
| v3 + F2 (concurrent with production) | 40.4 GiB | 46.8 GiB | 35:31 | 16,464 s | Black win, **exact W/L/D equivalence** (172,546,664 nodes) |
| v9, trie memo (concurrent with production) | 23.1 GiB | 26.6 GiB | 25:20 | 11,224 s | Black win, exact W/L/D equivalence |
| final (concurrent with production) | **22.5 GiB** | **26.6 GiB** | **24:57** | 10,541 s | Black win, **exact W/L/D equivalence** (172,546,664 nodes) |

In every replay, each per-ply W/D/L count, DFA state count, and pool size is
identical to the official log (checked by `diff`). The archive has the same
172,546,666 nodes and 1,137,465,560 sparse bytes. It is not byte-identical:
node numbering follows creation order. Per-ply worker peak and wall
(official → v3 + F2 → final):

* ply 16: 131.4 → 39.4 → 18.3 GiB; 963 → 271 → 153 s.
* ply 15: 201.5 → 39.7 → 22.3 GiB; 1,111 → 375 → 210 s.
* ply 14: 185.5 → 28.6 → 17.7 GiB; 563 → 199 → 108 s.

### Equivalence evidence

* W2: exact node-for-node W/L/D equivalence with the exhaustively verified
  5x5 reference, for every build.
* W1: exact reachable/terminal/active equivalence of every build's ply-10
  checkpoint against the baseline's (22,855,960 nodes), at 4 and 32 workers.
* W4: exact W/L/D equivalence with the official 5x6 archive.
* W3: the final build's ply-15 checkpoint is **exactly equivalent to
  production's own ply-15 checkpoint**, written by the classic code at
  22:56 UTC (reachable/terminal/active, 767,064,639 nodes). It is also exactly
  equivalent to v3's, which used the pairwise union.
* `--self-test`: 9,768 randomized language checks on 5x5, 5x6, and 6x6
  boards, all comparing canonical IDs:
  * fused vs classic image and preimage, with and without random peg cubes;
    targets include children with corrupted incident, crossing, or arbitrary
    link bits;
  * the unfiltered retrograde intersection identity;
  * compact vs classic classifier;
  * n-ary vs pairwise union.
* `symbolic_dfa_test`: exhaustive set algebra, now including `unite_many`.
* `make test`: 64-thread parallel tests and exhaustive 3x3, 4x4, 3x5, 5x3,
  4x5 integration.
* AddressSanitizer + UBSan + LeakSanitizer build (`-O1 -g`): no reports on the
  unit tests, `--self-test`, a parallel 5x5 forward to ply 12, a 4x4 solve with
  every profiling option, and a full parallel 5x5 solve (4 workers, parallel
  paths forced from 2,000 nodes) that also passed exact W/L/D equivalence.
* ThreadSanitizer: it fails on this kernel with "unexpected memory mapping"
  unless ASLR is disabled for the process (`setarch $(uname -m) -R …`). With
  that, there are no reports on `parallel_dfa_test` or `--self-test`, and none
  on parallel 5x5 runs of the original code (forward to ply 10) or the final
  code (forward to ply 11). The patched code before F9's arena memo also ran a
  full parallel 5x5 solve with exact W/L/D equivalence and no reports. TSan
  found one real race in the reviewer's own diagnostics (a shared memo-size
  field), fixed by making it thread-local.

## 7. What still limits a complete 6x6 solve

Retained pool after each forward ply: 3.3M, 8.7M, 22.9M, 52.8M, 114.0M,
232.8M, 421.2M, **767.1M** (plies 8–15). Growth was 2.66× at first, has
fallen to 1.81× and 1.82×, and is no longer declining. Layer DFA sizes
alternate: ×2.26 at 13→14, ×1.27 at 14→15.

* **Even growth of 1.6–1.8× per ply crosses 2³² ≈ 4.29B retained nodes around
  ply 18–19**, before any transient. With the measured transient
  (1.36B pre-GC peak vs 767M retained after ply 15 with the final build), the
  ID space runs out around ply 17.
* In 5x6 the retained pool grew 14× from the equivalent point (ply ~11) to the
  end of forward, which suggests several billion nodes for 6x6.

The transient fixes buy roughly two plies. Finishing needs one of:

1. **Per-layer storage (recommended).** Forward generation needs only
   `active(k)`; retrograde ply k needs layer k's forward sets and layer k+1's
   W/L/D. Write each finished layer to its own archive and keep at most two
   layers live. This bounds the ID space by two layers plus transients and
   removes ~60% of the live graph at ply 14 (421M retained vs 171M in the
   current layer). Every wave GC copies only the live layers. Formats and the
   probe tool need a layer manifest.
2. **40-bit node IDs packed with the level in 16 bytes** (3 × 40 + 8 bits).
   Node memory is unchanged, the registry grows 1–4 B/slot, memo keys need
   128 bits, and the archive needs a new version. Per-node RAM is now
   ~25 B/slot, so 10B slots ≈ 250 GB, feasible on this machine.

Other items, in order of value:

3. **Classifier**: 3.3 entries per layer node at ply 15. Even compact, the
   memo will be tens of GB at plies 16–18, and the call is serial (471 s at
   ply 15). Parallelize it over a prefix split, as `parallel_union` does.
4. **Union**: still the slowest phase. F9 removes the intermediates; finer
   partitioning did not help.
5. **Workers**: per-job memory is now small, so 16–32 workers are affordable
   for images. The union at the barrier sees more operands per wave; F9
   handles that well (W1, 32 workers).
6. **Turn-first variable order** (format change): −20% retained, −5% image
   creation. Worth folding into a format change for item 1 or 2.

**Production.** The running attempt uses the classic code. It completed
ply 15 at 22:56 UTC: transient 3.43B slots, 90% of the 3.8B cap; the
classification added 66 GiB. It is now generating ply 16.

* Ply 15's transient beyond the retained graph was ~3.0B slots with the
  classic code and ~0.94B with the final build.
* If that part roughly doubles with the next layer, ply 16 needs ~6.8B slots
  with the classic code, beyond both the cap and 2³². The final build would
  need ~2.6B, inside the cap.

Production's new ply-15 checkpoint (or the reviewer's) resumes in the patched
binary without conversion. Whether to switch is the operator's decision.

## 8. Reproduction

```sh
cd /data1/twixt/work/smallgames/twixt
make && make test            # includes --self-test with the fused/compact checks
# W1 (set TWIXT_PROFILE=trace.prof TWIXT_PROFILE_MALLINFO=1 for a trace)
cp tables/6x6-initial-ply8.twixtckp tables/in.twixtckp
python3 tools/guard.py --csv tables/w1.csv --as-gib 160 --min-avail-gib 110 -- \
  /usr/bin/time -v ./twixt --resume-dfa tables/in.twixtckp --symbolic-ply 10 \
  --workers 4 --dfa-max-nodes 3800000000 --census-seconds 14400 \
  --dfa-checkpoint tables/w1-ply10.twixtckp
python3 tools/analyze_prof.py trace.prof
# W2
./twixt --size 5 --symbolic --workers 8 --dfa-max-nodes 200000000 \
  --census-seconds 3600 --compare-dfa tables/5x5.twixtdfa --dfa-out tables/w2.twixtdfa
# W3 (copy of the preserved checkpoint; final build ~55 min, ~54 GiB)
./twixt --resume-dfa tables/w3-ply14-input.twixtckp --symbolic-ply 15 --workers 4 \
  --parallel-memory-gb 512 --dfa-max-nodes 3800000000 --census-seconds 86400 \
  --dfa-checkpoint tables/w3-ply15.twixtckp
# W4 (copy of 5x6-before-buffer-release.twixtckp; final build ~25 min, ~23 GiB)
./twixt --resume-dfa tables/w4-input.twixtckp --workers 32 \
  --parallel-memory-gb 512 --dfa-max-nodes 1000000000 --census-seconds 86400 \
  --dfa-checkpoint tables/w4.twixtckp --dfa-out tables/w4.twixtdfa \
  --compare-dfa tables/5x6-reference.twixtdfa
# turn-order diagnostic on a forward checkpoint copy
./twixt --resume-dfa tables/copy.twixtckp --turn-sharing-images \
  --dfa-max-nodes 0 --census-seconds 3600
```
