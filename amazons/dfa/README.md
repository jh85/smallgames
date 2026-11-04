# Amazons: strong-solving tools

Three independent ways of solving every placement of a small Amazons board, plus
tools that measure how compressible the solved sets are. See [PLAN.md](PLAN.md)
for what the measurements mean and what to do next.

None of them uses any other code in this project, or Considine's `dfa-games`, so
each can check the others and the older `wdlretro` tables. `slabsolve` is the one to
use for real solves; the other two exist mainly to check it.

## Model

A position is one symbol per cell, written from the point of view of the side to
move: empty, mover's queen, opponent's queen, arrow. Every move burns exactly one
square, so a position with `k` arrows only moves to one with `k + 1`. Writing
`P_k` for all placements with `k` arrows:

    W_k = positions with a move into L_{k+1}
    L_k = P_k \ W_k

That is a single pass over the layers with no fixpoint. Solving all of `P_k`,
rather than only what is reachable, requires no forward enumeration and gives the
value of every placement. The `WxH` table therefore also answers the rotated
`HxW` board and any other starting setup.

## Tools

| Tool | What it does |
|---|---|
| `slabsolve solve WxH --root DIR` | Bit-parallel, out-of-core solver. It decides all arrow subsets of a queen placement at once, as masks and shifts on 512-bit words, and streams layers through files. Needs AVX-512 with VBMI and GFNI, and libzstd. |
| `slabsolve probe WxH --root DIR` | Value of one position from a finished run, and optionally of every move. |
| `slabsolve playout WxH --root DIR` | Plays whole games from both orientations with the table and checks it against the rules at every ply. |
| `slabsolve bench WxH` | Kernel timing on real board sizes, without solving anything first. |
| `amzdfa solve WxH` | Symbolic solver. Sets are minimal layered DFAs, and each preimage is one level-by-level subset construction over an automaton of all moves. |
| `dense solve WxH DIR` | One bit per position, reduced by board symmetry. Faster and more predictable than the DFA in the middle game. |
| `dense dfa` | Exact minimal DFA of a solved layer, by full enumeration, for any cell order. |
| `dense stats` | Entropy bounds on how far the tables could be compressed. |
| `dense proof` | Size of a weak-solution proof from the start, using the tables as an oracle. |
| `dense forest`, `dense dead`, `dense reorder` | Measurements of ideas that were tried and rejected. See PLAN.md. |
| `blockzstd` | Compressed size of a file when each block is a separate zstd frame. |
| `crosscheck.py` | Runs all three engines on small boards: every layer's counts, and `slabsolve` bit for bit against `dense`. Part of `ctest`. |

## Build and test

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
ctest --test-dir build -R dfa_crosscheck
```

`blockzstd` and `slabsolve` are built only if libzstd is installed, and `slabsolve` only with AVX-512 VBMI and GFNI.

## Usage

```sh
# Everything on one disk, keeping all layers
./build/slabsolve solve 5x5 --root DIR

# Active layers on a fast disk, finished layers moved to a slow one, keeping the half
# that compresses better.  A dropped layer costs a query one ply of search.
./build/slabsolve solve 4x7 --root /fast/amz --root /slow/amz --keep parity

# Three roots: what does not fit on the fast disk spills to the middle one, and kept
# layers go to the last.  Kept layers at zstd level 9 are 12 to 15% smaller.
./build/slabsolve solve 5x6 --root /fast/amz --root /slow1/amz --root /slow2/amz \
  --keep parity --level-keep 9

# Games played with the table, checked against the rules at every ply
./build/slabsolve playout 4x7 --root /fast/amz --root /slow/amz --games 200

# Check every position of every layer against tables from the dense solver
./build/slabsolve solve 4x5 --root DIR --compare DENSE_DIR

# Value of a position.  Cells count row by row from 0; rerun `solve` to resume a run.
./build/slabsolve probe 4x5 --root DIR --m 1,2,4,7 --o 12,15,17,18 --moves

# Symbolic solve, relabelling every position of every layer with explicit rules
./build/amzdfa solve 4x4 --exhaustive

# Endgame layers only, down to 24 arrows
./build/amzdfa solve 6x6 --kmin 24 --sample 5000 --widths

# Dense solve with sampled relabelling; tables go to DIR, outside the repository
./build/dense solve 5x5 DIR --verify-samples 200000

# Minimal DFA of one solved layer
./build/dense dfa 5x5 DIR --k 12 --set W --order row
```

`slabsolve` picks one worker per NUMA node with one thread per physical core, and runs
compression and I/O on the other hardware thread of each core. It needs about 200 GiB of
RAM, set by `--classes`. Each class of each layer is fsynced and renamed into place, so a
rerun with the same arguments continues after a crash. If a root is full or fails, the
class goes to the next root. Frame checksums are verified on every read.

Generated tables stay out of git. The logs behind the numbers in PLAN.md are in
[results/](results/).
