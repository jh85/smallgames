# Strong-solving larger Amazons boards: findings and plan

Written 2026-11-02, updated 2026-11-04 after the 5x6 run. Measured on 2 x EPYC 9115 (32 cores, 64 threads), 723 GiB RAM, an
8 TB NVMe RAID 0 (2.0 TB free for the 4x7 run, 5.7 TB after 3 TB was cleared), and one external
8 TB USB drive. *Measured* means
reproducible with the tools in this directory, with logs in [results/](results/).
*Estimated* means extrapolated from those measurements.

## Summary

1. **A bit-parallel kernel, not DFAs, is what brought larger boards into reach.**
   `slabsolve` decides all arrow subsets of a queen placement at once. It solves 5x5 in
   5 minutes instead of 55, and 4x7 in 2.9 hours. At the central layer of 4x7 it ran 23
   times faster per position than `dense` in the real run, and 54 times in its benchmark.
   It reproduces every table of `dense` bit for bit.
2. **4x7/7x4 is solved: a first-player win on 4x7, as published, and a second-player win
   on 7x4, which the survey does not list.** The kept half of the layers takes 875 GiB on
   the external drive. **5x6/6x5 is solved too: a first-player win on both.** 5x6 matches Song and
   Mueller's weak solution; 6x5 was open. It took 35.4 hours of computing and 39.3 in all,
   and the kept half takes 6.5 TiB, 6.1 TiB on the external drive and 0.4 TiB left on NVMe
   when the drive reached its reserve.
3. **DFAs compress endgames very well and the middle game hardly at all**, so they cannot
   speed up these solves. The central layer has about 300 positions per DFA node on every
   board measured. The layers DFAs do compress hold under 6% of the positions.
4. **A strong solve of 6x6 stays out of reach, because of disk, RAM and time.** Its table
   is about 8 PiB, with about 780 TiB working. At the speed of the 4x7 run it would also
   take about 7 months, *estimated*.

## Solved so far

All placements of 4+4 queens with any number of arrows, so every starting setup, and both
orientations from one table.

| Board | Root (standard setup) | Positions | Wins | Losses | `slabsolve` | `dense` | Stored, zstd |
|---|---|--:|--:|--:|--:|--:|--:|
| 4x4 | second player | 230,630,400 | 154,827,660 | 75,802,740 | under 1 s | under 2 s | 0.001 GiB |
| 4x5 | second player | 36,118,118,400 | 25,626,518,984 | 10,491,599,416 | 14 s | 23 s | 0.33 GiB |
| 5x4 | first player | same table | | | | | |
| 4x6 | first player | 3,373,987,921,920 | 2,417,950,646,547 | 956,037,275,373 | 3.7 min | 35 min | 32.9 GiB |
| 6x4 | second player | same table | | | | | |
| 5x5 | first player | 9,923,493,888,000 | 7,169,262,583,367 | 2,754,231,304,633 | 5.0 min | 55 min | 55.5 GiB |
| 4x7 | first player | 228,135,901,593,600 | 162,376,663,387,286 | 65,759,238,206,314 | 2.9 h | not run | 1,879 GiB, 875 kept |
| 7x4 | second player | same table | | | | | |

The 4x7 time covers computing and checks. The last kept layers reached the external drive
an hour later. 4x7 keeps every second layer (`--keep parity`), which is still an exact
oracle; the other boards keep all of them. From the 4x7 start, 22 of the 350 first moves
win. From the 7x4 start, all 350 lose.

4x4, 5x4, 4x6, 6x4, 5x5 and 4x7 agree with the literature survey
(`/data1/amazons/amazons.md`, outside this repository), which lists root results only.
4x5 does not: see below. The survey has no entry for 7x4.

## What exists now

Everything is our own code, independent of the older search solver and of Considine's
`dfa-games`. See [README.md](README.md).

* `slabsolve`: the bit-parallel, out-of-core solver, for real runs. It also answers
  queries (`probe`) and plays checked games (`playout`).
* `dense`: one bit per position, one position at a time. It also derives exact minimal
  DFAs from solved tables. Now mainly an oracle for checking `slabsolve`.
* `amzdfa`: symbolic solver over minimal layered DFAs. Each layer needs one preimage:
  `W_k = pre(L_{k+1})`, `L_k = P_k \ W_k`. The preimage is a single level-by-level subset
  construction over an automaton of all moves. Now mainly a second check.
