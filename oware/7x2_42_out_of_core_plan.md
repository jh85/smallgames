# Plan: solving 7×2 Oware with 42 seeds (3 per pit) on the 723 GiB machine

Status: design only, written 2026-09-22. Nothing below is implemented yet. The
solver already supports `OWARE_ROW=7` (`build/solve7`) and a run-time seed count; the
7×2/28 game was solved with it in 31 minutes. This document records why 7×2/42 does
not run as the solver stands, and the design that makes it fit.

## 1. Size of the problem

Exact counts from the closed form (`tests7` checks the total against `Index`):

| n | boards | V | P | bytes/board | table |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 30 | 34,266,058,568 | 15 | 120 | 7/8 | 30 GB |
| 32 | 67,591,258,749 | 13 | 91 | 7/8 | 59 GB |
| 34 | 128,643,625,565 | 11 | 66 | 7/8 | 113 GB |
| 35 | 175,308,172,936 | 10 | 55 | 3/4 | 131 GB |
| 36 | 237,078,052,484 | 9 | 45 | 3/4 | 178 GB |
| 37 | 318,283,670,432 | 8 | 36 | 3/4 | 239 GB |
| 38 | 424,344,643,268 | 7 | 28 | 5/8 | 265 GB |
| 39 | 562,007,350,555 | 6 | 21 | 5/8 | 351 GB |
| 40 | 739,626,735,680 | 5 | 15 | 1/2 | 370 GB |
| 42 | 1,258,254,741,654 | 3 | 6 | 1/3 | 419 GB |
| **all** | **4,161,983,837,529** | | | | **2,356 GB** |

(V = number of clamped value levels, `SEEDS + 3 - n` for n ≥ 22; P = V(V+1)/2 pairs;
layer 41 does not exist — a single seed can never be captured.) 22 captured seeds win,
21–21 is the draw by score. The 48-seed 6×2 game, for comparison, has 8.9 × 10¹¹ boards
and 516 GB of tables, and took 17.5 h of solve + verify on this machine (64 threads).

## 2. Why the current solver cannot run it

* `solve` keeps every finished layer in anonymous memory and reads lower layers at
  random when a move captures. For 7×2/42 that is 2.36 TB against 776 GB (723 GiB) RAM.
* Layer 42 alone needs 419 GB of values plus a 157 GB dirty bitmap while it is solved.
  What is left (~140 GB) cannot cache layers 36–40 (1.4 TB), which every capture from
  layer 42 reads.
* Plain `mmap` of the lower layers does not help: the reads stay random, single-byte,
  ~9 × 10¹¹ per full sweep, several full sweeps per layer. At NVMe page-fault rates
  (≤ 10⁶/s) that is ~10 days per sweep, months per layer; the HDD is out of the question.
* Compression does not close the gap either. Measured on the finished 48-seed layers
  (2 GB samples, zstd -3): layer 40 compresses 1.4×, layer 44 1.6×, layer 48 2.7×.
  Layers 36–40 compressed in RAM would still be ~0.9 TB on top of the working set.

## 3. Design: turn capture lookups into a sequential pass

### 3.1 Observation

Lower layers are final when layer n is solved, so a board's best capture never changes
during the fixpoint iteration. Define, for each board b of layer n,

    capM(b) = max over capturing moves i of  cap_i + gN(child_i)      (0 if none)
    capN(b) = min over capturing moves i of  gM(child_i)              (V-1 if none)

Then the in-layer fixpoint only needs same-layer children:

    gM(b) = max(capM(b), max over non-capturing moves of gN(child))
    gN(b) = min(capN(b), min over non-capturing moves of gM(child))

`capM` is folded into the initial value of the layer (values only rise, and it is a
sound lower bound), so only `capN` needs a new per-board array.

### 3.2 Why the capture pass can be sequential

A child's **opponent row is the parent's mover row after sowing**, whatever the capture
is. In the index, `rank = base[n'][k'] + rankZ(opp row) * cntA[n'-k'] + rankA(mover row)`,
so all boards of layer n' with a given opponent row R' occupy one contiguous run of
`cntA[n'-k']` entries (≤ C(46,6) ≈ 9.4 M boards, ≤ ~9 MB). All capturing children of all
parents whose sowing produces R' lie in the runs (n', R') for n' = n-2 … n-20 (a
capture takes 2 or 3 seeds from each of up to 7 pits; a grand slam captures nothing).

