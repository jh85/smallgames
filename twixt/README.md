# Small-board TwixT

This directory contains an exact C++17 solver for square and rectangular
auto-link computer TwixT. It can solve a single position by depth-first search,
measure exact reachable layers, or strongly solve a board by forward enumeration
and layered WDL retrograde analysis. A symbolic solver performs move generation
and retrograde directly on minimized DFA languages, storing a queryable DFA
solution instead of explicit WDL records. Completed solutions can be queried directly,
including the value of every legal move, which is useful for evaluating an
AlphaZero-style agent.

Large generated databases are deliberately **not stored in this repository**.
The `tables/` directory, `*.twixtdb`, and `*.twixtdfa` files are ignored by git. Published
results belong in an external archive such as Zenodo; the result table below is
where their SHA-256 digests and download links should be recorded.

## Rules implemented

- White moves first and connects the top edge to the bottom edge. Black connects
  the left edge to the right edge.
- A move places one peg in an empty legal hole. Corners are absent. White cannot
  play on the left or right border, and Black cannot play on the top or bottom
  border.
- After a placement, every currently legal knight-move link from the new peg to
  a friendly peg is added automatically. A link is not added if it would cross
  an existing link of either colour.
- The first completed connection wins. If neither player has won and there is no
  legal placement, the result is a draw.
- There is no pie rule, link removal, or manual choice of links.

The position representation includes both pegs and links. This is essential:
different move orders can produce the same peg diagram but different link sets
because an earlier link can block a later crossing link.

## Build and test

From this directory:

```sh
make
make test
```

The tests exercise the rules, a known 3x3 result, exact strong retrograde, WDL
write/read/probe round-tripping, and a short rectangular-board census. Symbolic
tests compare every reachable state, both connection predicates, and every WDL
value with explicit retrograde on 3x3, 4x4, 3x5, 5x3, and 4x5. Additional tests
exhaustively check DFA set operations against ordinary sets and reject corrupted
automatic-link transitions and malformed solution files.

## Symbolic strong solving