* `crosscheck.py` (in `ctest`): all three engines on 10 board and queen configurations.

## The kernel

`dense` walks every move of every position until it finds a losing child, 13 to 14 probes
per position. With the queens fixed, though, a move is legal exactly when no arrow lies on
its path, so "this move wins" is a mask and a shift over a bit vector indexed by the arrow
subset. `slabsolve` keeps those vectors as 512-bit words: the low 9 bits of the arrow index
pick a bit, and the rest pick a word. A layer has a fixed arrow count, so most bits are dead
and stay zero.

All children of the parents with opponent set `Y` lie in the row with mover set `Y`, so one
row is processed at a time, in two stages:

```
Stage 1, per child slab (Y, X') and queen t of X':
    S_t(A') = exists s on a ray from t, path clear of A', with L(Y, X', A'+s)
Stage 2, per parent slab (X, Y):
    W(A)   = exists f in X and t on a ray from f, path clear of A, with S_t(A')
```

`S_t` does not depend on which queen moved to `t`, so parents share it. `A'` is `A` with
the bit of `t` removed and a zero inserted for `f`, which only shifts the index bits between
those two ranks. Across words that is a renumbering; inside a word it is a byte permute and
a GF2 affine bit permute (`LowPerm`). This is the same idea as the automaton of per-cell
requirements in `amzdfa`, applied to bit vectors rather than DFAs.

### Throughput

| | `dense` | `slabsolve` |
|---|--:|--:|
| 5x5, whole solve | 55 min | 5.0 min, including files and checks |
| 4x7, whole solve | not run | 2.9 h, including files and checks |
| 4x7, central layer, real run | 3.6e8 positions/s (from 5x5 and 4x6) | 8.4e9 positions/s (23x) |
| 4x7, central layer, `bench` | 3.6e8 positions/s (from 5x5 and 4x6) | 1.95e10 positions/s (54x) |
| 5x6, central layer, `bench` | 3.6e8 positions/s (from 5x5 and 4x6) | 1.42e10 positions/s (39x) |

Positions are symmetry classes here. The real run reaches under half the `bench` rate. On the
central layer of 4x7 its two kernel stages took 1,001 s, against 523 s from `bench`, and
gathering rows took another 137 s. While the kernel runs, compression and decompression use
the other hardware thread of each core, which `bench` does not do; the cause has not been
measured separately. Estimates in this plan scale the real run, not `bench`.

**Memory bandwidth is the limit, not cores.** On 5x6, 16 threads on one socket reached 7.4e9
positions/s with local memory, against 8.5e9 for all 64 threads on both sockets with memory
wherever it fell. So there is one worker per NUMA node with memory on its own node. One
thread per physical core does as well as two (1.42e10 either way), which leaves the other
hardware thread of each core free for compression and I/O. Stage 1 builds its tables in a
scratch buffer and streams them out with non-temporal stores, which removes the
read-for-ownership traffic. Symmetry is applied to whole words by swapping index bits rather
than by walking set bits.

Boards smaller than 4x6 gain less, since their slabs are only a few words.

## Verification

| Check | Result |
|---|---|
| `slabsolve` against `dense`, every position of every layer of 4x4, 4x5, 4x6, 5x5 | identical, through the files on disk, with 1 to 8 classes |
| `crosscheck.py`: 3x3 to 4x4, q = 1 to 4, all three engines | identical |
| `amzdfa` against `dense`, 4x5 all layers, 5x5 layers 13 to 17 | identical |
| DFA built from the dense table against `amzdfa` | identical node counts, including 17,423,225 on 5x5 layer 13 |
| `dense --reach` against a rerun of the old `layercount`, 5x5 layers 1 to 5 | identical |
| Black's replies after the 5x6 opening B1-B4xD4 | 157, the number printed in Song & Mueller |
| `slabsolve` self-checks on 4x7, including 200 checked games | no mismatch |
| Roots against the literature | agree, except 4x5; 7x4 is not listed |

The comparison with `dense` closes the exhaustive-check item for 5x5 and 4x6: two kernels
that share no code agree on all 1.2e12 canonical positions of 5x5.

On boards too large for `dense`, a run checks itself in four ways:

