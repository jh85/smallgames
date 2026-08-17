# Strongly Solving Small-Board Fanorona — Implementation Notes

This document describes a C++17 program that builds WDL (win/draw/loss)
tables for Fanorona on small rectangular boards, i.e. it **strongly solves**
these variants. It is written for an independent code review: it states the
rules that were implemented, the algorithmic design, the verification that was
performed, the results, and the known weak spots a reviewer should scrutinize.

Project layout:

```
src/fanorona.{h,cc}  board geometry, exact rules, move generation, perft
src/index.{h,cc}     state-space indexing (combinadic, color fold, D4)
src/retro.{h,cc}     parallel layered retrograde WDL builder
src/bns.{h,cc}       BNS / df-pn weak solver (independent verifier)
src/main.cc          CLI (perft, idextest, build, query, root, verify,
                     crosscheck, solve)
Makefile             g++ -std=c++17 -O3 -march=native -pthread
out/<variant>/       built tables: values/<a>_<b>.wdl + report.txt
FanoronaNote.pdf     Schadd et al., "Fanorona is a draw", ICGA Journal 2007
Fanorona.pdf         Schadd et al., "Best Play in Fanorona Leads to Draw",
                     NMNC 4(3):369–387, 2008
```

Build: `make`. Everything below refers to the resulting `fanorona` binary.

**Conventions.** Board sizes are given as width × height as passed to the
program (`fanorona build <W>x<H>`). The game itself is invariant under
transposition (the diagonal-line rule, diagonals from points with `x+y` even,
is transpose-invariant), so W×H and H×W are the same abstract game — but a
"fill the camps, alternate the middle row" starting position is
orientation-dependent, which matters in §2. (The papers use the opposite,
height × width, notation; see §1.3.) All values below are for the
**side to move**; White moves first, so "White wins" = first-player win,
"Black wins" = second-player win.

---

## 1. Results

### 1.1 Values of starting positions

**A note on setups first (important).** There are two plausible ways to
generalize the Fanorona opening to arbitrary board widths, and they are
*different positions* with *different values*; see §2 for the full analysis.
The **standard** construction (the actual 5×9 tradition and the one Schadd
et al. use) makes the middle row symmetric under 180° rotation + color swap:
width 3 → `B.W`, width 5 → `BW.BW`, width 7 → `BWB.WBW`, width 9 →
`BWBW.BWBW`. Our program's built-in `InitialPos()` instead generates an
"alternate starting and ending with Black" (**palindromic**) middle row:
width 3 → `B.B`, width 5 → `BW.WB`, width 7 → `BWB.BWB` — which does **not**
reproduce the traditional setup on any width (§2.1, §8 item 5).

Values from the completed tables:

| Board W×H | Standard setup (paper/tradition) | Value | `InitialPos()` (palindromic) | Value |
|---|---|---|---|---|
| 3×3 | `BBB/B.W/WWW` | **White wins** | `BBB/B.B/WWW` | Black wins |
| 3×5 | `BBB/BBB/B.W/WWW/WWW` | **White wins** | `BBB/BBB/B.B/WWW/WWW` | White wins |
| 5×3 | `BBBBB/BW.BW/WWWWW` | **White wins** | `BBBBB/BW.WB/WWWWW` | Black wins |
| 3×7 | `BBB/BBB/BBB/B.W/WWW/WWW/WWW` | **White wins** | `.../B.B/...` | Black wins |
| **5×5** | `BBBBB/BBBBB/BW.BW/WWWWW/WWWWW` | **Draw** | `BBBBB/BBBBB/BW.WB/WWWWW/WWWWW` | **White wins** |

