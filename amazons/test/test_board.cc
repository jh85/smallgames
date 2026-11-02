// amazons — board / movegen / hashing / canonicalization tests.
#include <cstdio>
#include <vector>

#include "board.h"

using namespace amazons;

#define CHECK(cond)                                     \
  do {                                                  \
    if (!(cond)) {                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, \
                   __LINE__, #cond);                    \
      return 1;                                         \
    }                                                   \
  } while (0)

static int TestPlacements() {
  const Position p44 = Position::Standard(4, 4);
  // White: a2 b1 c1 d2 ; Black: a3 b4 c4 d3 ((1,2)-points of the corners).
  const uint64_t w44 = (1ull << 4) | (1ull << 1) | (1ull << 2) | (1ull << 7);
  const uint64_t b44 = (1ull << 8) | (1ull << 13) | (1ull << 14) | (1ull << 11);
  CHECK(p44.queens[kWhite] == w44);
  CHECK(p44.queens[kBlack] == b44);
  CHECK(p44.burned == 0 && p44.stm == kWhite);

  // Paper Fig. 1 (5x6): White A2 B1 D1 E2, Black A5 B6 D6 E5.
  const Position p56 = Position::Standard(5, 6);
  const uint64_t w56 =
      (1ull << 5) | (1ull << 1) | (1ull << 3) | (1ull << 9);
  const uint64_t b56 =
      (1ull << 20) | (1ull << 26) | (1ull << 28) | (1ull << 24);
  CHECK(p56.queens[kWhite] == w56);
  CHECK(p56.queens[kBlack] == b56);
  return 0;
}

static int TestMovegen() {
  // 2x2, W a1, B b2: exactly 4 legal moves (hand-computed).
  const Position p = Position::OneQueen(2, 2, 0, 0, 1, 1);
  std::vector<Move> moves;
  p.GenerateMoves(&moves);
  CHECK(moves.size() == 4);
  CHECK(p.HasLegalMove());

  // Every move is applicable and flips the side to move.
  for (Move m : moves) {
    Position c = p;
    c.DoMove(m);
    CHECK(c.stm == kBlack);
    CHECK(c.burned == (1ull << m.arrow));
    CHECK(c.queens[kWhite] == (1ull << m.to));
  }

  // Terminal: 2x2 with only b1 empty after W a1->b1 x a1 ... construct
  // directly: W a1, B b2, a2 and b1 burned -> White has no move.
  Position t;
  t.w = t.h = 2;
  t.queens[kWhite] = 1ull << 0;  // a1
  t.queens[kBlack] = 1ull << 3;  // b2
  t.burned = (1ull << 1) | (1ull << 2);
  t.stm = kWhite;
  std::vector<Move> tm;
  t.GenerateMoves(&tm);
  CHECK(tm.empty());
  CHECK(!t.HasLegalMove());
  return 0;
}

static int TestRandomPlayoutConsistency() {
  // HasLegalMove must agree with GenerateMoves along random playouts, and
  // Hash() must be recomputed consistently.
  uint64_t seed = 12345;
  auto rnd = [&seed]() {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return seed >> 33;
  };
  for (int game = 0; game < 200; game++) {
    Position p = Position::Standard(4, 4);
    std::vector<Move> moves;
    while (true) {
      p.GenerateMoves(&moves);
      CHECK(p.HasLegalMove() == !moves.empty());
      CHECK(p.Hash() == p.Hash());
      if (moves.empty()) break;
      p.DoMove(moves[rnd() % moves.size()]);
    }
  }
  return 0;
}

static int TestCanonical() {
  // A position and its horizontal mirror must canonicalize identically.
  const Position p = Position::OneQueen(3, 3, 0, 0, 2, 1);
  Position q;
  q.w = q.h = 3;
  q.queens[kWhite] = 1ull << 2;  // c1 (mirror of a1)
  q.queens[kBlack] = 1ull << 3;  // a2 (mirror of c2)
  q.stm = kWhite;
  CHECK(p.CanonicalHash() == q.CanonicalHash());

  // Color swap with stm flip must canonicalize identically.
  Position r = p;
  r.queens[kWhite] = p.queens[kBlack];
  r.queens[kBlack] = p.queens[kWhite];
  r.stm = kBlack;
  CHECK(p.CanonicalHash() == r.CanonicalHash());

  // Distinct positions must (with overwhelming probability) differ.
  const Position s = Position::OneQueen(3, 3, 0, 0, 2, 2);
  CHECK(p.CanonicalHash() != s.CanonicalHash());

  // Canonical form is stable along playouts: mirroring the whole board
  // state must not change the canonical hash.
  Position a = Position::Standard(4, 4);
  std::vector<Move> moves;
  a.GenerateMoves(&moves);
  a.DoMove(moves.front());
  Position b;
  b.w = b.h = 4;
  for (int sq = 0; sq < 16; sq++) {
    const int x = sq % 4, y = sq / 4;
    const int msq = y * 4 + (3 - x);  // horizontal mirror
    const uint64_t bit = 1ull << sq, mbit = 1ull << msq;
    if (a.queens[kWhite] & bit) b.queens[kWhite] |= mbit;
    if (a.queens[kBlack] & bit) b.queens[kBlack] |= mbit;
    if (a.burned & bit) b.burned |= mbit;
  }
  b.stm = a.stm;
  CHECK(a.CanonicalHash() == b.CanonicalHash());
  return 0;
}

int main() {
  if (int r = TestPlacements()) return r;
  if (int r = TestMovegen()) return r;
  if (int r = TestRandomPlayoutConsistency()) return r;
  if (int r = TestCanonical()) return r;
  std::printf("test_board OK\n");
  return 0;
}