* Every 64th parent slab, 4 positions are relabelled from the rules by walking rays cell by
  cell, reading the child slabs directly. On 4x7 that is 3.4 million positions per layer.
* After each layer, 1000 random raw positions are relabelled through the files on disk. This
  covers the file format, the tile addressing and the symmetry mapping.
* Wins and losses must sum to `|P_k|`, and zstd frame checksums are verified on every read,
  so each kept layer is fully re-read when its parent is computed.
* `slabsolve playout` plays whole games and checks the table against the rules at each ply.

On 4x7 all four passed. 3.45 million positions per layer were relabelled in memory and 1000 per
layer through the files, with no mismatch, and every layer summed to `|P_k|`. 200 games, 100 from
each orientation, checked 130,984 child values over 3,876 plies with no contradiction. The first
player won every 4x7 game and lost every 7x4 game. Every first move from both starts was probed
([results/4x7_root_probes.log](results/4x7_root_probes.log)).

4x5 stays a second-player win, contradicting Table I of Song & Mueller. That is now four
independent implementations (`wdlretro`, `amzdfa`, `dense`, `slabsolve`). Conventions are not
the explanation: 4x6 and 6x4 have different winners and both match the paper, and the
157-reply check matches too.

## Storage

Only two adjacent layers are ever hot. Everything older is either dropped or cold. This is
the same shape as the earlier Breakthrough work, where cold files went to external drives.

**Tiles remove random I/O.** Output comes out as columns (all movers `X` for a canonical
opponent set `Y`) and is needed as rows (all opponents for a canonical mover set). Canonical
sets are split into `D` classes. The output of class `c` is one file with `D` tiles, and tile
`d` holds the slabs whose mover set is canonically in class `d`. Assembling the rows of class
`d` reads tile `d` of all `D` files: a forward sweep with `D` short seeks and no sort. The
design in the first version of this plan, which re-read about 60 groups per parent group, was
fine on NVMe and would have taken about 10 days per layer on a hard disk.

**Placement.** `--root` may be given several times, and each class goes to the first root with
room, leaving `--reserve` free. So NVMe takes what fits, and the rest spills to disk. A
finished layer is deleted or, if kept, moved to the last root. A kept class that does not fit
on the first root goes straight to the last, and if the last is full the class stays where it
is. Each copy is written and then read back, so the drive moves about 100 MB/s of layer. On
4x7 the copies queued up behind the solver from layer 11 on, and the run ended 56 minutes after
computing did.

**Lessons from earlier runs on this machine**, all in the code now:

| What happened | What the engine does |
|---|---|
| A power outage killed a long run mid-ply | Each class is fsynced and renamed into place, and a rerun continues from the first missing class |
| A cold copy error would not have been noticed on load | A copy is read back and compared before the original goes, and frame checksums are verified on every read |
| The SATA SSD at `/data` (BX500) went emergency read-only in a write-timeout storm under sustained writes | `/data` is not used |
| A disk can fill up or fail mid-run | The class goes to the next root, or the run stops cleanly and resumes |

Tested on 4x5 with a real drive: moving to cold, spilling past a full root, a root turning
unwritable mid-run, `kill -9` four times mid-layer, and a flipped bit in a frame. Totals stayed
exact in each case and the damaged frame was detected.

### The external drive

WD80EDAZ (CMR, helium) in a WD Elements enclosure, USB 3 at 5 Gbit/s, in `usb-storage` mode,
with ext4. Measured on its outer tracks; inner tracks of a drive like this are typically about
half as fast.

| Access | Speed |
|---|--:|
| Sequential write, 1 MiB direct | 222 MB/s |
| Sequential read, 1 MiB direct | 224 MB/s |
| Random 64 MiB reads, like tiles | 216 MB/s |
| Random 4 KiB reads | 130 IOPS |
| Reading and writing together | 91 + 91 MB/s |

So tiles lose almost nothing to seeks, and the thing to avoid is reading and writing the same
drive at once.

### Compression

zstd in 16 MiB frames, on the bitmaps. The five central layers compress 2.2 to 3.2 times at level
3 on 5x5, 2.3 to 3.6 times on 4x6 and 3.0 to 4.3 times on 4x7. Late layers compress much more.
Layers with an odd number of empty cells are the better half: on 4x7 they compress 3.9 times in
all, against 3.4 for the others.