So the pass iterates over R' instead of over parents:

```
for each opponent row R' in index order (k' ascending, then rankZ):      # parallel over R'
    load the runs (n', R') for every lower layer n' that can be reached  # ≤ 20 runs, ≤ ~100 MB
    for each pit i with R'[i] == 0, for each s = 1 .. n - |R'|:          # un-sow
        M = R' with M[i] = s and the sown seeds removed from pits after i (laps included)
        if impossible (some pit would go negative) break
        if move i is not legal from M continue
        for each opponent row O with |O| = n - |M| (all cntZ[n-|M|] of them):
            cap = play((M, O), i)          # child's opponent row is R' by construction
            if cap == 0 continue           # same-layer child: handled by the sweeps
            v = value of child in run (n - cap, R') at rankA(child mover row)
            parent index x = base[n][k] + rankZ(O) * cntA[n-k] + rankA(M)
            gM[x]  = max(gM[x],  clamp(cap + gN(v)))     # atomic byte CAS (packed)
            capN[x] = min(capN[x], clamp(gM(v)))         # atomic byte CAS (packed)
```

* Each (parent, pit) pair is enumerated exactly once (R' is a function of (M, i)).
* Processing R' in index order makes the run positions in every lower-layer file
  monotone, so the reads are sequential streams over ≤ 20 files. Every lower-layer byte is
  read once per top layer: ~2.2 TB for layer 42, 15–30 min from the NVMe RAID.
* CPU: ~7 × 1.26 × 10¹² `play()` calls for layer 42, about 1–2 h on 64 threads. Only the
  ~10 % that capture touch the parent arrays (random RAM writes, ~10¹¹, ~30 min).
* The parent enumeration for a fixed (M, i, s) walks all O in rank order, i.e. a stride
  of `cntA[n-k]` through the working layer. That is random RAM access, which the sweeps
  already do; huge pages keep TLB cost down.
* Threads take disjoint ranges of R'. Two threads can still update the same packed byte
  (different parents sharing a byte), hence CAS on the byte.

### 3.3 In-layer sweeps

Unchanged in structure (dirty-driven chaotic iteration, then a full verification pass),
with `evaluate()` using `capM`-seeded values and `capN` instead of reading lower layers.
Lower layers are **not** in memory during the sweeps at all. The final pass also packs the
layer, checks the fixpoint and writes the file exactly as today, so the file format and
`probe7` are unaffected.

### 3.4 Memory budget (layer 42 is the worst)

| item | representation | GB |
| --- | --- | ---: |
| values gM/gN, layer 42 | RADIX, 3 boards/byte | 419 |
| capN | 5 boards/byte (3⁵ = 243) | 252 |
| dirty bits | 1 bit per **8** boards (today: per board, 157 GB) | 20 |
| Index row tables (ROW = 7, MAX_SEEDS = 42) | | 1.5 |
| **total anonymous** | | **~693** |

Physical RAM 776 GB (723 GiB); the kernel and everything else must fit in the rest.
Nothing else memory-heavy may run during layers 40 and 42 (the Breakthrough solver's
34 GB would not fit). Page cache for the streaming pass is reclaimable and not counted.

Other top layers: 40 → 370 + 247 (3/byte) + 12 = 629 GB; 39 → 351 + 187 + 9 = 547 GB
**only if solved in the 5-bit BITS format** (the current 1-byte RADIX working format would
be 562 GB for values alone); 38 → 265 + 212 (2/byte) + 7 = 484 GB; 37 → 239 + 159 + 5 =
403 GB; 36 and below fit easily.

`capN` packing per V: V = 3 → 5/byte, V = 4 → 4/byte, V = 5..6 → 3/byte, V = 7..16 →
2/byte, else 1/byte.

### 3.5 Changes to the code

1. **Capture pass** (new, `solve.cpp`): the loop of 3.2, with lower-layer runs read through
   `mmap` + `madvise(MADV_SEQUENTIAL)` (or `pread` into a per-thread buffer) and released
   as R' advances. Reuses `forPredecessors`' un-sowing arithmetic.
2. **`capN` array** with packed get/CAS-set; `evaluate()` takes it instead of `L[n - cap]`.
3. **BITS as working format** for layers whose RADIX working format has 1 board per byte
   (P > 16): `Layer::set()` must read-modify-write only the byte(s) that contain the field
   (1 byte when the field lies inside one byte, 2 bytes otherwise). Block boundaries are
   byte-aligned (`BLOCK_WORDS` × 64 boards × w bits), so this removes the cross-block race
   that currently forces RADIX for work.
4. **Dirty granularity** of 8 boards per bit (a dirty group re-evaluates 8 boards). Later
   sweeps do ~8× more evaluations, but they are small; sweeps 0 and 1 are full anyway.
5. **Memory accounting**: the solver decides per layer between the present all-in-RAM mode
   (small n) and the streaming mode, from the actual sizes; a command-line switch forces
   either for testing.
6. **Lower layers are files only** in streaming mode: `L[n']` for n' < n is not allocated.

### 3.6 Validation

* Streaming mode forced on 6×2/36 and 7×2/28 must reproduce the existing tables
  **byte for byte** (`SHA256SUMS` in `checksums/`), and on 6×2/48 layers ≤ 40 against
  `/mnt/oware-wdl` (the solver's own `stats.txt` lines must match too).
* `tests`/`tests7` and the OpenSpiel cross-check as today.
* The built-in fixpoint verification stays on for every layer.

### 3.7 Storage

All 2.36 TB of tables on NVMe during the run; captures reach up to 20 seeds down, so at
layer 42 the layers 22–40 (~2.2 TB) are streamed. `/data1` (md0 of two 3.6 TB SN850X)
had 1.4 TB free on 2026-09-22, so about 1 TB more must be freed (target ≥ 2.4 TB free).
The 48-seed and 36-seed tables can stay on `/mnt` (HDD); they are not involved. Finished
layers no longer needed can be moved to the HDD afterwards.

### 3.8 Time estimate

Per top layer: capture pass 1–2 h, sweeps and verification scaled from 6×2/48's layer 48
(2.0 × 10¹¹ boards, 5,049 s + 1,466 s) by boards × ~1.2 for the wider board, plus the
coarser dirty bits: layer 42 ≈ 1 day, 40 ≈ 0.6, 39 ≈ 0.45, 38 ≈ 0.35, 37 ≈ 0.25, 36 ≈ 0.2,
everything below ≈ 1 day together. **About 5–7 days of run time**, after 3–5 days of
implementation and validation.

### 3.9 Risks and fallbacks

* **Memory margin at layer 42 (~80 GB).** If it does not fit: drop the dirty bits entirely
  and run full sweeps only (layer 42 then costs ~2.4 h per sweep × ~50 sweeps ≈ 5 days
  instead of 1); or pack `capN` for V = 3 together with the value pair into one
  base-18 digit (6 × 3 = 18 states, 2 boards per byte = 629 GB, no better) — so the
  full-sweep fallback is the real one.
* **Streaming pass slower than estimated** (many tiny runs for large k'): batch R' by k'
  and read whole k' bands (`cntZ[k'] × cntA[n'-k']` entries) when runs are small.
* **oomd**: run in `system.slice` (`sudo systemd-run --scope --slice=system.slice --uid=ei
  --gid=ei numactl --interleave=all build/solve7 <nvme dir> 42`), since
  `systemd-oomd` kills the whole user slice on memory pressure > 50 % for 20 s (this
  killed the 48-seed run once, at layer 42 of 48, on 2026-09-21).
* Restart safety is as today: finished layers are reloaded from files, and the capture
  pass is idempotent (max/min), so a killed run loses only the current layer.

## 4. Prerequisites checklist

- [ ] ≥ 2.4 TB free on an NVMe file system (`/data1`).
- [ ] No other memory-heavy job during layers 40 and 42 (Breakthrough solver stopped).
- [ ] Implement 3.5, validate per 3.6 (36-seed and 7×2/28 tables byte-identical).
- [ ] Start under `system.slice`, detached; log to `<dir>/solve.out`.