(The 3×7 "standard" row assumes the camps-on-top-and-bottom orientation; the
paper's 3-tall × 7-wide setup was tested via transposition, see §1.3.)

Other middle rows probed on 5×5: `WB.WB` (mirror of the standard) = **Draw**
— necessarily the same value, since reflection is a symmetry of the rules.

Two things stand out: small-board values are extremely sensitive to the
middle-row setup, and on 5×5 the standard setup is a draw while the
palindromic variant is a first-player win.

### 1.2 The 5×5 table

Full strong solve of 5×5 (`out/5x5_d4/`, D4-canonicalized):

- 182 stone-count layers, 1.06×10^11 canonical states total
  (≈ 8.5×10^11 stored states before the D4 fold, ≈ 1.7×10^12 before the
  color fold);
- ~149 GB of 2-bit value files on disk;
- ~8 h wall time on a 64-thread / 125 GB RAM machine;
- largest single layer: {8,9}, 4.06×10^10 superset entries
  (6.57×10^9 canonical states), 1949 s.

### 1.2a State-space sizes vs. reachable-position counts

Three different quantities should not be confused:

- **Combinatorial state space** (every legal board configuration × side to
  move): exactly `2·3^(W·H)`. For 5×5: **1,694,577,218,886** (≈ 1.7×10^12);
  stored color-folded: 847,288,609,443 (≈ 8.5×10^11); D4-canonical as
  built: 1.06×10^11. For 9×5: 2·3^45 ≈ **5.9×10^21** (Schadd et al. quote a
  state-space complexity of ~10^21 from their gapless index "disregarding
  symmetry"; same order).
- **Table size**: the tables store every combinatorial position, reachable
  or not (retrograde analysis does not need reachability).
- **Reachable positions** from the initial position (forward BFS over the
  composite-move graph, `reach` command): known exactly only for the small
  boards —

| Board | Setup | Reachable | Combinatorial | Fraction |
|---|---|---|---|---|
| 3×3 | standard `B.W` | **1,012** | 39,366 | 2.6% |
| 3×3 | palindromic `B.B` | 878 | 39,366 | 2.2% |
| 3×5 | standard `B.W` | **185,204** | 28,697,814 | 0.65% |
| 5×3 | standard `BW.BW` | **444,908** | 28,697,814 | 1.6% |

(3×5 and 5×3 differ because their standard setups are not transposes of
each other.) The exact reachable count for **5×5 is not computed** — it is
bounded above by the table size and would require a bitmap-based BFS over
the D4 index space (~80 GB of bits, ~day-scale) rather than the hash-set
`reach` used for small boards. For **9×5 it is unknown** and far beyond
exact computation.

### 1.3 Comparison with Schadd et al. — full agreement

The 2007 ICGA note (`FanoronaNote.pdf`, Table 1) reports 3×3, 3×5, 5×3, 3×7,
7×3 as wins for White and 5×5, 5×9 as draws, but does not depict the
small-board setups. The 2008 follow-up paper (`Fanorona.pdf`, Table 4 repeats
the values) does depict them:

- **Figure 1** (5×9 initial position): middle row `BWBW.BWBW`.
- **Figure 2** shows the 5×7 and 7×5 initial positions. Board-size notation
  in the papers is **height × width** (ranks × files): their "5×7" is our
  `7x5` (7 wide, 5 tall) and vice versa. The middle rows shown are
  `BWB.WBW` (width 7) and `BW.BW` (width 5).
- **Figure 6** shows the 3×3 initial position and the optimal game: the
  middle row is `B.W` (a balanced 4-vs-4 setup).

All of these are exactly the **standard (180°-rotation + color-swap
symmetric)** family of §2.1 — the same family as the real 5×9 tradition.

With these setups, **every testable Table-4 value is reproduced exactly by
our tables**:

| Paper (H×W) | Middle row | Position queried (our W×H convention) | Paper | Ours |
|---|---|---|---|---|
| 3×3 | `B.W` | 3×3 `BBB/B.W/WWW w` | White | White ✓ |
| 3×5 | `BW.BW` | 5×3 `BBBBB/BW.BW/WWWWW w` | White | White ✓ |
| 5×3 | `B.W` | 3×5 `BBB/BBB/B.W/WWW/WWW w` | White | White ✓ |
| 3×7 | `BWB.WBW` | 3×7 `BWW/BBW/BWW/B.W/BBW/BWW/BBW w` (transpose of their 3-tall × 7-wide setup) | White | White ✓ |
| 7×3 | `B.W` | 3×7 `BBB/BBB/BBB/B.W/WWW/WWW/WWW w` | White | White ✓ |
| **5×5** | `BW.BW` | 5×5 `BBBBB/BBBBB/BW.BW/WWWWW/WWWWW w` | **Draw** | **Draw ✓** |

(Their 5×7, 7×5, 3×9, 9×3 and 5×9 values are untested — those tables were
not built; 3×9 is out of reach on the reference machine.)

**Conclusions.** (a) There is no discrepancy anywhere: the paper's values
are exactly reproduced once their setups are used. (b) Our 5×5 table
**upgrades their 5×5 result from a weak solve to a strong solve** (they
proved only the initial position's value with PN² + 9-piece databases; we
computed the value of every legal position). (c) The palindromic 5×5 variant
`BW.WB` — a position neither paper considered — is a **White win** (§2.2),
demonstrating that the 5×5 outcome hinges on a single stone's color in the
middle row.

### 1.4 Result history (important for reviewers)

The 5×5 `BW.WB` value was **White win in every computation ever run**: the
first full retrograde build, the rebuilt tables after a bug fix (verified
byte-identical), an independent BNS weak-solve using only ≤9-stone databases
(6,239 search nodes), and an independent BNS weak-solve with **no database
at all** (170,440 nodes, confirmed under both branch-number and
proof-number arithmetic). Debugging during the project found and fixed real
bugs (see §7), but none of them changed this value. The draw outcomes belong
to **different starting positions** (the standard `BW.BW` and its mirror
`WB.WB`), not to a revised value of the `BW.WB` position.

---

## 2. The 5×5 starting position: standard vs. palindromic variant

### 2.1 The standard construction (and how `InitialPos` deviates from it)

The traditional 5×9 Fanorona opening position — Figure 1 of both papers, and
also the current Wikipedia depiction — is:

```
B B B B B B B B B
B B B B B B B B B
B W B W . B W B W      <- middle row "BWBW.BWBW"
W W W W W W W W W
W W W W W W W W W
```

Two independent corroborations that this (and not the palindrome
`BWBW.WBWB`) is the paper's setup: the rendered Figure 1 itself (f3 is
clearly a filled Black stone), and the paper's own move notation — they list
`d3-e3A` as an opening **approach** capture by White, which is only possible
if f3 is Black; in the palindrome `BWBW.WBWB` the f3 point is White and
`d3-e3` would be a withdrawal instead.

The defining symmetry of this position is invariance under **180° rotation
combined with a color swap** — the board looks identical from each player's
seat (a "fairness" symmetry). The natural generalization to any odd width
keeps that property: the left half of the middle row alternates starting
with Black, and the right half is the rotated color-swapped copy:

| width | middle row (standard) |
|---|---|
| 3 | `B.W` |
| 5 | `BW.BW` |
| 7 | `BWB.WBW` |
| 9 | `BWBW.BWBW` (the 5×9 tradition) |

This is precisely the family used in the paper's small-board figures
(§1.3), and on width 5 it yields the traditional Fanoron-Dimy layout. Note
that on widths ≡ 1 (mod 4) an equivalent description is "both halves of the
middle row identical, alternating from Black"; on widths ≡ 3 (mod 4) that
simpler phrasing produces a *different* (non-symmetric) row, which is why
the symmetry phrasing is the safe one.

Our program's `InitialPos()` instead implements "alternate starting and
ending with Black", i.e. a **palindromic** middle row (`BW.WB` on width 5,
`BWBW.WBWB` on width 9 — note it would not even reproduce the real 5×9
setup). That construction came from a misremembered 5×9 diagram; it is
mirror-symmetric but lacks the rotation+swap fairness symmetry, and it is
found nowhere in the papers. On width 5 the two constructions differ in
exactly one stone's color (the d3 point: Black in `BW.BW`, White in
`BW.WB`), and that single stone flips the game-theoretic value.