* Keeping every second layer is an exact oracle, because a dropped layer is recovered by a
  one-ply search of the next one. `--keep parity` does this and also keeps layer 0.
* Level 9 is 12 to 15% smaller than level 3 and level 12 adds almost nothing. Level 9 runs at
  about 1 GB/s on 32 hardware threads, slower than the kernel's output on NVMe (2.4 GB/s on
  4x7), but not when a disk is the bottleneck. So 5x6 should use `--level-keep 9`.
* Row width should be the short side: 4x5 is 11% smaller than 5x4. The canonical queen set
  should be the smallest, not the largest: 22% smaller on 4x5.
* The number of classes changes the total by only 2 to 5%.

## Why DFAs do not help these solves

`k` is the number of arrows, so the opening is `k = 0`. The DFA uses 16 bytes per node with no
symmetry reduction; a sparse encoding might halve that, which does not change the conclusions.
The bitmap is symmetry-reduced.

5x5 (25 cells), zstd level 3:

| k | empties | positions | DFA nodes | positions/node | DFA | bitmap | zstd |
|--:|--:|--:|--:|--:|--:|--:|--:|
| 16 | 1 | 1.3e9 | 5,171 | 248,902 | 0.08 MB | 20 MB | 1.5 MB |
| 15 | 2 | 1.0e10 | 145,874 | 70,586 | 2.3 MB | 161 MB | 26 MB |
| 14 | 3 | 5.1e10 | 917,105 | 56,136 | 15 MB | 805 MB | 120 MB |
| 13 | 4 | 1.8e11 | 17,423,225 | 10,342 | 279 MB | 2.8 GB | 649 MB |
| 12 | 5 | 4.7e11 | 119,933,217 | 3,906 | 1.9 GB | 7.3 GB | 1.7 GB |
| 10 | 7 | 1.5e12 | 2,102,240,062 | 700 | 34 GB | 23 GB | 7.2 GB |
| 8 | 9 | 1.8e12 | 6,495,599,809 | 283 | 104 GB | 29 GB | 9.8 GB |
| 6 | 11 | 9.4e11 | 4,949,966,299 | 189 | 79 GB | 15 GB | 5.0 GB |

4x5 (20 cells):

| k | empties | positions | DFA nodes | positions/node | DFA | bitmap | zstd |
|--:|--:|--:|--:|--:|--:|--:|--:|
| 10 | 2 | 5.8e8 | 36,253 | 16,053 | 0.6 MB | 18 MB | 3.4 MB |
| 8 | 4 | 4.4e9 | 1,605,733 | 2,718 | 26 MB | 137 MB | 32 MB |
| 6 | 6 | 8.1e9 | 20,885,972 | 390 | 334 MB | 255 MB | 90 MB |
| 4 | 8 | 4.4e9 | 32,697,739 | 133 | 523 MB | 137 MB | 61 MB |
| all | | 3.6e10 | 102,430,717 | | 1.64 GB | 1.13 GB | 324 MB |

6x6 (36 cells), from the endgame backwards:

| k | empties | positions | DFA nodes | positions/node | DFA | bitmap |
|--:|--:|--:|--:|--:|--:|--:|
| 27 | 1 | 5.9e10 | 12,121 | 4.9e6 | 0.2 MB | 884 MiB |
| 26 | 2 | 8.0e11 | 636,976 | 1.3e6 | 10 MB | 11.7 GiB |
| 25 | 3 | 6.9e12 | 5,409,467 | 1.3e6 | 87 MB | 101 GiB |
| 24 | 4 | 4.3e13 | 155,517,151 | 2.8e5 | 2.5 GB | 631 GiB |

Positions per node at the central layer are 363 on 4x4, 390 on 4x5 and 283 on 5x5, while the
layer grows from 6e7 to 2e12 positions. So DFA size grows about linearly with positions there,
not as the n^0.45 to n^0.7 seen in Breakthrough. The paper warned of "more modest leverage" for
Amazons; this puts a number on it.

Why it does not compress: two prefixes merge in a DFA only if they agree on *every* suffix, and
one witness separates them. In the 4x5 middle game (k = 4, 6), 94% of all possible 8-cell
prefixes and about 80% of 10-cell prefixes have a distinct residual function, so the row cuts up
to mid-board are close to saturated. Oware behaves the same way
(`../../oware/dfa/README.md`).

