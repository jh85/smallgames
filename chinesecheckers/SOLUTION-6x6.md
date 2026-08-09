# The Strong Solution of 6×6 Chinese Checkers (6 pieces per player)

This document is the complete, self-contained specification of the game
that was solved, every rule and corner case as implemented, the solution
method, and the results. Companion documents: `README.md` (architecture,
tools, validation methodology), `RESULTS.md` (solve statistics).

Solved 2026-08-05. **Result: the initial position is a win for the first
player.**

---

## 1. The board

The 6×6 game is played on the central diamond of a Chinese Checkers
board. Because the gameplay area has even side length, uniform star
corners cannot be drawn, so the board is *only* the 36-cell diamond
(there are no extra corner areas, and no rules about them apply).

Cells live on a skewed grid (x, y), 0 ≤ x, y < 6. Drawn as a diamond,
row k = x + y runs from 0 (top tip) to 10 (bottom tip), with row widths
1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1. Cells are numbered 0–35 row-major
(row k ascending; x ascending within a row):

```
            0                 ← row 0: top tip (0,0)
           1 2                ← row 1
          3 4 5               ← row 2
         6 7 8 9
       10 11 12 13 14
      15 16 17 18 19 20       ← row 5 (widest)
       21 22 23 24 25
        26 27 28 29
         30 31 32             ← row 8
          33 34               ← row 9
           35                 ← row 10: bottom tip (5,5)
```

**Adjacency.** Each cell has up to six neighbors (triangular lattice):
(x±1, y), (x, y±1), (x+1, y−1), (x−1, y+1). In the drawing: the two
horizontal neighbors within the same row, and two neighbors in each of
the rows above and below. This orientation was verified against the
paper's Figure 2: the initial position has exactly six single-step moves
(the alternative diagonal orientation gives eleven).

**Areas.** Player 1 starts in the top triangle, cells {0,1,2,3,4,5}
(rows 0–2), and moves *down*; player 1's goal is the bottom triangle
{30,31,32,33,34,35}. Player 2 is the mirror image: starts in the bottom
triangle, moves up, goal = top triangle. Each player has 6 pieces, so
the game begins with each start area exactly filled.

## 2. Movement rules

A move transfers one of the mover's pieces to a different empty cell, in
one of two ways:

* **Step:** to any empty adjacent cell (any of the six directions —
  sideways and backward moves are allowed; there is no forward-only
  restriction).
* **Hop chain:** a sequence of one or more hops. A single hop jumps in
  one of the six directions over an *adjacent occupied* cell (a piece of
  either color) and lands on the cell immediately beyond, which must be
  empty and on the board. Hops may be chained from each landing cell;
  the chain may stop at any point, even if further hops are available.
  Hopped-over pieces are never captured or removed.

Corner cases, as implemented:

* **The origin is vacated during a chain.** A chain may later hop back
  *across* the origin cell (it is empty once the piece leaves), and may
  even land on it in mid-chain — but a *move* must end on a cell
  different from where it started. Ending a chain back on the origin
  would be a null move / pass, and **null moves are not legal** (tested
  explicitly as a rule variant; it does not reproduce the reference
  results, and passing is not part of Chinese Checkers).
* **No revisiting:** within one chain a landing cell is never visited
  twice (this only prunes duplicate move generation — it does not reduce
  the set of reachable destinations).
* A destination reachable both by a step and by a hop chain is a single
  move (moves are (from, to) pairs; the path taken is irrelevant).
* **There is no restriction on entering or leaving areas as such**: a
  piece may leave its own goal after entering it, may move back into its
  own start area, and may pass through any cell. The only restrictions
  on *resulting positions* are the illegal-state rules of §4.

Players strictly alternate turns; player 1 moves first.

## 3. Winning and terminal states (Def. 1 of the paper)

> A state is **won for player n** if player n's goal area is completely
> filled with pieces and at least one of those pieces belongs to
> player n.

Crucial subtleties:

* The goal must be *filled* — all six cells occupied — but the pieces
  may be **of either color**; only one of them needs to belong to the
  winner. This is deliberate: an opponent piece squatting in your goal
  cannot block you — it *helps fill* your goal. (It also creates
  "shallow" wins on small boards: a single piece jumped deep into its
  goal among five enemy pieces wins immediately — paper Fig. 3(c).)