### 2.2 Values

- `BBBBB/BBBBB/BW.BW/WWWWW/WWWWW w` (standard, = the paper's setup) → **Draw**
- `BBBBB/BBBBB/WB.WB/WWWWW/WWWWW w` (mirror of standard) → **Draw**
- `BBBBB/BBBBB/BW.WB/WWWWW/WWWWW w` (palindromic variant, `InitialPos`) → **White wins**

A winning first move on the `BW.WB` variant: the White stone directly below
the empty center steps up into the center, approach-capturing the two Black
stones on the middle file above the center. (BNS reproduces this move as the
start of its principal variation.)

### 2.3 Discussion

- Once the correct setups are used, our tables agree with Schadd et al. on
  **six out of six** testable board values (§1.3), including the 5×5 draw.
  This is strong evidence that the move generator and the retrograde
  pipeline implement the same game the papers analyze.
- The `BW.WB` White win is proved by the full retrograde table and
  independently by BNS with no database (§1.4). Capture-direction rule
  variants (`seq_rule` 1 and 2, §3) were also tried on this setup via BNS
  plus ≤9-stone databases: the White win was still proved (a proved BNS win
  is sound independent of database coverage).
- One rule is left unspecified by both papers: the stalemate case (side to
  move with no legal move). We play stalemate = loss (hardcoded, §8). The
  perfect Table-4 match suggests it does not matter for the starting
  positions, but a reviewer could quantify its impact (e.g. by counting
  stalemate-terminal states in the tables).
- Lesson recorded for reviewers: every "contradiction with the paper" we
  encountered during this project traced back to setup assumptions, not to
  the engine. The values themselves were stable across all verification
  (§7).

---

## 3. Rules as implemented

Source of truth: the header comment of `src/fanorona.h` and the move
generator in `src/fanorona.cc`. The implementation follows Schadd et al.:

- Stones move one step along grid lines; diagonal lines exist only from
  points with `(x+y)` even (this is what makes the Fanorona graph
  non-rectangular; it is precomputed in `Geom::Init`).
- **Approach capture**: moving onto an empty point captures the maximal
  contiguous run of enemy stones starting at the next point along the
  direction of movement. **Withdrawal capture**: the same for the run of
  enemy stones starting directly behind the origin. If a single step offers
  both, they are two distinct legal moves (a paika step is only legal when
  neither exists, per the next rule).
- **Capture is mandatory** when at least one capturing step exists.
- **Continuation captures** with the same stone are optional (the player may
  stop after any step). Within one turn the capturing stone may not revisit
  a point it already occupied that turn, and the direction rule applies:
  - `seq_rule = 0` (default; literal reading of the paper): a step may not
    repeat the immediately preceding direction;
  - `seq_rule = 1`: no direction may be used twice in the whole sequence;
  - `seq_rule = 2`: a step may neither repeat nor reverse the preceding
    direction.
  All published tables were built with the default.
- **Win**: the opponent has no stones. A side to move with no legal move
  **loses** (stalemate = loss; this is hardcoded in `retro.cc`, not
  switchable from the CLI despite a header remark).
- **Draw**: infinite play. The tablebase has no repetition rule; positions
  from which neither side can force a win are draws by the retrograde
  fixpoint, which matches "repetition/infinite play = draw".

### 3.1 Composite captures — the key state-space reduction

A WDL state is a **turn-start position**: (board, side to move). A full
capturing sequence (including the choice to stop after any step) is collapsed
into a single **composite move**, with one child per reachable stop point.
This is strategically exact, because during a sequence only the mover makes
decisions and stopping is allowed after every step: for any multi-step
sequence there is a composite edge to the same final board, and conversely
every composite edge is a legal real turn. Game values are therefore
identical, while the state space stays at ~2·3^(W·H) instead of blowing up
with path-dependent mid-turn state (visited set, last direction, current
point). Duplicate child boards arising from different step sequences are
deduplicated (`keep_dups=false`; perft can disable dedup to cross-check
against the stepwise generator — both must and do agree exactly).

---

## 4. State indexing (`src/index.{h,cc}`)

- **Color fold**: every position is stored as *White to move*; a
  Black-to-move position is color-swapped. The stored value is always the
  value for the side to move. This halves the space and makes stone counts
  unordered: a **layer** is a pair `{a,b}`, `a ≤ b`, with sub-layer 0 holding
  (#white = a, #black = b) and sub-layer 1 (when a ≠ b) holding
  (#white = b, #black = a).
- **Combinadic indexing** within a layer: colex rank of the White set times
  C(N−a, b), plus the restricted colex rank of the Black set among non-White
  points (`RankSet` / `RankRestricted` and inverses).
- **Symmetry-folded mode**: each position is canonicalized to the orbit
  representative minimizing (first White stone point, white bits, black bits)
  over a symmetry group — **D4** (8 transforms, square boards, `--d4`) or
  **D2** (4 transforms {id, rot180, flip-x, flip-y}, `--d2`). The index
  space is the *superset* "first White stone lies in the fundamental
  domain"; every representative lands in it exactly once and ranks are
  directly computable, while slots of non-representatives are never used
  (~2× overhead vs. the ideal, still ~4× smaller than plain overall — for
  5×5 D4, 1.06×10^11 vs. 8.5×10^11 stored states). Layers `{0,b}` have no
  distinguished White stone and stay plain.
- **Automorphism validation**: a geometric transform is only accepted as a
  symmetry if it preserves the line graph (verified at init via adjacency).
  This rejects transforms that swap strong and weak points — so `--d2`
  requires both side lengths odd (covers 3×9, 3×5, 5×3, 3×7, …) and `--d4`
  now requires odd side length (even-square D4 previously *silently* folded
  non-equivalent positions; no such table was ever built).

`idextest` checks rank/unrank round-trips plus orbit consistency (all
transformed images canonicalize to the same slot) exhaustively on small
layers, for both fold modes.

---

## 5. Retrograde builder (`src/retro.{h,cc}`)

Layers are processed in increasing `a+b` order. Captures strictly reduce the
stone count, so **cross-layer edges point strictly downward** and are resolved
at the parent's initialization from the already-computed layers. **Paika
moves stay within a layer**, forming arbitrary (reversible) graphs that are
solved by propagation to a fixpoint.

Per layer, one byte per state in RAM:

- `0..250` — unresolved: number of children not yet known to be a win *for
  the child* (all same-layer children, plus lower-layer draws, which can
  never become wins);
- `251` — counter overflow; true count kept in a side map (see §8);
- `252` / `253` — proved WIN / proved LOSS.

Algorithm per layer:

1. **Initialization** (parallel over states): decode the board, generate
   composite children, evaluate lower-layer children from the mmap'd `.wdl`
   files. A child that is a LOSS for the child immediately proves the parent
   WIN. Terminal states: side to move without stones = LOSS; opponent
   without stones = WIN; no legal move (stalemate) = LOSS.
2. **Propagation**: when a state resolves as LOSS, its in-layer reverse
   predecessors are generated and marked WIN; when a state resolves as WIN,
   each predecessor's counter is decremented, and a counter reaching 0 marks
   the predecessor LOSS. Predecessors are found by reverse move generation;
   the **mandatory-capture rule is honored by rejecting any predecessor that
   itself has a capture available** (a paika edge out of such a predecessor
   is illegal). This reject step is a classic source of retrograde bugs and
   deserves review attention.
3. **Fixpoint**: unresolved states are draws. Values are written as 2 bits
   per state to `values/<a>_<b>.wdl` (mmap; `kUnknown` must not appear in a
   completed layer). Completed layers are skipped on restart (resumable).

Memory: peak RAM ≈ 1 byte × largest layer's superset size (≈ 40 GB for 5×5
D4 layer {8,9}) plus page-cache pressure from the mmap'd value files of all
lower layers (evictable). This 1-byte-counter design is why 10^12–10^13
total states are feasible only if no *single layer* exceeds available RAM;
this is what rules out 3×9 on the reference machine (peak layer
≈ 4.6×10^11 states).

---

## 6. BNS weak solver (`src/bns.{h,cc}`) — independent verifier

A from-scratch port of the Branch Number Search from the JHBR3 Shogi
checkmate solver (`mate/bns.{h,cc}`), which implements Okabe's route
branch-number method; search shape follows cshogi/dlshogi's `dfpn_inner`.
OR node = attacker (the side we try to prove a forced win for) to move; both
node types expand all legal moves. Notable design points:

- branch-number arithmetic copied verbatim from the reference; a `kPnDn`
  template switch swaps in proof/disproof-number arithmetic as a control
  (both modes proved the 5×5 `BW.WB` root);
- transposition table keyed by (position hash, **ply**) — the ply in the key
  prevents cross-depth value feedback cycles (documented in the header);
- route-dependent verdicts (repetition = failure for the attacker) are kept
  out of the TT via path-scoped overrides anchored to a DFS ancestor;
- optional endgame-database probe (`--db`, `--db-stones`) truncates the
  search; a proved win is sound regardless of DB coverage because unbuilt
  layers probe as `kUnknown`.

Roles: (a) verify the retrograde root value through a completely separate
code path (only the move generator is shared); (b) answer questions the
tables cannot (alternative setups, `seq_rule` variants) without building new
tables.

---

## 7. Verification performed

| Check | Scope | Result |
|---|---|---|
| `idextest` | rank/unrank round-trip + D4 orbit consistency | OK |
| `perft` composite vs. stepwise (real turn structure, sequences counted with multiplicity) | several boards/depths | exact agreement |
| `verify` — independent brute-force value iteration over a hash map (shares only the move generator) | **every** 3×3 and 3×5 position | 0 mismatches |
| `crosscheck` — plain build vs. D4 build of the same board | 2.8×10^9 positions | 0 mismatches |
| D2 fold: `verify` (brute force) + `crosscheck` vs. plain | every 3×3 and 3×5 position, both checks | 0 mismatches |
| D2 fold: `idextest` incl. orbit consistency | 3×3, 3×5, **3×9** (target board) | OK |
| root values vs. Schadd et al. Table 4, **using the setups depicted in their 2008 paper** (Fig. 1, 2, 6) | 3×3, 3×5, 5×3, 3×7, 7×3, 5×5 | **all 6 match exactly** (§1.3) |
| 5×5 `BW.WB` root via BNS | with ≤9-stone DB (6,239 nodes); no DB (170,440 nodes); both arithmetics | White win |
| dense-layer rebuild after race fix | layers {7,9}, {8,9}, {9,9} | md5 byte-identical |

Bugs found during the project (all fixed before the final tables were
written; none affected any table value): an initialization-side race in the
counter-overflow path (>250 children), and minor issues caught by the
brute-force cross-checks on 3×3/3×5 before any large build was started.
Separately, the project's `InitialPos()` was found late to generate a
non-traditional (palindromic) middle row — a *setup* bug, not an engine bug;
all table values are unaffected since tables cover every position (§8).

---

## 8. Known limitations / review targets

1. **Stalemate = loss is hardcoded** in `retro.cc` (`res = kCLoss` on zero
   moves); the `fanorona.h` comment says "switchable to draw" but no such
   CLI option exists. The brute-force verifier applies the same rule, so the
   cross-checks do not independently validate this *rule choice* (only its
   consistent implementation). Neither paper specifies the stalemate case;
   the perfect Table-4 match is empirical evidence that it does not affect
   the starting positions (§2.3).
2. **The counter-overflow path never fired in production** — no 5×5 position
   exceeded 250 composite children. The path was reviewed and the fix
   verified by rebuilds of dense layers, but it has never executed on real
   data.
3. **Unreachable 25-stone layers** (`a+b = 25`, full board) are built and
   come out all-LOSS via the stalemate rule; harmless, but a reviewer should
   confirm they cannot leak into reachable values (they cannot: play starts
   at 24 stones and captures only decrease the count).
4. `out/5x5_d4/report.txt` has **duplicate trailing lines** for {7,9},
   {8,9}, {9,9} from the post-fix rebuild; the `.wdl` files were verified
   byte-identical between builds. Cosmetic only.
5. **`InitialPos()` generates the palindromic (non-traditional) middle row**
   (§2.1): `fanorona root` and default `build`/`solve` runs therefore report
   values for the variant setup, not the standard one. This does not affect
   any *table* (tables cover all positions and both setups were queried
   explicitly), but a reviewer checking `root` output against the
   literature must use `--pos` with the standard setup strings of §1.1.
   Fixing `InitialPos()` to the standard construction is a one-line change
   left to the maintainer's discretion (it would change what `root`
   reports).
6. The BNS solver shares the move generator with the retrograde builder, so
   a *rules* bug (not an indexing/algorithm bug) could in principle cancel
   out between the two. The defense against this is the brute-force
   `verify` on 3×3/3×5 (still shares the move generator) and the exact
   match with the paper's Table 4 on six boards — an outside reviewer could
   strengthen this further by re-implementing move generation independently.

---

## 9. Reproducing

```sh
make

# Build a full table (resumable; skips completed layers)
./fanorona build 5x5 --d4 --out out/5x5_d4 --threads 64

# NB: root uses InitialPos() = the palindromic variant (§8 item 5)
./fanorona root 5x5 --d4 --out out/5x5_d4

# The standard (paper's) 5x5 setup, and the palindromic variant
./fanorona query 5x5 --d4 --out out/5x5_d4 --pos "BBBBB/BBBBB/BW.BW/WWWWW/WWWWW w"
./fanorona query 5x5 --d4 --out out/5x5_d4 --pos "BBBBB/BBBBB/BW.WB/WWWWW/WWWWW w"

# Independent weak solve (optionally with a DB, optionally PN arithmetic)
./fanorona solve 5x5 --pos "BBBBB/BBBBB/BW.WB/WWWWW/WWWWW w"
./fanorona solve 5x5 --d4 --db out/5x5_d4 --db-stones 9

# Checks
./fanorona idextest 5x5 --d4
./fanorona perft 5x5 5            # add --stepwise to cross-check generators
./fanorona verify 3x5 --out out/3x5
./fanorona crosscheck 5x5 --a out/5x5_plain --b out/5x5_d4 --max-stones 8
./fanorona reach 3x5 --pos "BBB/BBB/B.W/WWW/WWW w"   # reachable count (small boards)
```

Other CLI options: `--seq-rule {0,1,2}` (capture-direction variant),
`--max-stones S` (partial tables), `--tt-mb`, `--max-ply`, `--nodes`
(BNS limits), `--setup` (perft from a given position), `--d2` (D2 symmetry
fold for rectangular boards with both sides odd).

---

## 10. Next target: strong-solving 3×9

Feasibility is governed by three constraints: RAM = the largest single
stone-count layer (1-byte counters per state during its build), disk = 2
bits per state for the value files, time ∝ total states processed.

| Board | N | Folded states | Peak layer (plain) | Plain RAM / disk | D2 RAM / disk |
|---|---|---|---|---|---|
| 5×5 | 25 | 8.5×10^11 | 5.3×10^10 | 53 GB / 0.21 TB | (D4: done) |
| **3×9** | **27** | **7.6×10^12** | **4.1×10^11** | **410 GB / 1.91 TB** | **376 GB / 1.70 TB** |
| 4×7 | 28 | 2.3×10^13 | 1.3×10^12 | 1276 GB / 5.7 TB | fold unavailable (even side) |
| 5×6 | 30 | 2.1×10^14 | 1.0×10^13 | 10 TB / 51 TB | — |

3×9 is the largest board that fits a 750 GB RAM / 7 TB NVMe machine; the
next size up (4×7) exceeds RAM even before symmetry savings (a D2 fold is
impossible there anyway: 4 is even, so the only symmetry is a single flip).
The D2 fold's main win on 3×9 is ~4× less propagation work (~1.9×10^12
canonical states instead of 7.6×10^12); the RAM/disk savings are modest
because the superset indexing overhead grows with density. The D2 code is
implemented and validated (§4, §7) but the build was **not** started on the
development machine (128 GB).

Handoff command (on the big machine; expect order 1–2 weeks at 64 threads,
resumable per layer):

```sh
make
./fanorona idextest 3x9 --d2          # smoke test, minutes
./fanorona build 3x9 --d2 --out out/3x9_d2 --threads 64
./fanorona query 3x9 --d2 --out out/3x9_d2 \
    --pos "BBB/BBB/BBB/BBB/B.W/WWW/WWW/WWW/WWW w"  # expect WIN (paper's 9x3: White)
```

Notes for the run: (a) every query/root/solve against this table directory
must repeat `--d2`; (b) the composite-children cap `kMaxMoves` (4096) is
checked and never came close on 5×5 — if a dense 3×9 position ever exceeds
it the build aborts loudly (raise the cap and resume); (c) the counter
overflow path (>250 children) is handled and may actually fire on 3×9;
(d) the value files are mmap'd (1.7 TB), so steady-state NVMe read traffic
is expected, same as the 5×5 build on a 125 GB machine.