Time goes the same way. Solving 4x5 on 48 threads took `amzdfa` 13 minutes and 108 GiB, against
14 seconds for `slabsolve`. The subset construction visits 20 to 100 times as many states as the
result has nodes in the middle and opening layers, and splitting the preimage by moving-queen
cell did not help.

**So DFAs cannot speed up 4x7 or 5x6.** Time goes to the central layers. Layers with at most 6
empty cells, roughly where DFAs start to beat zstd, hold 5.8% of the positions of 4x7 and 2.6% of
5x6. Making them free would save very little, and symbolic solving costs far more per position
everywhere else. The DFA engine stays as a check and for a possible 6x6 endgame database.

## Other measurements

### Reachability

The share of `P_k` reachable from the standard start is 0.7% at k = 4, 20% at k = 6, 52% at k = 7,
95% at k = 8 and 100% from k = 10 on 4x5, and 2.4% at k = 5, 14.6% at k = 6, 40% at k = 7 and 81%
at k = 8 on 5x5. So restricting to reachable positions cannot help the central layers.

For storage, values of unreachable positions are don't-cares. Choosing per layer between the full
table and reachable-only gives 140 MB for 4x5, against 324 MB. Combined with keeping every second
layer it gives about 62 MB, 1/18 of the raw table. Reachable sets do not compress as DFAs either: 36
strings per node for 5x5 `R_4`, and on 4x5 layer 7 restricting a DFA costs 64.9M nodes against 5.4M
for the whole envelope.

### Ideas measured and rejected

| Idea | Result | Verdict |
|---|---|---|
| Other cell orders for the DFA | Row-major on the short side beat 5 alternatives, which were 1.5 to 10 times worse | Keep row-major |
| Treating dead empty cells as arrows | 3% of positions on 4x5; compression and DFA size barely move | Rejected |
| Hash-consed forest of slab bit vectors | About the size of the raw bitmap | Rejected |
| Sound static territory bound in proofs | Under 7% smaller proofs on 4x4 to 5x5 | Not a lever on these boards |
| Both hardware threads of each core in the kernel | 1.41e10 positions/s, against 1.42e10 with one | One thread per core |

### Weak-solution proofs

With the tables as a perfect oracle, a proof from the start includes every move at the loser's turns
and one winning move at the winner's. These are upper bounds on the smallest proof, in position
classes:

| Board | Cells | Winner | Proof size | Share of all placements |
|---|--:|---|--:|--:|
| 4x4 | 16 | second | 7,100 | 2.5e-4 |
| 5x4 | 20 | first | 45,000 | 5.0e-6 |
| 4x5 | 20 | second | 1.2M | 1.3e-4 |
| 4x6 | 24 | first | 4.3M | 5.1e-6 |
| 6x4 | 24 | second | 345M | 4.1e-4 |
| 5x5 | 25 | first | 47M | 3.8e-5 |

Growth is about 3 to 4 times per cell, and a second-player win costs 26 to 80 times as much as a
first-player win on the same cell count. *Estimated* for 6x6: 10^13 to 10^16 classes, with perfect
move ordering. Real search is far from perfect: the old df-pn solver did not prove 5x4 within 2 x 10^9
nodes, against a 45,000-class oracle proof. So a weak solution of 6x6 is unlikely without a much better
move-ordering idea, though it is not as far out of reach as a strong one.

## Feasibility by board

Positions are symmetry classes. 5x5, 4x7 and 5x6 are measured; 6x6 is *estimated*.

| Board | Positions | Bitmap | zstd, all layers | Kept, `--keep parity` | Peak on disk | Time |
|---|--:|--:|--:|--:|--:|--:|
| 5x5 | 1.3e12 | 153 GiB | 56 GiB | | | 5.0 min |
| 4x7 | 5.8e13 | 6.6 TiB | 1.8 TiB | 0.9 TB | 1.2 TB | 2.9 h |
| 5x6 | 4.4e14 | 50 TiB | 14.4 TiB | 6.5 TiB (7.1 TB) | | 35.4 h |
| 6x6 | 7.2e16 | 8 PiB | | | about 780 TiB working | about 7 months |