* **The winner must make the last move.** A state where player n's win
  condition holds is a *terminal win for n* only when it is the
  **opponent's** turn to move — i.e. the position arose from n's own
  completing move. The game ends there: the state is a loss for the
  player to move, with no further moves. (A position where the win
  condition holds for the player *to move* cannot legally exist — see
  §4, part 1.)
* Both win conditions can never hold simultaneously in a reachable
  state (a single move cannot complete both goals, and such a state is
  illegal for whoever would move — §4).

## 4. Illegal states

Illegal states are positions excluded from the game entirely: they are
never assigned a value, and **any move whose resulting position would be
illegal does not exist** ("it is illegal to take an action that leads to
this state"). Two rules define them.

### Part 1 (Def. 2): the mover already "won"

> A state is illegal if the win condition of the **player to move** is
> satisfied.

Consequences:

* A player can never "move into" being a winner-on-opponent's-turn in
  reverse: the winner always makes the completing move (§3).
* **Suicidal moves are impossible.** Example: your start area is the
  opponent's goal. If the opponent has a piece sitting in your start
  area and you move a piece back home filling the last empty cell, the
  resulting state would have the opponent's win condition satisfied with
  the opponent to move — illegal — so that move is simply not available.
  You can never be forced to hand the opponent a win with your own move,
  and equally can never do so voluntarily.

### Part 2 (Def. 3): blocked goals — the "empty tip" rule

Purpose: forbid fortress draws. Because opponent pieces *count toward
filling* a goal (§3), the only way to freeze the game is to make an
**empty** goal cell permanently unenterable. Definition 3 declares such
positions illegal:

> A state is illegal if one or more unoccupied cells of a player's goal
> area are unreachable by that player due to the other player's pieces.

For 6-piece games this reduces to a single pattern per goal. Labeling a
goal triangle's cells (bottom goal shown; the top goal is the mirror):

```
   c d e      c = 30  d = 31  e = 32
    a b       a = 33  b = 34
     t        t = 35  (the board corner)
```

**A goal is blocked iff the tip `t` is empty and the four edge cells
`a, b, c, e` are all occupied by the goal owner's opponent.** A state in
which either goal is blocked is illegal, regardless of whose turn it is.

Why exactly these cells: the tip's only step entries are from `a` and
`b`; its only hop entries are the straight lines `c→a→t` and `e→b→t`,
launched from `c` or `e`. With all four held by the opponent (who may
keep them there forever — they are in the opponent's own start area),
`t` can never be entered by anyone, the goal can never be filled, and
the game could never end.

Corner cases of part 2, as implemented:

* The blockers must be **opponent** pieces. If any of the four cells
  holds one of the goal owner's own pieces, the state is legal (the
  owner can move their own piece aside).
* The middle cell `d` is irrelevant — occupied by anyone or empty.
* An occupied tip (by either color) means no block: the pattern requires
  the tip *empty*.
* **Completeness (proved, not assumed):** with only 6 opponent pieces,
  the tip is the only goal cell that can be made permanently
  unreachable. Every other goal cell has at least six entry routes
  (neighbors plus hop lines) requiring ≥ 8 blockers to seal. So this
  pattern is the *entire* Definition 3 for 6-piece games.
* **Precedence:** a position that satisfies both a blocked-goal pattern
  and a terminal win condition (possible: the opponent blocks your goal
  with four pieces while their own goal is filled) is classified
  **illegal**, not terminal. This precedence is confirmed by the
  published 4×4 illegal count, which this implementation reproduces
  exactly (405,420).
* Like all illegal states, blocked-goal states are unreachable: neither
  the blocker (placing the fourth edge piece) nor anyone vacating the
  tip (leaving the sealed pattern behind) is allowed to create one.

### Part-2 count note

Static counts under this rule: 4×4 = 405,420 (matches the paper
exactly); 5×5 = 64,173,018 and 6×6 = 1,979,450,250 (the paper reports
63,860,706 and 1,898,692,650). The published 5×5/6×6 counts are
inconsistent with *every* rule of this structural family (they are all
combinatorially count-equivalent), so the paper's own implementation
deviated from its stated rule in an unpublished detail; this solution
follows the published rule. See README.md, "Known discrepancy".

## 5. Draws (Def. 4)

> A game is drawn if any board state is repeated during play.

Chinese Checkers is cyclic (pieces move freely in all directions), so
repetition must be handled. Game-theoretically, a single-repetition rule
makes the value of a position: *win* if the mover can force a win (a
forced win never needs to repeat), *loss* if all moves lose, and *draw*
otherwise — exactly the least fixpoint of the win/loss recurrences. The
solver computes that fixpoint; every state not proven win or loss is a
draw. This matches the paper's methodology ("states that are still
unable to be proven … are drawn") and reproduces the published draw
counts exactly on the games without the part-2 rule (7×7 with 3 and 4
pieces), as well as the paper's concrete drawn example (Fig. 5a).

Draw mechanics in practice are zugzwang standoffs: e.g. both players
shuffle pieces already in their goals because whoever commits their
mid-board piece first loses the race (Fig. 5a), or one player
perpetually re-blocks a double-hop lane (Fig. 5b/c).

## 6. Stuck players (normal-play convention)

If the player to move has **no legal move** — either no geometric moves
at all, or every move would create an illegal state — that player
**loses** (the normal-play convention, per the 2017 Sturtevant–Saffidine
formalization). This corner case is provided for in the solver but
**never occurs**: in all solved games including 6×6, every non-terminal
legal position has at least one legal move (counted during solving:
zero stuck states).

## 7. Symmetries (used for storage/compute, invisible to rules)

* **Color/turn symmetry:** rotating the board 180° (cell id → 35 − id)
  and swapping colors and the turn maps every position to an equivalent
  one with the same value from the mover's perspective. Only
  player-1-to-move positions are stored (factor 2).
* **Left-right mirror:** reflecting (x, y) → (y, x) preserves values.
  Placement blocks whose mirror ranks lower are not stored; blocks that
  are their own mirror keep both images of the opponent placement
  (≈ 0.2% redundancy). Combined reduction: factor 3.996 — the paper's
  1.998 × 2.
* All reported counts are for the **full** state space (both sides to
  move, no symmetry), matching the paper's conventions.

## 8. State space and solution numbers

```
Positions (full space)      2 × C(36,6) × C(30,6) = 2,313,100,389,600
Stored canonical states     578,946,590,400  (2 bits each = 144.7 GB)

                       this solve            paper (Table 1)
Wins  (player 1 wins)  1,152,969,765,114     1,153,000,938,173   (−0.0027%)
Losses (player 2 wins) 1,152,969,765,114     (= wins, by symmetry)
Draws                      5,181,409,122         5,199,820,604   (−0.35%)
Illegal                    1,979,450,250         1,898,692,650   (+4.25%)
```

Wins = positions won by the first player under optimal play (regardless
of whose turn it is); wins = losses exactly, by color symmetry. The four
classes partition the position count exactly. The deltas vs. the paper
are confined to the part-2 rule's unpublished implementation detail
(§4); all four published games without that rule reproduce
digit-for-digit, and the paper itself marks its 6×6 row as never
verification-solved.

**The initial position is a first-player win.** About 99.6% of legal
positions are decided; 0.22% are draws; no position is stuck.

## 9. How it was solved (summary)

Cyclic retrograde value iteration over the canonical state space, fully
in RAM: (1) static pass classifying illegal and terminal states; (2)
fixpoint passes — a state becomes a win when some legal successor is a
proven loss, a loss when all legal successors are proven wins, with
immediate back-propagation of wins to predecessors whenever a loss is
proven (the move relation is symmetric, so predecessors are generated by
the same move generator applied to the opponent's pieces); (3) unproven
states at the fixpoint are draws. Passes iterate opponent-placement
major so that all successor lookups of a group land in one L2-resident
block. 24 passes, 19.8 h on 64 threads, hourly checkpoints,
kill/resume-safe. Verification: an independent brute-force solver agrees
on every state of two complete smaller games; 100,000 sampled 6×6 states
pass local win/loss/draw consistency; four published games reproduce
exactly.

## 10. Querying the solution

```
./src/query runs/m6p6 --m 6 --p 6 --initial
./src/query runs/m6p6 --m 6 --p 6 --p1 3,4,5,17,33,35 --p2 0,1,2,15,30,31 --stm 1
./src/query runs/m6p6 --m 6 --p 6 --stats
./src/query runs/m6p6 --m 6 --p 6 --verify 100000
```

Cell ids per the diagram in §1; `--p1` is the top player (moving down),
`--stm` the side to move. Values returned: `win` / `loss` (for the side
to move), `draw/unknown`, or `illegal`.
