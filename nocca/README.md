# NOCCA × NOCCA strong solver (ZDD + retrograde analysis)

C++ implementation of *Yamamoto & Hoki, "Strongly Solving NOCCA × NOCCA", Game Programming
Workshop 2022*: the 147,969,899,280 pseudo-reachable positions of NOCCA × NOCCA are indexed
by a ZDD-based minimal perfect hash, retrograde analysis computes the game value of every
position, and the result is a packed 2-bit WDL table that answers win/draw/loss for any
position. Section and table references below (§6, Table A-1, …) point into that paper, which
is not redistributed here.

## Build

```
make            # produces ./nocca (g++ -O3 -march=native, needs C++20 + pthreads)
```

## Commands

| command | what it does |
|---|---|
| `./nocca selftest` | State tables, ZDD vs. independent DP count (must be 147,969,899,280 for 5×6), rank/unrank roundtrip, iterator, cached child-rank checks |
| `./nocca solve3` | Fully solves the 3×3 variant in <1 s (writes `wdl_3x3.bin`) |
| `./nocca bf3check` | Cross-checks the 3×3 table against an independent depth-bounded minimax (no ZDD / no symmetry / no retrograde) |
| `./nocca solve [--threads N] [--dir D] [--ckpt K]` | The full 5×6 solve. Resumable: re-running continues from the last checkpointed pass |
| `./nocca analyze [--dir D]` | Initial-position evaluation + per-move values |
| `./nocca probe <b\|w> <cell0> … <cell29> [--dir D]` | WDL of an arbitrary position |

## Generating the WDL files

The tables are **not** stored in this repository — the 5×6 table is 34.5 GiB. Everything
needed to regenerate them byte-for-byte is here; both solves are deterministic, so a
successful run reproduces the digests below exactly.

### 3×3 (seconds, negligible resources)

```
make
./nocca solve3        # writes wdl_3x3.bin (20,816 bytes)
./nocca bf3check      # optional: cross-check against an independent minimax
```

### 5×6 (the full solve)

```
make
./nocca selftest                          # ~seconds; verifies the ZDD count before committing hours
./nocca solve --threads 64 --ckpt 1       # writes wdl_5x6.bin (36,992,483,016 bytes)
./nocca analyze                           # initial position + per-move values
```

Requirements and behaviour:

| | |
|---|---|
| RAM | ~34.5 GiB — the table is a single anonymous `mmap`, held resident for the whole solve |
| Disk | ~69 GiB free in the table directory: checkpoints are written to `wdl_5x6.bin.tmp` and `rename`d over the target, so both copies exist briefly |
| Time | 8.1 h wall clock on 2× EPYC 9115 (64 threads). Roughly linear in thread count; pass 1 alone is ~3.5 min |
| Threads | `--threads N`, default = hardware concurrency |
| Location | `--dir D` places/reads the table in `D` (default: current directory) |
| Checkpoints | `--ckpt K` snapshots every `K` passes (default 1, ~7 s each). Re-running the identical `solve` command resumes from the last checkpointed pass, so the job can be killed at any time |

The run prints one line per pass with the new win/loss counts, each compared against the
paper's Table A-1 (`[paper: OK]`), and finishes with the W/L/D totals and the initial-position
verdict. Convergence is at pass 70.

### Published tables

The generated tables are hosted outside this repository, **zstd-compressed**. A packed 2-bit
WDL table is highly redundant, so `wdl_5x6.bin` shrinks 51.8× — 34.5 GiB down to a 681 MiB
download. Decompress with `zstd -d wdl_5x6.bin.zst`.

| table | download | compressed | uncompressed |
|---|---|---|---|
| `wdl_3x3.bin.zst` | not published yet | 2,903 B | 20,816 B |
| `wdl_5x6.bin.zst` | not published yet | 713,812,191 B | 36,992,483,016 B |

Verify with `sha256sum -c` against these digests — the compressed file as downloaded, or the
decompressed table (which is also what a local solve reproduces):

| file | SHA-256 |
|---|---|
| `wdl_3x3.bin.zst` | `c6b797d771991ceaa7706a959393daafd8652f0f0fef13b5f6661caac7977744` |
| `wdl_5x6.bin.zst` | `5bceb3640d9650303e308c18023fe4ac54defdc750f7a9b17ffa25d511a10979` |
| `wdl_3x3.bin` | `df3e40c96e83164d817621ed951af8000de8c84bdc40d042d286755ae5f21390` |
| `wdl_5x6.bin` | `6d752da11d0c3b339b352dca0179b918a6ee5a28acd5bef49eddd130c87b8715` |

To check without unpacking 34.5 GiB to disk:

```
zstd -dc wdl_5x6.bin.zst | sha256sum
```

The archives were produced with `zstd -12 --long=27 -T0`; the 128 MiB window is zstd's
default decoder limit, so plain `zstd -d` works with no extra flags.

## Position input format (`probe`)

