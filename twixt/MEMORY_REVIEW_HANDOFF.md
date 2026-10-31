# TwixT solver memory review handoff

Please independently review the symbolic solver's allocation, cache, and garbage
collection behavior, and identify changes that could make a complete 6x6 strong
solve practical. Distinguish genuine leaks from retained scratch space, allocator
retention, unavoidable live graphs, and temporary DFA products. Preserve exact
move semantics and W/L/D results.

**The confirmed recent failure is a configured node-pool limit, not a RAM-limit
failure.** Four 6x6 attempts exhausted a 2-billion-slot pool. A newer attempt with
a 3.8-billion-slot cap is running. A separate, earlier 5x6 `std::bad_alloc`
failure has an unresolved cause; a leak has not been established.

## Repository and machine

- Repository: `/data1/twixt/smallgames`, branch `main`.
- Solver directory: `/data1/twixt/smallgames/twixt`.
- Remote: `https://github.com/jh85/smallgames.git`.
- HEAD when this review was requested: `467cfb4`, which records the completed
  5x6 result. The latest solver implementation change is
  `21721f40dfa4794f9ec3e376e573ee3b7a424892`.
- The production executable is frozen at `tables/twixt-21721f4`. Rebuilding
  `./twixt` does not replace that executable.
- Dedicated machine: about 723 GiB physical RAM, 64 logical CPUs, 32 physical
  cores, two AMD EPYC 9115 sockets, and two NUMA nodes. The user described this
  as 750 GB RAM and 64 CPUs, all available to this project.
- `tables/`, binaries, archives, checkpoints, and logs are ignored by Git.
  A fresh clone will not contain the large inputs referenced below.

## Current run and preserved progress

Snapshot taken **2026-10-31 18:33:57 UTC**. Recheck the live log before acting.

The 3.8-billion-slot attempt started at 17:23:39 UTC and was still generating
ply 15. Solver PID was `358096`, with Linux process start ticks `58885740`.
Its controller uses 4 workers initially and retries with 2 and then 1 after a
handled resource failure. Each attempt has a 24-hour solver wall-time budget, a sampled
512 GiB RSS guard, and a kernel 640 GiB virtual-address-space limit.

```sh
cd /data1/twixt/smallgames/twixt
tail -n 40 tables/6x6-cap38-run.log
ps -p 358096 -o pid,etime,nlwp,pcpu,rss,stat,args
tail -n 5 tables/6x6-cap38-memory.csv
```

The active command is:

```sh
tables/twixt-21721f4 \
  --resume-dfa tables/6x6-working.twixtckp \
  --workers 4 --parallel-memory-gb 512 \
  --dfa-max-nodes 3800000000 --census-seconds 86400 \
  --dfa-checkpoint tables/6x6-working.twixtckp \
  --dfa-out tables/6x6.twixtdfa
```

The saved checkpoint contains complete **forward layers 0 through 14**:

| Measurement | Value |
|:---|---:|
| Positions across saved layers | 103,396,572,807 |
| Positions in layer 14 | 62,415,155,356 |
| Active positions in layer 14 | 58,467,071,424 |
| Nodes reachable from the layer 14 reachable root | 170,826,699 |
| Shared nodes retained across checkpoint roots | 421,230,724 |
| Checkpoint bytes | 2,826,210,114 |
| Checkpoint phase and cursor | 0 and 0, meaning forward generation |

`tables/6x6.twixtdfa` did not exist at the snapshot. Retrograde has not started,
and the empty-board W/L/D value is unknown.

The immutable recovery copy is
`tables/6x6-ply14-before-cap38.twixtckp`, with SHA-256:

```text
2ac4996d6a19c9a92f902e3643799d38183f224336cd111629a82ade56131da5
```

The working checkpoint may advance after this note. Its replacement is atomic;
the neighboring `.partial` file is not a completed checkpoint.

## How the earlier 6x6 attempts stopped

All four attempts below used the same solver implementation, a
2,000,000,000-slot pool cap, a 512 GiB RSS guard, and a 640 GiB virtual-memory
limit. Each exited with status **3**, reporting:

```text
Symbolic computation stopped: DFA node pool cap reached (includes intermediate results). No complete solution file written.
```

