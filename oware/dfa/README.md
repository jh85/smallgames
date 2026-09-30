# Can Oware be solved with compressed DFAs? A measurement

Question (2026-09-29): can the DFA approach of Considine, *Compressed Game
Solving* (arXiv:2411.07273), which we used for Breakthrough up to
6×6 in `/data1/dfa-games-6x6-clean`, give a strong solution of Oware that is
smaller than our WDL tables (516 GB for 6×2/48, ~2.4 TB for 7×2/42)?

**Answer: no.** The sets a DFA solver has to store for Oware do not compress.
Measured on the finished tables, the minimal layered DFA of an Oware layer has
about one state per 6–13 boards, and the ratio does not improve with layer size (the rise from 7 to 13 at large n tracks the value clamp cutting the number of terminals from 105 to 45)
(Breakthrough: ~n^0.45 states for n positions). Encoded as sparsely as
possible, the DFA of a layer is 3–4× *larger* than our perfect-hash table, and
the family of threshold sets a DFA solver would need is 5–14× larger. Because
the DFA method's time advantage comes entirely from operating on compressed
sets, a DFA solve would also be slower than the current solver, not faster.
Details and numbers below; everything is reproducible with the tools in this
directory.

## 1. How a DFA solution of Oware would look

The framework represents a position as a fixed-length string with one
character per square and a set of positions as a minimal layered DFA; moves are
change nodes (fixed before/after character on some layers) chained in a move
graph, and retrograde analysis is `W_{i+1} = W_0 ∪ reverse(L_i)`,
`L_{i+1} = L_0 ∪ (P \ reverse(P \ W_i))` on whole sets.

For Oware the string is the 12 (or 14) pit counts, alphabet 0..seeds. The
value of a board is not a win/loss bit but the pair (gM, gN) of the layer files
(seeds the mover / the other side can force), because the result depends on the
captured scores; a DFA solver would therefore keep, per layer n, the sets

    W^t = { boards : gM >= t }     N^t = { boards : gN >= t }

for every threshold t that matters (2(V−1) sets, up to 52 for 48 seeds), and
the in-layer recursion is exactly the two-set recursion of `wtraj.cpp`
(captures leave the layer, so lower layers are constants). This is the
Oware analogue of Considine's W/L sets, and its final sizes are what
`dfacomp` measures directly from our tables — no DFA move generator is needed
to know how big the answer would be.

Move generation itself would also be awkward: sowing from pit i with s seeds
changes s other pits by +1 (non-local), so as change nodes it is
6 pits × 48 seed counts × up to 11 stages × 48 before-values ≈ 10⁵ nodes per
side, each a materialised DFA operation in the framework. It can be reduced to
~300 sowing variants with a custom linear-time "shift this layer's characters
by +1" primitive plus a few hundred capture patterns (2/3-chains, grand-slam
exception, feeding rule), so it is *expressible*, but this only matters if the
sets compress, which they do not.

## 2. Tools

