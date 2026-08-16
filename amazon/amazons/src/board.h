/*
  amazons — board representation and move generation for the Game of the
  Amazons on small rectangular boards (W*H <= 64 squares).

  A move is a queen slide followed by an arrow shot from the queen's new
  square; the arrow's landing square is burnt off permanently.  A player
  with no legal move loses.  Every move burns exactly one square, so the
  game graph is a DAG (no repetitions, no graph-history interaction).
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace amazons {

using Bitboard = uint64_t;

enum Color : uint8_t { kWhite = 0, kBlack = 1 };

inline Color Opp(Color c) { return c == kWhite ? kBlack : kWhite; }

// A full move: queen from -> to, then arrow shot from `to` landing on
// `arrow` (which burns that square).
struct Move {
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t arrow = 0;

  bool operator==(const Move& o) const {
    return from == o.from && to == o.to && arrow == o.arrow;
  }
};

class Position {
 public:
  // Board geometry. Set once before use; w*h must be <= 64.
  int w = 0;
  int h = 0;

  Bitboard queens[2] = {0, 0};  // indexed by Color
  Bitboard burned = 0;
  Color stm = kWhite;  // side to move; White moves first (paper convention)

  int NumSquares() const { return w * h; }
  Bitboard Occupied() const { return queens[kWhite] | queens[kBlack] | burned; }
  Bitboard Empty() const { return ~Occupied() & BoardMask(); }
  Bitboard BoardMask() const {
    const int n = w * h;
    return n >= 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1);
  }

  // Standard W×H starting position with 4 amazons each: the small-board
  // setup of the literature (Song & Müller, "on the (1,2)-points in the
  // corner"; confirmed by Fig. 1 of paper.pdf for 5x6). Requires w,h >= 4.
  static Position Standard(int w, int h);

  // Tiny variant: one amazon each. White at (wx, wy), Black at (bx, by),
  // 0-based (x = file, y = rank).
  static Position OneQueen(int w, int h, int wx, int wy, int bx, int by);

  // Move generation: appends all legal moves of the side to move.
  void GenerateMoves(std::vector<Move>* out) const;

  // Applies a legal move in place.
  void DoMove(Move m);

  // True if the side to move has at least one legal move (cheap early-out
  // version of GenerateMoves().empty()).
  bool HasLegalMove() const;

  // Zobrist hash of the exact (non-canonicalized) position, including stm.
  uint64_t Hash() const;

  // Canonical form under the board's symmetry group (D4 for square boards,
  // else the 4 rectangle symmetries), with colors optionally swapped so that
  // stm is always kWhite in the returned position. Swapping colors is value
  // preserving because the game is symmetric between the players.
  // The solver keys its tables by Hash() of the canonical position.
  Position Canonical() const;
  uint64_t CanonicalHash() const { return Canonical().Hash(); }

  // Coordinate string like "B1-B4xD4" (paper notation).
  std::string MoveString(Move m) const;
  // Compact board diagram, rank h down to 1.
  std::string ToString() const;

  bool operator==(const Position& o) const {
    return w == o.w && h == o.h && queens[0] == o.queens[0] &&
           queens[1] == o.queens[1] && burned == o.burned && stm == o.stm;
  }
};

// Zobrist table access (deterministic, fixed seed). state: 0=white queen,
// 1=black queen, 2=burned.
uint64_t Zobrist(int sq, int state);
uint64_t ZobristStm();  // xor in when side to move is kBlack

}  // namespace amazons