| Workers | Saved through ply | Failed generating ply | Elapsed | Peak RSS GiB | Recorded peak virtual GiB |
|---:|---:|---:|:---|---:|---:|
| 32 | 12 | 13 | 1:33:42 | 333.97 | 372.72 |
| 16 | 13 | 14 | 1:51:35 | 273.30 | 309.64 |
| 8 | 13 | 14 | 1:32:04 | 224.11 | 252.38 |
| 4 | 14 | 15 | 4:50:54 | 240.47 | 256.29 |

The controller stopped after exhausting its worker-count retries, at
**2026-10-31 16:33:49 UTC**. It did not write a completed WDL archive.
The launcher then printed `INCOMPLETE` and exited with status 1; the individual
solver attempts had exited with status 3.
Peak RSS comes from `/usr/bin/time -v`; recorded virtual peaks come from the
external counter log. These measurements do not show an OS OOM kill or a
512 GiB RSS-limit failure.

Relevant evidence:

- `tables/6x6-run.log`: combined original attempts.
- `tables/6x6-workers32.log`, `6x6-workers16.log`, `6x6-workers8.log`, and
  `6x6-workers4.log`: individual attempts.
- `tables/6x6-memory.csv`: original memory history, indexed by solver PID.
- `tables/6x6-cap38-run.log`, `6x6-cap38-workers4.log`, and
  `6x6-cap38-memory.csv`: current attempt and counters.
- `tables/run-6x6.sh` and `tables/run-6x6-cap38.sh`: exact launch policies.

The 4-worker run that successfully generated layer 14 reported
`transient_pool_nodes=1413113739` during forward ply 13, then retained
421,230,724 shared nodes after classification/collection. The 16-worker run
that generated layer 13 reported a transient pool of 1,926,053,194 slots but
retained only 232,785,031 nodes. These illustrate the gap between temporary
pool demand and the retained graph.

## Solver structure and memory accounting

This solves auto-link computer TwixT: White moves first, connects top to bottom,
and cannot occupy side borders; Black connects left to right and cannot occupy
top/bottom borders. Corners are absent. Legal links are added automatically and
cannot cross existing links. There is no pie rule or link removal. Link history
is part of the state; merging states by peg placement alone is unsound.

The solver generates reachable layers and then computes W/L/D backwards using
exact images, preimages, union, intersection, and difference on minimal layered
acyclic DFAs. The 6x6 encoding has 32 peg variables, 72 binary link variables,
and a turn variable. All forward layers remain live during generation.
Proven-dead draw descendants are retained. Solved layers release redundant
reachable/active/terminal roots during retrograde.

Three different quantities must stay separate:

1. **Positions** are accepted words, potentially billions or trillions.
2. **Retained nodes** are nodes reachable from all roots after collection.
3. **Pool slots** include retained nodes, unreachable intermediate results still
   owned by the interning registry, and unused reserved IDs. The configured
   `--dfa-max-nodes` limit applies to this third quantity.

IDs are `uint32_t`; zero and one are rejecting and accepting terminals. The
implementation cannot use more than roughly 4.29 billion IDs, even with the
configured cap disabled. The new 3.8-billion cap is below that ceiling but
permits values above `INT32_MAX`; audit narrowing conversions and serialization
if changing this area. Widening IDs also requires revisiting 32-bit archive and
delta references and the packed 64-bit `(Id, Id)` product keys.

## Code paths to inspect

Paths are relative to the solver directory. Function names refer to commit
`21721f4`; later line numbers may change.