Before the run, the *estimates* for 5x6 were 13 to 17 TB for all layers, 5.8 to 7.2 TB kept,
7.8 to 9.8 TB at the peak and 2 to 3 days. Those sizes took the per-layer ratios of 4x7, and the upper end of each range assumes 25%
worse compression, closer to 5x5. Kept layers use level 9. Peak on disk is the kept layers so far
plus the two active ones, which on 4x7 came at layer 7. The time scales the 4x7 run by positions
(7.5 times) and by the `bench` rate of 5x6 against 4x7 (1.4 times slower). Scaling each layer
separately gives 36 hours of computing.

What 5x6 needs, against what there is:

| Resource | Needed | Available |
|---|--:|--:|
| RAM | 73 GiB of row buffers, plus classes of 33 GiB (256 classes) | 723 GiB |
| Disk at the peak | 7.8 to 9.8 TB | 5.7 TB NVMe + 7.0 TB on `/mnt` = 12.7 TB |
| Kept layers | 5.8 to 7.2 TB | 7.0 TB on `/mnt`, and NVMe for any rest |

Before 3 TB was cleared on `/data1` there were 9.0 TB, short of the upper end. A class-by-class
simulation of the run on the two roots, with the copies to the drive running behind the solver and the drive at half speed on its inner tracks, never runs
out of space with a 300 GiB reserve. The root value comes after 39 to 45 hours. Copying the kept
layers that waited on NVMe takes 15 to 16 hours more. In the worse case 0.5 TB of kept layers stay
on NVMe because the drive is full. A second drive is not needed.

### Why 6x6 is still out of reach

1. **Storage.** Two adjacent central layers need about 780 TiB even when compressed 3 times.
2. **Memory.** A row needs 1.2 TB of child slabs and 4.8 TB of shot tables, against 723 GiB of RAM. The
   kernel could be reorganized around that, at a real cost in time.
3. **DFAs do not rescue it.** At the measured 200 to 400 positions per node, one central layer is about 3
   x 10^14 nodes, roughly 5 PB. On real 6x6 the symbolic engine reaches 4 of 28 layers before its next
   layer needs an *estimated* 1 TB of RAM.
4. **Time.** 7.2 x 10^16 positions at the rate of the 4x7 run, slowed by the `bench` ratio of 5x6
   to 4x7, take about 7 months. The per-position rate fell from 4x7 to 5x6, so 6x6 would
   probably be slower still.

Fewer queens would bring it closer (1+1 on 6x6 is about 310 GiB, and 2+2 about 22 TiB raw), but that is a
different game.

## Milestones

1. Done: `slabsolve`, identical to `dense` on every position of 4x4, 4x5, 4x6 and 5x5.
2. Done, 2026-11-03: 4x7 and 7x4, in `/mnt/amazons/slab_4x7` with its config and progress files in
   `/data1/amazons/work/slab_4x7`. The run log is [results/4x7_slabsolve.log](results/4x7_slabsolve.log).
3. Done, 2026-11-04: 5x6 and 6x5, with `/data1/amazons/work/run_5x6.sh`, which runs

   ```sh
   slabsolve solve 5x6 --root /data1/amazons/work/slab_5x6 --root /mnt/amazons/slab_5x6 \
     --keep parity --level-keep 9 --reserve 300 --verify-samples 1000
   ```

   Both roots are first-player wins. 10 of 410 first moves win on 5x6 and 8 of 410 on 6x5.
   As in Song and Mueller, B1-B4xD4 (cell 1 to 16, arrow on 18) leaves Black 157 replies, and
   all 157 lose ([results/5x6_root_probes.log](results/5x6_root_probes.log)). Every layer
   passed its in-run check, and `playout` played 200 games from both setups (4,150 plies,
   35,613 child values) with no contradiction ([results/5x6_playout.log](results/5x6_playout.log)). Layers 0, 1, 3, 5 and 7 stayed partly or wholly on NVMe because the
   drive reached its 300 GiB reserve, so queries need both roots. The run log is
   [results/5x6_slabsolve.log](results/5x6_slabsolve.log).
4. Optional: a second 4x7 run with other `--classes` and `--canon`, compared position by
   position with the first. That needs a compare mode between two stores, which does not exist
   yet; the run's own checks sample every layer but do not cover all of it.
