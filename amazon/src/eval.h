/*
  amazons — lightweight sound static bounds, a simplified version of the
  area analysis in Song & Müller, "An Enhanced Solver for the Game of
  Amazons" (paper.pdf, sections II and IV).

  The board is decomposed into 8-connected areas of empty + queen squares.
  Because queens and arrows both travel along contiguous rays and burned
  squares never recover, an area containing queens of only one color can
  never be entered by the opponent (any ray into the area would have to
  pass through a square 8-adjacent to the area, which would itself belong
  to the area).  Such an area is a "simple territory" whose moves belong
  to its owner.

  Sound bounds derived here:
    w_lo = sum of plodding lower bounds over White territories (White can
           guarantee these moves no matter what Black does; likewise b_lo),
    E    = total number of empty squares.
  Every move burns exactly one empty square, so total moves <= E, giving
  the sound upper bounds  w_hi = E - b_lo,  b_hi = E - w_lo.
  With alternating play and no passes, for first player F and second S:
    F certainly wins iff f_lo >  s_hi   (i.e. 2*f_lo > E)
    S certainly wins iff s_lo >= f_hi   (i.e. 2*s_lo >= E)
  (Equal guaranteed move counts favor the second player: the first player
  runs out of moves first.)

  The plodding heuristic (paper section IV-A1): a queen repeatedly steps
  to an adjacent empty square of its territory and shoots back to its
  origin, burning it; each such move consumes exactly one territory square
  and is always legal, so the count is a valid lower bound.
*/
#pragma once

#include "board.h"

namespace amazons {

struct EvalResult {
  bool decided = false;    // verdict is certain
  Color winner = kWhite;   // valid iff decided
  int white_lo = 0;        // guaranteed remaining White moves
  int black_lo = 0;        // guaranteed remaining Black moves
  int empties = 0;

  // Margin in favor of the side to move (guaranteed moves minus the
  // opponent's sound upper bound); positive = side to move looks ahead.
  int Margin(const Position& pos) const;
};

EvalResult EvaluateBounds(const Position& pos);

}  // namespace amazons