| File | Functions or structures | Memory concern |
|:---|:---|:---|
| [symbolic_dfa.hpp](symbolic_dfa.hpp) | `NodeStore`, `claim`, `Manager::intern` | Stable 65,536-node blocks; 16-byte nodes; per-worker reservations of 64 IDs can leave gaps. Interning owns intermediate nodes until collection. |
| [symbolic_dfa.hpp](symbolic_dfa.hpp) | `Context`, `filter`, `conditional_bit` | Per-worker dense values/tags indexed by `root ID + 2`, rather than the number of nodes visited. Vector capacity can exceed size. `conditional_bit` also has a local hash memo. |
| [symbolic_dfa.hpp](symbolic_dfa.hpp) | `apply` | The 32,768-entry concurrent cache threshold is checked at call entry only. A single recursive operation can exceed it substantially. `clear()` also retains hash buckets. The shared product cache has an admission bound. |
| [symbolic_dfa.hpp](symbolic_dfa.hpp) | `collect` | Marks roots, allocates a pool-sized remap, builds a fresh graph/registry, then destroys old registries and contexts. Old and new graphs coexist during copying. |
| [twixt_symbolic.hpp](twixt_symbolic.hpp) | `image`, `preimage` | Multiple link assignments create intermediate graphs. No collection occurs between assignments inside one move transformation. |
| [twixt_symbolic.hpp](twixt_symbolic.hpp) | `placement_tasks`, `parallel_forward`, `parallel_union` | Jobs run in waves. Results are unioned before collection; no GC occurs inside `parallel_union`. Four workers currently use one peg shard and 24 placement tasks. |
| [twixt_symbolic.hpp](twixt_symbolic.hpp) | `collect_roots`, `maybe_collect` | Every live global and temporary root must be preserved across renumbering. Serial move generation can collect after individual moves; parallel generation collects at wave barriers. |
| [twixt_symbolic.hpp](twixt_symbolic.hpp) | `won`, `add_layer` | Connection classification is serial and has its own frontier-state memo. It can allocate outside the worker RSS-monitor loop. |
| [parallel_dfa.hpp](parallel_dfa.hpp) | `thread_jobs`, `resident_memory` | Production uses shared-memory threads. Workers have private contexts and release filter scratch when they finish. The POSIX fork backend is available but is not used by these runs. |
| [twixt.cpp](twixt.cpp) | options and outer exception handlers | `Limit` produces resource-stop status 3; `std::bad_alloc` reaches the generic error handler and produces status 1. |

At a root index of 500 million, two 32-bit dense scratch arrays require about
3.73 GiB per worker, or 238 GiB for 64 workers, before spare capacity or graph
storage. Root IDs can be large even when the particular filtered subgraph is
small. This is an identified scaling concern, not proof that it caused every
failure.

The printed worker `peak_RSS_GiB` is sampled during that worker computation.
It can miss larger union, classification, or collection peaks. Prefer the
external CSV and `/usr/bin/time` for whole-attempt peaks. The CSV records
`VmSize`, `VmPeak`, `VmRSS`, and `VmHWM` in **KiB**, every ten seconds, from cheap
process counters. Avoid frequent `smaps_rollup` scans; they previously disrupted
the solver.

## Earlier memory incidents and changes already made

**Expensive monitoring, fixed in `3548086`.** The original thread monitor read
`/proc/PID/smaps_rollup` every second. A live sample showed the coordinator using
about 99% of a core in kernel work with that file open while workers waited in
`do_mprotect_pkey`. The thread backend now reads RSS from `statm`, and scan
intervals start after a sample completes. The fork backend still uses aggregate
PSS because it shares pages by copy-on-write.

**Historical allocation failure, exact cause unresolved.** A 64-worker 5x6
attempt failed with `error: std::bad_alloc` during retrograde ply 17, exit status
1. Peak RSS was 412,104,960 KiB, about 393.01 GiB, while the launcher enforced
a 512 GiB virtual-memory limit. RSS below 512 GiB does not establish that
address space was available. That attempt did not capture its peak virtual
memory or failed allocation size, so an address-space-limit explanation remains
a hypothesis. See `tables/5x6-rss.log`.

**Retained worker buffers, improved in `21721f4`.** Finished threads used to keep
their dense filter arrays until the later collection. They now call
`release_worker_filter_buffers()` before exiting. A sequential 64-worker full
5x5 comparison reduced peak RSS from 17.59 to 15.84 GiB, with identical W/L/D
languages. Elapsed times were 313.38 and 330.25 seconds alongside a varying
production workload. This did not establish the historical allocation failure's
cause. See `tables/5x5-memory-old.log` and `tables/5x5-memory-new.log`.

**Leak evidence is limited.** Twelve repeated small workload/collection cycles
returned to 22 retained nodes each time, and ASan/UBSan/LeakSanitizer reported
no errors in that exercise. A 5x6 production trace also showed virtual memory
falling from about 330 to 85 GiB and RSS from 242 to 82 GiB after collection.
Those observations demonstrate reclamation, but do not exclude a leak specific
to large production operations. Relevant local files are
`tables/dfa-memory-audit.cpp`, its CSV and sanitizer logs, and
`tables/5x6-memory-history.csv` (use solver PID `252433` for that older trace).
ThreadSanitizer could not start on this host because of an `unexpected memory
mapping`; no ThreadSanitizer pass is claimed.

