#include "eval.h"

namespace amazons {

namespace {

constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

// Plodding lower bound for one territory: starting from each owner queen
// in turn, greedily step to an adjacent empty territory square and burn
// the origin (shooting back is always legal). Returns the move count.
int PloddingMoves(Bitboard queens, Bitboard area_empty, int w, int h) {
  int moves = 0;
  Bitboard empty = area_empty;
  Bitboard qs = queens;
  while (qs) {
    int cur = __builtin_ctzll(qs);
    qs &= qs - 1;
    while (true) {
      const int x = cur % w, y = cur / w;
      int next = -1;
      for (int d = 0; d < 8; d++) {
        const int nx = x + kDx[d], ny = y + kDy[d];
        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
        const int nsq = ny * w + nx;
        if (empty & (uint64_t{1} << nsq)) {
          next = nsq;
          break;
        }
      }
      if (next < 0) break;
      // The arrow burns the origin and the queen occupies `next`, so `next`
      // leaves the empty set.  (The origin needs no removal: it is either
      // the queen's start square or a destination removed earlier.)
      empty &= ~(uint64_t{1} << next);
      cur = next;
      moves++;
    }
  }
  return moves;
}

}  // namespace

int EvalResult::Margin(const Position& pos) const {
  const int own_lo = pos.stm == kWhite ? white_lo : black_lo;
  const int opp_lo = pos.stm == kWhite ? black_lo : white_lo;
  return own_lo - opp_lo;
}

EvalResult EvaluateBounds(const Position& pos) {
  EvalResult r;
  const int w = pos.w, h = pos.h;
  const Bitboard empty_all = pos.Empty();
  r.empties = __builtin_popcountll(empty_all);

  // Area decomposition: 8-connected components of empty + queen squares.
  Bitboard unseen = empty_all | pos.queens[kWhite] | pos.queens[kBlack];
  Bitboard visited_queens[2] = {0, 0};
  while (unseen) {
    const int seed = __builtin_ctzll(unseen);
    Bitboard area = 0;
    Bitboard frontier = uint64_t{1} << seed;
    unseen &= ~frontier;
    while (frontier) {
      area |= frontier;
      Bitboard next = 0;
      Bitboard f = frontier;
      while (f) {
        const int sq = __builtin_ctzll(f);
        f &= f - 1;
        const int x = sq % w, y = sq / w;
        for (int d = 0; d < 8; d++) {
          const int nx = x + kDx[d], ny = y + kDy[d];
          if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
          const Bitboard b = uint64_t{1} << (ny * w + nx);
          if ((unseen & b) && !(area & b)) {
            next |= b;
            unseen &= ~b;
          }
        }
      }
      frontier = next;
    }
    const Bitboard wq = area & pos.queens[kWhite];
    const Bitboard bq = area & pos.queens[kBlack];
    const Bitboard ae = area & empty_all;
    visited_queens[kWhite] |= wq;
    visited_queens[kBlack] |= bq;
    if (wq && !bq) {
      r.white_lo += PloddingMoves(wq, ae, w, h);
    } else if (bq && !wq) {
      r.black_lo += PloddingMoves(bq, ae, w, h);
    }
    // Areas with queens of both colors (active) or with no queens at all
    // (dead) contribute no guaranteed moves.
  }

  // F certainly wins iff 2*f_lo > E; S certainly wins iff 2*s_lo >= E.
  if (pos.stm == kWhite) {
    if (2 * r.white_lo > r.empties) {
      r.decided = true;
      r.winner = kWhite;
    } else if (2 * r.black_lo >= r.empties) {
      r.decided = true;
      r.winner = kBlack;
    }
  } else {
    if (2 * r.black_lo > r.empties) {
      r.decided = true;
      r.winner = kBlack;
    } else if (2 * r.white_lo >= r.empties) {
      r.decided = true;
      r.winner = kWhite;
    }
  }
  return r;
}

}  // namespace amazons