First argument: side to move (`b` or `w`). Then 30 cells, row-major, **row 0 = Black's home
row first**, each cell `-` (empty) or 1–3 letters from `w`/`b` listed **bottom→top**, e.g.
`wb` = white with black on top. Black moves toward row 5 and enters its goal off row 5;
White moves toward row 0. Both players start with 5 pieces filling their home row:

```
./nocca probe b  b b b b b  - - - - -  - - - - -  - - - - -  - - - - -  w w w w w
```

## WDL table format (`wdl_5x6.bin`)

* 8 KiB header (`struct Hdr` in `nocca.cpp`): magic `NOCCAWDL`, board size, position count
  `N`, passes done, `done` flag, per-pass win/loss counts (`histW`/`histL`).
* Then `ceil(N/32)` little-endian `u64` words, 2 bits per position, indexed by the ZDD rank
  of the canonical **black-to-move** encoding (white-to-move positions are looked up after
  flipping rows and swapping colors — the game is symmetric under that involution):
  * `0` = draw (or not-yet-decided while `done=0`)
  * `1` = win for the player to move
  * `2` = loss for the player to move

The full table is ~34.5 GiB. `probe` reads single words with `pread`, so lookups need no RAM.

## How it works

1. **Square states** (paper Table 6): each of the 30 squares holds one of 15 states — empty,
   or a stack of height 1–3 with each level white/black. State 14 (black-black-black) is the
   ZDD's implicit default; states 0–13 are explicit items, giving 30×14 = 420 ZDD variables.
2. **ZDD** (§7): built canonically by memoized recursion on the frontier signature
   (square-assigned flag, #white, #black, white-top-seen, black-top-seen) — equivalent to the
   paper's frontier method (Table 7) + reduction. The pseudo-reachable constraint is: 5 white
   + 5 black pieces, stacks ≤ 3, and ≥1 white and ≥1 black piece visible from above.
   Node count ~15 k; the root path count is exactly 147,969,899,280, which doubles as an
   independent verification against a plain DP count.
3. **Minimal perfect hash** (Tables 2–3): rank = walk the ZDD accumulating 0-child path
   counts; unrank is the inverse; a stack-based iterator enumerates positions in rank order
   for the sequential passes. Child positions reuse the parent's per-square walk prefix.
4. **Retrograde analysis** (Table 4, §8): strict passes over black-to-move positions.
   Pass 1 marks intrinsic values: win-in-1 (a movable black piece on row 5 → goal entry; or a
   move that covers White's last visible piece / leaves White with no movable piece) and
   0-move terminal losses (Black cannot move). Pass k ≥ 2 marks positions whose children (via
   the flip+swap involution) contain a loss (→ win at distance k) or are all wins (→ loss at
   distance k). New values are marked `3` during a pass and converted afterwards, so passes
   are strict — the per-pass counts reproduce the paper's Table A-1 exactly. Unmarked
   positions after convergence are draws (infinite play).
5. **Checkpointing**: after each pass the table+header is snapshotted atomically
   (tmp + rename), so the solve can be killed and resumed at any time.

## Validation

* ZDD cardinality = 147,969,899,280 = paper §6 (and = independent DP count).
* 3×3 variant: full agreement (all sampled positions, both sides to move) with an
  independent minimax search; first player wins in 9 plies under this paper's rules.
  (The related-work table cites 11 moves for Suwa's 3×3 study — different convention.)
* 5×6: every pass count is compared against Table A-1 (values embedded in the binary; the
  entry for pass 48 is unreadable in the PDF and is skipped), the terminal-loss count (30),
  the final W/L/D totals, and the paper's headline result: the initial position is a
  first-player win decided at pass 41.

## Results of the completed 5×6 solve (2026-07-29, 8.1 h on 2× EPYC 9115 / 64 threads)

* All 69 Table A-1 per-pass counts match the paper exactly; the pass-48 entry unreadable in
  the PDF is **879,284**. Convergence at pass 70 (no new values), maximum distance 69 moves
  (30 positions), as in the paper.
* Initial position: **WIN for the first player, decided at pass 41**. Of its 21 legal moves,
  the 13 forward/diagonal moves win and the 8 sideways moves lose — matching §8.2.
* Totals: wins 106,144,078,911 / losses 41,129,930,539 / draws 695,889,830. The paper's
  Table 8 reports losses 41,129,930,509 / draws 695,889,860: it leaves the 30 zero-legal-move
  terminal positions unvalued (implicitly draws), although its own Fig. 4 describes them as
  losses; this solver marks them losses-in-0. All other numbers agree to the unit.
* 3×3 variant: first player wins in 9 plies (confirmed by the independent minimax).

## Files

| file | contents |
|---|---|
| `nocca.cpp`, `Makefile` | complete standalone solver and build recipe (~1,100 lines C++20) |
| `README.md` | results, table format, build instructions, and command examples |

The compiled `nocca` binary, run logs, and the generated `wdl_*.bin` tables (plus their
`.tmp` checkpoints) are excluded by `.gitignore`; see *Generating the WDL files* above to
recreate the tables and check them against the published digests.
