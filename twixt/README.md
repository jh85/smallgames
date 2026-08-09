# Small-board TwixT

This directory contains an exact C++17 solver for square and rectangular
auto-link computer TwixT. It can solve a single position by depth-first search,
measure exact reachable layers, or strongly solve a board by forward enumeration
and layered WDL retrograde analysis. Completed databases can be queried directly,
including the value of every legal move, which is useful for evaluating an
AlphaZero-style agent.

Large generated databases are deliberately **not stored in this repository**.
The `tables/` directory and `*.twixtdb` files are ignored by git. Published
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
write/read/probe round-tripping, and a short rectangular-board census.

## Three analysis modes

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

The current implementation keeps each layer and all WDL values in memory. It is a
clear reference implementation and is suitable for genuinely small boards, but it
is **not yet an external-memory solver**. Do not launch 5x6 or larger solely from
the peg-diagram estimates below: run progressively deeper capped censuses and use
their measured peak layer size to check RAM and disk requirements first.

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
the geometric symmetries used by the solver.

| Board | Peg diagrams | Optimistic / symmetry |
|---:|---:|---:|
| 5x5 | 2.065e7 | 2.581e6 |
| 4x7 | 1.340e8 | 3.350e7 |
| 5x6 | 1.831e9 | 4.578e8 |
| 4x9 | 9.228e10 | 2.307e10 |
| 5x7 | 1.594e11 | 3.985e10 |
| 6x6 | 5.739e11 | 7.174e10 |
| 5x8 | 1.376e13 | 3.440e12 |

For a larger research target, census 5x6 first. If its measured layers fit, compare
5x7 and 6x6; 4x9 has a similar optimistic count to 5x7 but is geometrically skewed.
An early 5x6 census through ply 5 produced 31,898 full states from 31,420 peg
diagrams (multiplicity 1.0152), but shallow multiplicity is not a safe estimate of
the late layers.

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
- `Makefile` - optimized build plus short tests.
- `README.md` - rules, reproducibility instructions, format, and checksums.