This follows the compressed-set approach of Jeffrey Considine,
[Compressed Game Solving](https://arxiv.org/abs/2411.07273), with TwixT-specific
conditional link updates and frontier connectivity recognition.

**Completed and exhaustively verified: 5x5 is a draw under the auto-link rules
above.** The DFA solution covers all 60,535,612 reachable positions across 22
layers, including descendants of proven-dead draws. All positions, both players'
connectivity predicates, and every WDL value agree with independent explicit
retrograde analysis. The saved solution has 8,388,377 shared DFA nodes and is
55,171,387 bytes (52.62 MiB). Verification peaked at 3,826,016 KiB RSS, including
the explicit reference. Development used staged runs and restarts, so the raw
logs are not a clean single-version runtime benchmark.

Completed local DFA artifacts (generated files remain ignored by git):

| Board | Root | Reachable positions | Bytes | DFA SHA-256 |
|---:|:---:|---:|---:|:---|
| 4x5 | Black win | 304,210 | 1,112,064 | `5574ea10ee328c26fed988f9925aae8d6657ba746e7e4baa77afe0051a44cc2d` |
| 5x5 | Draw | 60,535,612 | 55,171,387 | `54643f75bb27afce1bbd68bcf85dc83f43f3667dac7b9e4ef6fba2a1262d149e` |

Files are `tables/4x5.twixtdfa` and `tables/5x5.twixtdfa`. No external archive or
DOI has been published. Reload/probe examples:

```sh
./twixt --probe-dfa tables/5x5.twixtdfa --quiet
./twixt --probe-dfa tables/5x5.twixtdfa --moves C3 --quiet
```

Every opening move on 5x5 draws under optimal play. After White's C3, Black's
only drawing replies are B3 and D3; the other replies lose. This illustrates
why the full policy/value solution is more informative than the root result.

The symbolic solver stores sets of fixed-length strings as minimized layered
acyclic DFAs. Cells use `{empty, White, Black}` and links use `{absent, present}`;
one final symbol records the turn. Spatial ordering emits each cell followed by
links whose second endpoint has just been visited. It supports every geometry
accepted by the explicit solver, including strings longer than the offline
pilot's 128-symbol limit.

```sh
./twixt --board 4x5 --symbolic --symbolic-verify --census-seconds 0 \
  --dfa-out tables/4x5.twixtdfa
./twixt --probe-dfa tables/4x5.twixtdfa --moves B1
```

Create `tables/` before using it as an output directory. `--symbolic-verify`
independently enumerates reachable states and solves them explicitly, then
compares every value. It is intended for small boards and obeys
`--census-max-states` for its explicit enumeration. It can also be applied to
`--probe-dfa` to validate a saved solution without recomputing the symbolic solve.

For bounded symbolic generation without solving, use:

```sh
./twixt --board 5x6 --symbolic-ply 7 --symbolic-verify \
  --census-max-states 3000000 --census-seconds 120
```

`--dfa-max-nodes` caps the shared node pool (default 5 million), including
intermediate results. This is distinct from the potentially enormous number of
positions represented. `--census-seconds` caps symbolic wall time too (default
10 seconds; `0` disables it). State counts fail explicitly on 64-bit overflow.
Cap exits return status 3 and do not create a completed solution file.
Node IDs are 32-bit, so the shared pool cannot exceed about 4.29 billion nodes
even with `--dfa-max-nodes 0`. The pool cap does not bound temporary product
memo tables or the extra copying space needed for collection.

The solver generates complete reachable layers using DFA filters, renaming,
union, and difference. Each placement restricts the old hole to empty and adds
each incident link exactly when the other endpoint is friendly and no existing
link crosses it. New incident links cannot cross one another, so these
conditional assignments commute. The preimage reverses the same assignments;
it never treats an automatic link as optional. A frontier component automaton
recognizes completed connections without enumerating positions.

Retrograde uses `W = preimage(next L)` and
`L = terminal losses OR (active positions MINUS preimage(next R MINUS next W))`.
Draws are the remaining reachable positions, including exhausted boards. Roots
are shared and garbage-collected across layers and W/L/D sets. Conditional link
updates split DFA suffixes at the changed variable, avoiding large temporary
whole-position products. The engine retains all layers in RAM; it does not yet
have an external-memory node store.

Once a layer is solved, its W/L/D languages suffice to identify reachable
positions. The separate reachable, active, and terminal roots are released;
retrograde uses `next L OR next D` directly for the nonwinning successors.
Completed archives therefore leave their R roots zero and represent the domain
by W/L/D. The verifier temporarily reconstructs R to check connectivity and
coverage against the explicit reference.

Long computations can checkpoint and restart at layer boundaries:

```sh
./twixt --size 5 --symbolic --census-seconds 600 --dfa-max-nodes 30000000 \
  --dfa-checkpoint tables/5x5.twixtckp --dfa-out tables/5x5.twixtdfa
./twixt --resume-dfa tables/5x5.twixtckp --census-seconds 600 \
  --dfa-max-nodes 30000000 --dfa-checkpoint tables/5x5.twixtckp \
  --dfa-out tables/5x5.twixtdfa
```

The `TWXCKP01` checkpoint includes complete reachable/terminal/active languages,
solved W/L/D roots, and the next retrograde layer. It is replaced atomically
after each completed layer, using a neighboring `.partial` file during writing.
Resume gets a fresh time budget and uses the saved root and geometry. The
preceding checkpoint survives an interrupted layer; that layer is recomputed.
Only `--dfa-out` files are complete queryable solutions. A checkpoint can also
be generated with `--symbolic-ply`, then resumed into a complete solve.

The symbolic mode retains descendants of proven-dead draws, stopping only at
actual wins or an exhausted side to move. This makes reachability independent
of heuristic pruning and keeps every legal query after a dead position valid.
Consequently its state counts differ slightly from the default pruned census.
It uses no explicit symmetry canonicalization. The `--no-draw-pruning` and
`--no-symmetry` switches are therefore unnecessary in this mode.

The portable little-endian `TWXDFA01` format records version, board dimensions,
variable order, string length, shared node count, layer count, root key, four
language roots per layer (R/W/L/D), and topologically ordered DFA nodes. Version
2 stores one nonzero-arc mask byte followed by a 32-bit child ID for each
present arc (5--13 bytes per node). The variable index is inferred from the
children's levels. IDs zero and one are implicit reject and accept terminals.
The loader also supports version 1's fixed 16-byte records. It checks exact
file length, schema, root levels, topological order, transition levels, and
node uniqueness. Queries walk the DAG and report every legal move's value.

Direct symbolic generation has reached these layers on one EPYC 9115 core:

| Board / ply | Reachable positions | DFA states | Generation seconds |
|---:|---:|---:|---:|
| 5x6 / 7 | 2,433,214 | 179,894 | 2.28 |
| 5x6 / 8 | 9,555,392 | 550,106 | 11.60 |
| 5x6 / 9 | 24,833,712 | 878,826 | 40.36 |
| 5x6 / 10 | 75,211,848 | 2,205,672 | 141.89 |
| 6x6 / 7 | 15,887,060 | 642,738 | 20.57 |

Times are cumulative through the indicated layer. The 5x6 ply-10 run peaked
at 1,082,720 KiB RSS; with checkpoints, generation took 145.17 seconds. Every
reachable state and both connectivity tests were independently checked through
5x6 ply 7 (3,287,469 positions across layers) and 6x6 ply 7 (19,649,105 positions
across layers). These are reachability milestones, not complete WDL solutions
of 5x6 or 6x6. Raw run logs/checkpoints are under the ignored `tables/` directory.
Resuming the 5x6 checkpoint reached ply 11 with 149,777,156 positions in
2,874,329 DFA states (52.1 positions per node); that additional layer took about
167 seconds, including checkpoint overhead.

The first capped 5x6 run completed reachable layers 0--11 (262,665,577 positions
across layers), then stopped during layer 12 without a WDL solution. The working
checkpoint has since advanced. Checkpoints can continue with a larger pool and
time allowance, without regenerating their completed layers:

```sh
./twixt --resume-dfa tables/5x6-working.twixtckp \
  --dfa-max-nodes 100000000 --census-seconds 3600 \
  --dfa-checkpoint tables/5x6-working.twixtckp \
  --dfa-out tables/5x6.twixtdfa
```

## Parallel symbolic solving

`--workers N` enables up to 64 symbolic worker threads (default: 1). Existing
solutions and checkpoints are compatible with every worker count, so a serial
run can resume in parallel without repeating its completed layers:

```sh
./twixt --resume-dfa tables/5x6-working.twixtckp --workers 64 \
  --dfa-max-nodes 1000000000 --census-seconds 86400 \
  --parallel-memory-gb 512 \
  --dfa-checkpoint tables/5x6-working.twixtckp \
  --dfa-out tables/5x6.twixtdfa
```

The parallel implementation covers forward placement images, retrograde
preimages, union suffixes, and DFA compaction/registry cleanup. The node array
uses stable blocks, interning uses sharded locks, and operation caches are
private to each worker with a bounded shared product cache. Small reservations
of node IDs reduce allocator contention; unused reservations disappear during
compaction. Every published child still precedes its parent, preserving the
portable file format and exact minimization invariants.

Disjoint peg-prefix shards create enough placement jobs when there are fewer
legal holes than workers. Large unions are split into independent suffix
languages and their prefix automaton is reconstructed exactly. No peg/link
histories are approximated or dropped. Connection recognition, input loading,
parts of collection, and checkpoint I/O retain serial work, so speedup need not
scale linearly with the worker count. On this machine 64 logical CPUs share
32 physical cores; compare 8, 32, and 64 workers for the board/layer of interest.

`--parallel-min-nodes N` avoids worker overhead below N active DFA nodes
(default: 20,000; use 0 to exercise parallelism on tiny tests).
`--parallel-memory-gb N` sets the Linux PSS monitor's process/worker memory
budget (default: 512 GiB; 0 disables). It is sampled during worker computation,
not a kernel-enforced bound; collection/checkpoint peaks can require additional
space. The local production wrapper also imposes a 512 GiB virtual-memory
limit. Worker errors and resource caps cancel/join the remaining workers and
leave the last completed checkpoint intact.

`--parallel-backend processes` selects the alternative POSIX fork backend.
It shares immutable snapshots by copy-on-write and returns validated node
deltas for import at wave barriers. `--parallel-temp-dir PATH` selects its
scratch directory (default: `tables`). Temporary deltas are removed on normal
completion and handled failures; Linux workers receive a parent-death signal
if the coordinator is killed. The thread backend avoids delta files/imports
and is the default.

`--compare-dfa PATH` checks exact DFA-language equivalence, rather than sampling
positions. For completed solutions it compares every W/L/D set; for bounded
forward runs it compares reachable, terminal, and active sets to a serial
forward checkpoint. For example:

```sh
./twixt --size 6 --symbolic-ply 8 --workers 1 --census-seconds 0 \
  --dfa-max-nodes 200000000 \
  --dfa-checkpoint tables/6x6-serial.twixtckp
./twixt --size 6 --symbolic-ply 8 --workers 64 --census-seconds 0 \
  --dfa-max-nodes 200000000 \
  --compare-dfa tables/6x6-serial.twixtckp
```

The 64-worker complete 5x5 solve reproduced all W/L/D languages in the
independently verified serial solution: 60,535,612 covered positions,
8,388,377 shared nodes, and a 55,171,387-byte artifact. It finished in 324.40
seconds including comparison and checkpoints, peaking at 18,562,096 KiB RSS.
This is a new clean parallel run, not a comparison against the earlier staged
development timings.

One release binary generated 6x6 reachable layers 0--8 at each worker count:

| Workers | Generation seconds | Speedup | Peak RSS, GiB |
|---:|---:|---:|---:|
| 1 | 105.736 | 1.00x | 1.05 |
| 8 | 56.406 | 1.87x | 2.62 |
| 32 | 52.868 | 2.00x | 7.43 |
| 64 | 50.267 | 2.10x | 13.27 |

Generation times include checkpoints and exclude the final comparison. Every
parallel checkpoint exactly matched the serial reachable/terminal/active sets:
96,701,369 positions across layers, including 77,052,264 at ply 8. This is a
reachability benchmark, not a complete 6x6 strong solution. The production 5x6
serial run occupied one CPU throughout these measurements. Union and collection
remain significant costs; 64 workers do not imply a 64-fold speedup.

Tests cover both worker backends, 64-way node publication across block
boundaries, exact small-board values, cross-worker checkpoint restarts,
corrupted value-language comparisons, and failure/timeout cleanup. Address
and undefined-behavior sanitizer checks are also run. ThreadSanitizer could
not start on this host (`unexpected memory mapping`), so no ThreadSanitizer
pass is claimed.

## Explicit analysis modes

Running without an analysis-mode option uses exact memoized depth-first search.
This proves the WDL value of the requested root, but does not construct a complete
tablebase:

```sh
./twixt --size 5
./twixt --board 4x7 --moves "B1 A2 C3" --all-optimal
```

Use `--estimate` for a cheap combinatorial peg-diagram estimate, then a bounded
reachable-state census before starting a large computation:

```sh
./twixt --board 5x7 --estimate
./twixt --board 5x7 --census-ply 8 \
  --census-max-states 1000000 --census-seconds 10
```

Add `--dfa-stats` to encode every state in each completed census layer as a
fixed-length peg-plus-link string and construct its exact minimal acyclic DFA:

```sh
./twixt --board 5x6 --census-ply 8 --dfa-stats --no-symmetry \
  --census-max-states 20000000 --census-seconds 0
```

The pilot reports both a simple cells-then-links order and a spatial frontier
order. Counterintuitively, `--no-symmetry` compresses much better: lexicographic
symmetry canonicalization introduces a global dependency that damages DFA
locality, while the DFA shares the reflected sublanguages directly.

The census distinguishes full peg-plus-link states from peg diagrams and reports
their measured link-history multiplicity. Exit status 3 means that a state or
time cap stopped the census before the requested layer was complete.

For a complete WDL database, remove both safety caps explicitly:

```sh
mkdir -p tables
./twixt --board 4x7 --strong \
  --census-max-states 0 --census-seconds 0 \
  --db-out tables/4x7.twixtdb
```

`--strong` enumerates every reachable canonical nonterminal state layer, stopping
at wins, exhausted boards, and sound proven-dead draws, then evaluates all stored
states backwards. Add `--no-draw-pruning` if the database must retain descendants
of positions where neither player can possibly connect. An interrupted or capped
run exits with status 3 and does not write a database.

The explicit implementation keeps each layer and all WDL values in memory. It is a
clear reference implementation and is suitable for genuinely small boards, but it
is **not yet an external-memory solver**. Do not launch 5x6 or larger solely from
the peg-diagram estimates below: run progressively deeper capped censuses and use
their measured peak layer size to check RAM and disk requirements first.

Child canonicalization reuses all transformed versions of the parent and patches
only the new peg and links. Board scans and connectivity queues are likewise
fixed-size bit operations; these details matter substantially in deep censuses,
where symmetry reduction is performed once per generated move.

## Querying a WDL database

The database stores values relative to the side to move. Query the initial state
or replay a position using the same A1-style move notation as the solver:

```sh
./twixt --probe-db tables/4x7.twixtdb
./twixt --probe-db tables/4x7.twixtdb --moves "B1 A2 C3"
```

The probe prints `WIN`, `DRAW`, or `LOSS`, followed by every legal move as `W`,
`D`, or `L`. A trailing `*` marks a game-theoretically optimal move. For agent
evaluation, keep a fixed holdout set of reachable positions, query each WDL and
optimal-move set, and compare the agent's value sign and policy mass on starred
moves.

## Board-size estimates and solver choice

These are alternating legal **peg-diagram counts**, not measured reachable
peg-plus-link state counts or runtime forecasts. The optimistic column divides by
the four geometric symmetries preserving the white-first reachable parity
class. Although square canonicalization tests eight images, diagonal/color
swaps also flip the turn and do not double the reduction within reachable
layers. Earlier square-board estimates divided by eight and were too optimistic.

| Board | Peg diagrams | Optimistic / symmetry |
|---:|---:|---:|
| 5x5 | 2.065e7 | 5.162e6 |
| 4x7 | 1.340e8 | 3.350e7 |
| 5x6 | 1.831e9 | 4.578e8 |
| 4x9 | 9.228e10 | 2.307e10 |
| 5x7 | 1.594e11 | 3.985e10 |
| 6x6 | 5.739e11 | 1.435e11 |
| 5x8 | 1.376e13 | 3.440e12 |
| 7x7 | 1.498e17 | 3.745e16 |
| 8x8 | 3.625e23 | 9.062e22 |

For a larger research target, census 5x6 first. If its measured layers fit, compare
5x7 and 6x6; 4x9 has a similar optimistic count to 5x7 but is geometrically skewed.
On a 32-core AMD EPYC 9115 host (single-threaded solver), the optimized code
measured the following checkpoints; these are layer sizes, not total database
sizes:

| Board / ply | Full states | Peg diagrams | Link multiplicity | Time |
|---:|---:|---:|---:|---:|
| 5x6 / 7 | 608,789 | 554,462 | 1.0980 | 0.82 s |
| 5x6 / 11 | 36,926,578 | 20,281,337 | 1.8207 | 174 s |
| 6x6 / 7 | 3,971,765 | 3,734,024 | 1.0637 | 12.0 s |

The first compressed-game-solving pilot produced these exact minimal-DFA
measurements using the spatial frontier variable order and no symmetry
canonicalization:

| Board / ply | Represented positions | DFA states | DFA arcs | Positions / DFA state |
|---:|---:|---:|---:|---:|
| 5x6 / 7 | 2,433,178 | 180,450 | 253,418 | 13.48 |
| 5x6 / 8 | 9,526,546 | 572,936 | 809,236 | 16.63 |
| 6x6 / 6 | 3,214,752 | 227,263 | 317,708 | 14.15 |
| 6x6 / 7 | 15,887,060 | 642,738 | 899,311 | 24.72 |

Log-log fits over the measured growing layers give preliminary exponents of
about 0.75 for 5x6 and 0.73 for 6x6 in `DFA states = c * positions^alpha`.
These shallow-layer fits are encouraging but are not forecasts until deeper
layers and, especially, W/L/D subsets are measured. `--dfa-stats` is the offline
compression experiment; `--symbolic` implements direct generation and retrograde
on automata.

A 7x7 census already had 8,928,002 full states at ply 6 and exceeded a
30-million-state cap while constructing ply 7. Its previously reported
multiplicity 7.0820 was invalid: a capped census had divided the complete layer
size by a partially inspected peg count. Capped rows now print `NA` for
unfinished statistics. An explicit 7x7 or 8x8 WDL table is not a realistic target
for this implementation.
The practical next complete target is 5x6; 6x6 is a stretch target requiring a
larger symbolic W/L/D experiment. For explicit enumeration it would require
compact external-memory generation; successful DFA compression could avoid
that storage requirement, but its full runtime is still unmeasured.

BNS/df-pn is useful when only the initial WDL result or selected positions need a
proof. A strong database, however, requires values for all reachable positions,
so proof-number search is not the core generation algorithm here. A ZDD may become
valuable if deeper censuses show that many link histories share structure. The
solver reports that multiplicity specifically so this can be decided from data;
introducing a ZDD before that measurement would not eliminate the move-order-
dependent link state.

## WDL file format

`TWIXTDB1` is a portable little-endian binary format:

1. Eight-byte magic `TWIXTDB1`.
2. Seven 32-bit fields: version (`1`), width, height, symmetry flag,
   draw-pruning flag, compact-key word count, and layer count.
3. Total state count as a 64-bit integer.
4. The canonical root key, then one 64-bit state count for every layer.
5. Sorted records grouped by ply. Each record is the compact key followed by one
   signed byte: `-1` loss, `0` draw, or `+1` win for the side to move.

The reader validates the magic, version, geometry, state counts, value bytes, and
exact file length before serving binary-search lookups.

## Publishing databases outside git

Generate the raw database, record its digest, and package it using relative paths:

```sh
sha256sum tables/5x6.twixtdb
tar -cf - tables/5x6.twixtdb | \
  zstd -q -12 --long=27 -T0 -o twixt_5x6_wdl.tar.zst
sha256sum twixt_5x6_wdl.tar.zst
```

Upload the archive to Zenodo (or another durable data repository), then replace
the placeholders below with the actual result, exact state count, raw WDL digest,
archive digest, and DOI-backed download link. Never enter a digest from an
incomplete run.

| Board | Root WDL | States | WDL SHA-256 | Archive SHA-256 | Download |
|---:|:---:|---:|:---|:---|:---|
| 4x7 | not generated | - | - | - | not published |
| 5x6 | not generated | - | - | - | not published |
| 5x7 | not generated | - | - | - | not published |
| 6x6 | not generated | - | - | - | not published |

After downloading, verify both the archive and extracted WDL against the README:

```sh
sha256sum twixt_5x6_wdl.tar.zst
tar --zstd -xf twixt_5x6_wdl.tar.zst
sha256sum tables/5x6.twixtdb
```

## Files

- `twixt.cpp` - solver, census, strong retrograde, WDL writer, and WDL probe.
- `symbolic_dfa.hpp` - exact DFA set algebra, filtering, counting, and collection.
- `parallel_dfa.hpp` - shared-memory workers, process fallback, and delta transfer.
- `twixt_symbolic.hpp` - symbolic TwixT moves, terminal sets, retrograde, and files.
- `symbolic_dfa_test.cpp`, `parallel_dfa_test.cpp`, `test_symbolic.py` - regression tests.
- `Makefile` - optimized build plus short tests.
- `README.md` - rules, reproducibility instructions, format, and checksums.