* `dfacomp <dir> <seeds> <n> [order] [threads] [max_nodes]` — enumerates every
  indexed board of layer n in a chosen pit order, reads its pair code from the
  table, and hash-conses the decision tree bottom-up into the minimal
  multi-valued layered DFA (one terminal per pair code). From it the minimal
  binary DFA of each `{gM >= t}` / `{gN >= t}` is obtained by relabelling
  terminals and re-minimising. Reports nodes and non-rejecting edges per layer
  and per set; verifies 100,000 random boards against the table. Byte estimates:
  *dense* = 4 bytes per character per node (the framework's format),
  *sparse* = 4 bytes per edge + 8 per node.
* `wtraj <dir> <seeds> <n> <t> [order] [threads] [every]` — runs the in-layer
  W/N recursion for one threshold explicitly (bitmaps), converting W_i and N_i
  to minimal DFAs every `every` plies, so we see the DFA sizes a compressed
  solver would carry at each ply and how many plies the fixpoint needs. Checks
  the fixpoint against the table (all runs: 0 mismatches).
* `mvdfa.hpp` — the shared builder (parallel over the first two pits, merge of
  sub-automata, threshold re-minimisation). `make` builds `dfacomp6/7`,
  `wtraj6/7` (6- and 7-pit rows). `sweep.sh`, `sweep2.sh`, `summarize.sh`
  reproduce the tables; raw outputs are in `results/`.

The tables read are the finished ones in
`/data1/smallgames/tmp/smallgames/oware/{v28s,v36s,v48s}` (7×2/28, 6×2/36,
6×2/48 layers ≤ 36).

## 3. Compression of the finished layers

Default pit order 0..11 (mover row then opponent row). Four orders were tried
on 7×2/28 layers 8–18 (mover first, opponent first, reversed, interleaved
i/11−i); they differ by less than 15 % and none changes the picture.
"Threshold sets" is what a DFA solver stores: all `{gM >= t}` and
`{gN >= t}` as separate minimal DFAs.

| Game | n | Boards | Table bytes | MV-DFA nodes | Boards/node | MV-DFA bytes (sparse) | Threshold sets | Their nodes | Their bytes (sparse) | DFA/table |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 6x2/36 | 8 | 75,504 | 56,630 | 11,015 | 6.9 | 2.4e+05 | 16 | 38,448 | 7.88e+05 | 13.9x |
| 6x2/36 | 10 | 351,351 | 307,435 | 56,515 | 6.2 | 1.31e+06 | 20 | 154,659 | 3.38e+06 | 11.0x |
| 6x2/36 | 12 | 1,339,702 | 1,172,242 | 218,066 | 6.1 | 5.32e+06 | 24 | 485,577 | 1.14e+07 | 9.7x |
| 6x2/36 | 14 | 4,381,818 | 3,834,093 | 671,241 | 6.5 | 1.74e+07 | 28 | 1,299,194 | 3.27e+07 | 8.5x |
| 6x2/36 | 16 | 12,685,179 | 12,685,179 | 1,792,862 | 7.1 | 4.91e+07 | 32 | 3,247,217 | 8.76e+07 | 6.9x |
| 6x2/36 | 18 | 33,245,212 | 33,245,212 | 4,284,806 | 7.8 | 1.24e+08 | 36 | 7,632,135 | 2.19e+08 | 6.6x |
| 6x2/36 | 20 | 80,214,915 | 80,214,915 | 9,502,866 | 8.4 | 2.89e+08 | 36 | 16,645,398 | 5.06e+08 | 6.3x |
| 6x2/36 | 24 | 382,628,610 | 334,800,036 | 38,315,045 | 10.0 | 1.29e+09 | 28 | 64,321,334 | 2.15e+09 | 6.4x |
| 6x2/48 | 8 | 75,504 | 56,630 | 11,015 | 6.9 | 2.4e+05 | 16 | 38,448 | 7.88e+05 | 13.9x |
| 6x2/48 | 12 | 1,339,702 | 1,172,242 | 218,066 | 6.1 | 5.32e+06 | 24 | 485,577 | 1.14e+07 | 9.7x |
| 6x2/48 | 16 | 12,685,179 | 12,685,179 | 1,792,862 | 7.1 | 4.91e+07 | 32 | 3,247,217 | 8.76e+07 | 6.9x |
| 6x2/48 | 20 | 80,214,915 | 80,214,915 | 9,502,943 | 8.4 | 2.89e+08 | 40 | 16,852,562 | 5.13e+08 | 6.4x |
| 6x2/48 | 24 | 382,628,610 | 765,257,220 | 39,595,489 | 9.7 | 1.32e+09 | 48 | 69,558,893 | 2.34e+09 | 3.1x |
| 7x2/28 | 8 | 203,476 | 152,609 | 14,659 | 13.9 | 3.26e+05 | 16 | 58,440 | 1.24e+06 | 8.1x |
| 7x2/28 | 10 | 1,143,506 | 1,000,570 | 90,458 | 12.6 | 2.12e+06 | 20 | 311,157 | 7.14e+06 | 7.1x |
| 7x2/28 | 12 | 5,191,732 | 4,542,768 | 581,820 | 8.9 | 1.39e+07 | 24 | 1,490,089 | 3.62e+07 | 8.0x |
| 7x2/28 | 14 | 19,980,780 | 17,483,185 | 2,693,982 | 7.4 | 6.72e+07 | 28 | 5,545,966 | 1.41e+08 | 8.1x |
| 7x2/28 | 16 | 67,366,495 | 58,945,686 | 9,143,334 | 7.4 | 2.4e+08 | 28 | 15,942,221 | 4.27e+08 | 7.2x |
| 7x2/28 | 18 | 203,756,931 | 178,287,317 | 23,830,594 | 8.6 | 6.66e+08 | 24 | 38,637,514 | 1.09e+09 | 6.1x |
| 7x2/28 | 20 | 562,765,840 | 492,420,112 | 54,535,303 | 10.3 | 1.62e+09 | 20 | 83,613,479 | 2.48e+09 | 5.0x |
| 7x2/28 | 22 | 1,438,895,640 | 1,079,171,732 | 110,889,931 | 13.0 | 3.5e+09 | 16 | 164,068,304 | 5.1e+09 | 4.7x |

(6×2/48 rows for n ≤ 16 are identical to 6×2/36 because those layers are
unclamped in both games and their values in seeds do not depend on the total;
the 6×2/48 n = 24 row has two bytes per board in the table, hence the smaller
ratio.)

Reading the table:

* **Boards per DFA node stays at 6–9 for every game and every layer size**
  from 10⁵ to 1.4 × 10⁹ boards. The DFA grows linearly with the layer; there is no
  sublinear regime, unlike Breakthrough (Fig. 3c of the paper) where the ratio
  keeps improving with size. The slightly better ratios at large n come from
  the value clamp (fewer distinct terminals), not from structure.
* Each node has ~4–5 live edges, so even a 4-byte-per-edge encoding costs
  ~0.6 bytes per board for the whole-table DFA against 0.33–1 byte per board
  for the tables (0.5–2 with 7 pits), and the threshold-set family a DFA solver
  needs costs 1.5–3 bytes per board: 5–14× the table (the ratio falls slowly
  with n only because the value clamp leaves fewer thresholds to store).
* A single threshold set compresses somewhat better (25–70 boards per node)
  but is still 3–4× the size of a plain bitmap of the same set, e.g. 6×2/48
  layer 20, `{gM >= 10}`: 30.4 M boards, 1.18 M nodes, 6.6 M edges ≈ 36 MB
  versus a 10 MB bitmap.
* The set of indexed boards itself (the "reachable" constraint) compresses
  perfectly — a few hundred nodes for any layer — which is the only part of
  Oware that behaves like the paper's examples. The *values* on top of it do
  not: after reading the six mover pits, two thirds of all possible mover rows
  are still distinguishable (6×2/36 layer 16: 50,277 nodes for 74,613 mover
  rows), i.e. hardly any two mover rows induce the same value function on the
  opponent rows, and the width peaks inside the opponent row at ~0.66 M nodes.
  Sowing mixes all pits, so the value function has near-maximal communication
  complexity across any cut of the string, which is exactly what a DFA (or a
  BDD) cannot compress.

Extrapolating linearly (which the data support): 6×2/48 layer 48 with
2.0 × 10¹¹ boards would need ~3 × 10¹⁰ DFA states for the whole-table
automaton and ~10¹¹ states for the threshold-set family — against a peak of
~10⁷–10⁸ states in the 6×6 Breakthrough solve.

## 4. What a DFA solve would carry, ply by ply

`wtraj` results (one threshold each; `results/wtraj_*.txt`):

| Game | n | Boards | t | Plies to fixpoint | Final \|W\| | W DFA nodes at ply 5–10 | W DFA nodes at fixpoint | Final \|N\| | N DFA nodes at fixpoint |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 7×2/28 | 10 | 1,143,506 | 5 | 139 | 566,870 | 12,278 (ply 5) | 16,983 | 439,642 | 14,755 |
| 7×2/28 | 14 | 19,980,780 | 7 | 111 | 8,810,478 | 189,094 (ply 5) | 282,971 | 5,582,003 | 236,558 |
| 6×2/36 | 16 | 12,685,179 | 8 | 83 | 4,504,323 | 196,893 (ply 5) | 237,449 | 1,989,907 | 144,818 |
| 6×2/48 | 20 | 80,214,915 | 10 | 78 | 30,445,056 | 1,079,158 (ply 10) | 1,178,852 | 13,830,636 | 742,715 |

* The DFA of W_i is already at 60–90 % of its final size after 5–10 plies and
  then stays there for another 70–130 plies. There is no cheap phase: every
  `reverse()` round works on a set of essentially final size.
* The in-layer recursion needs 78–139 plies to converge even for layers of
  10⁶–10⁸ boards, and each of the up to 52 threshold families needs its own
  iteration. A DFA solver would run on the order of 10⁴ `reverse()` rounds
  per game, each a union of ~300 sowing variants over DFAs of ~n/7 states,
  where the table solver does a handful of sequential sweeps per layer.

## 5. Conclusion

* A DFA-based strong solution of Oware is expressible in the framework but
  would be **larger** than the existing tables (about 3–4× for the whole
  value function, 7–10× for the win/loss threshold sets) and far slower to
  compute. The premise of the method — sublinear compressed sets — fails for
  Oware's value function; only the trivial "sum of seeds / empty opponent pit"
  constraints compress.
* The perfect-hash layered table with 1/3–2 bytes per board is close to the
  information-theoretic floor for storing (gM, gN) per board (log₂ P bits);
  the realistic ways to shrink it further are (a) storing only the WDL bit for
  the reachable score splits, (b) generic entropy coding of the packed layers
  (measured earlier: zstd gives 1.4–2.7×), or (c) not storing the low-value
  middle layers and re-deriving them by search from the top layers. None of
  these is a DFA method.
* Reproduce: `make && ./sweep.sh && ./sweep2.sh && ./summarize.sh`, and
  `./wtraj7 <v28s> 28 14 7 - 16 5` etc. Runs use ≤ 16–32 threads and a few GB
  of RAM (the 7×2/42 solve shares this machine).