**Reserved-ID gaps are unproven as a major cause.** The small audit in
`tables/dfa-id-audit.cpp` counted about 48,000 slots with 373--548 unused slots,
roughly 1%. Its output is `tables/dfa-id-audit-before.log`. It does not establish
the gap fraction at billion-slot scale. In a quiescent store, an ID greater
than one with all-zero children is an unused slot: `intern` returns zero for
that child pattern rather than allocating a node.

## Review questions and useful measurements

- At the failing operation, how much space is retained graph, unreachable
  intermediate graph, reserved ID gaps, filter capacity, product-memo entries
  and buckets, or allocator retention? Measure these separately.
- Which placement, conditional link update, union, or classification causes the
  largest growth? Instrument operation boundaries and wave barriers, including
  failures, rather than attributing the whole layer to one stage.
- Can smaller jobs, better prefix partitioning, a different union schedule, or
  collection between transformations reduce the peak while preserving all live
  roots? GC must not renumber IDs while other workers or unregistered temporary
  roots still use them.
- Can filter scratch avoid scaling with the largest global ID? Can product
  memoization use a practical memory budget? Arbitrarily dropping memo entries
  may cause severe recomputation; benchmark both memory and time.
- Does the final retained graph itself approach the 32-bit limit, or is the
  immediate problem only transient working space? More node-ID width increases
  memory use and is not an automatic solution.

`filter_cache_bytes()` and interning-registry/container inspection require
quiescent workers unless synchronization is added. Preserve child-before-parent
IDs and minimal-DFA invariants; checkpoints and delta imports depend on them.

## Reproduction and validation

Leave the live attempt running during initial inspection. Use separate output
paths and immutable checkpoint copies. Two large jobs with independent 512 GiB
guards could exceed the machine's total RAM; schedule a full replay when the
current production job has finished or its resources have been reassigned.

Build and run the existing semantic, concurrency, and archive tests:

```sh
cd /data1/twixt/smallgames/twixt
make
make test
```

A smaller forward profile can start from scratch with capped resources:

```sh
./twixt --size 6 --symbolic-ply 9 --workers 4 \
  --dfa-max-nodes 200000000 --census-seconds 900 \
  --dfa-checkpoint tables/review-6x6-ply9.twixtckp
```

To replay the latest failed stage later, copy the preserved input and use the
old cap. This may take hours; it is not a quick unit test:

```sh
cp --reflink=auto tables/6x6-ply14-before-cap38.twixtckp \
  tables/review-6x6-ply14-input.twixtckp
(
  ulimit -v 671088640
  ./twixt --resume-dfa tables/review-6x6-ply14-input.twixtckp \
    --symbolic-ply 15 --workers 4 --parallel-memory-gb 512 \
    --dfa-max-nodes 2000000000 --census-seconds 14400 \
    --dfa-checkpoint tables/review-6x6-ply15.twixtckp
)
```

For optimized code, require semantic equivalence as well as memory improvement.
The existing 5x5 reference was exhaustively verified against explicit retrograde
for all 60,535,612 positions. A full comparison is:

```sh
./twixt --size 5 --symbolic --workers 8 \
  --dfa-max-nodes 200000000 --census-seconds 1800 \
  --compare-dfa tables/5x5.twixtdfa \
  --dfa-out tables/review-5x5.twixtdfa
```

The completed 5x6 archive is also a regression reference, but has not been
independently exhaustively verified against explicit retrograde. Its result is
Black win, covering 11,101,540,840 positions in 172,546,666 shared nodes and
1,137,466,072 bytes. See [results/5x6.json](results/5x6.json) for its checksum and
validation scope. Do not overwrite either official reference archive.

Please return a measured diagnosis, the suspected allocation owners and their
lifetimes, proposed exact fixes, before/after peak RSS and virtual memory, node
counts and runtime, and semantic-equivalence results. A conclusion that this is
primarily legitimate intermediate-state growth is useful too; do not assume a
classic leak merely because the memory graph rises.
