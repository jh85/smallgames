# Design notes — morris solver

## Rank/unrank conventions (paper ambiguity resolutions)

* Ranks are zero-based. Each node stores the accepting-path count of its 0-child
  (`cntlo`). rank: add `cntlo` at every taken 1-branch. unrank: at a node, take the
  1-branch iff `h >= cntlo` (subtracting), else the 0-branch. This inverts the paper's
  printed §2.3 procedure (its inequality direction is a misprint); proven by exhaustive
  round-trips (all 1,680 raw 3MM configs; every rank of every small game) and by the
  order property: ZDD rank order == lexicographic order of the item string (white bit
  before black bit per point, point 0 most significant). That property justifies
  canonicalization by minimal interleaved code — identical representative to the paper's
  "smallest first-ZDD integer" — computed with byte LUTs instead of 16 ZDD walks.
* The paper's "maximum integer 264,369,400,848" equals the accepted-configuration count
  (our ranks run 0..count-1). Algorithm 1's `N_d` indexing and the `2m-1` terminal-depth
  test are implementation artifacts of the paper; our canonical memoized construction
  (frontier: piece counts + same-point-occupied flag, zero-suppression, hash-consing)
  builds the minimal ZDD directly. Table 11 node counts correspond to a different
  variable order (3MM: paper 41 < our canonical 98, impossible for the same family+order)
  and are reported as an unresolved, functionally irrelevant discrepancy.

## Pseudo-reachability filter

`minMillEvents(mask)` = exact minimum hitting set over the color's complete mills (each
formation event's stone is the last-placed of every mill through its point; any hitting
set is realizable by ordering non-chosen stones first). Config unreachable if
`opp_remaining + minMillEvents(own) > N` for either color. Closure under legal moves is
proven (non-capturing moves cannot create mills; capturing moves add ≤1 event and remove
an opponent piece), asserted at runtime by in-index lookups of all movement children.
Placement uses the *unfiltered* canonical index (the per-layer sound filter varies with
hands); out-of-index phase-2/3 boundary children (possible only from pseudo-unreachable
placement states) are valued DRAW by convention and counted.

## Symmetry groups

Board group = adjacency+mill-preserving candidates from D4 x ring-flip (8 or 16; extra
pure-graph automorphisms are detected and rejected). For 3-3 subsets of flying games the
uniqueness group is the full mill-hypergraph automorphism group (backtracking search;
order 48 = D4 x S3 on the 3-ring boards): once both players fly, adjacency is irrelevant
forever (piece counts never increase), so these permutations are game isomorphisms —
verified structurally on 5,000 random successor multisets and by exact reproduction of
the paper/Gasser 3-3 counts. Never used for placement states. No color swap anywhere.

## Second ZDD and integrity

Per-subset selections plus the paper's global set are built as one shared hash-consed
pool in a single sweep of the ZDD1 domain (blockwise sparse tries over the rank bits,
Algorithm-3 style with 2^20 blocks; upper levels combined after joining). Membership and
selRank walks enforce zero-suppressed skipped-level semantics (a 1-bit at an elided level
means "not a member"), which is what makes out-of-index detection alias-free. Hash-cons
is chained hashing under 65,536 striped mutexes; a prior lock-free open-addressing
variant produced rare nondeterministic path loss under contention and was replaced. Every
build must pass: per-subset ZDD counts == sweep tallies == (for the global root) their
sum, else the build throws.

## Retrograde solver

Phase 2/3: partitions (W,B) ascending in W+B (captures strictly descend). Per partition,
synchronous double-buffered value iteration: WIN if any child LOSS; LOSS if children
exist and are all WIN; DRAW if all children decided and best is DRAW; else unknown, to a
fixed point, then unknown := DRAW (infinite-play semantics). Children: atomic move+capture
generation, canonicalize, rank; same-partition lookups read the previous buffer (strict
iterations), cross-partition lookups read completed tables; capture-to-2-pieces is an
inline WIN; stuck side-to-move is a LOSS seed. Partitions are persisted immediately
(tmp+rename) and reloaded on restart.

Placement: exactly one hand pair per layer H (white to move iff wh==bh, White first), so
layers are solved 1..2N with a single acyclic sweep each, consulting layer H-1 and the
phase-2/3 boundary (full-board => DRAW for 12/16MM; <3 pieces at the transition: the side
to move loses, else the short opponent loses). The empty board is the last state of layer
2N; its value is the game value.

Correctness spine: the identical pipeline strongly solves 3MM with every reachable state
equal to an independent flat solver, and color-mirror partition pairs (e.g. 3-5 vs 5-3)
independently produce identical W/D/L tallies at 12MM.

## File formats

ZDD1/ZDD2 files: magic, board hash, structural checksum, node arrays, roots, counts.
WDL files: 32-byte header (magic, board hash, state count, partition key) + packed 2-bit
values. A finalized table contains no 00 entries (verified by the table verifier).
SHA-256 sums in data/mNN/MANIFEST.sha256.
