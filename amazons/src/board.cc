#include "board.h"

#include <algorithm>
#include <cstdio>

namespace amazons {

namespace {

// Deterministic splitmix64 stream for Zobrist keys.
struct ZobristTable {
  uint64_t sq[64][3];  // 0=white queen, 1=black queen, 2=burned
  uint64_t stm;        // xor when side to move is kBlack

  ZobristTable() {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    auto next = [&s]() {
      uint64_t z = (s += 0x9E3779B97F4A7C15ull);
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      return z ^ (z >> 31);
    };
    for (int i = 0; i < 64; i++)
      for (int j = 0; j < 3; j++) sq[i][j] = next();
    stm = next();
  }
};

const ZobristTable& Table() {
  static const ZobristTable t;
  return t;
}

constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

}  // namespace

uint64_t Zobrist(int sq, int state) { return Table().sq[sq][state]; }
uint64_t ZobristStm() { return Table().stm; }

Position Position::Standard(int w, int h) {
  Position p;
  p.w = w;
  p.h = h;
  auto sq = [&](int f, int r) { return (r - 1) * w + (f - 1); };
  // The small-board standard setup of the literature (Song & Müller,
  // "starting on the (1,2)-points in the corner"): each player's amazons
  // sit one step away from each home corner along both edges.
  // White (1-indexed): (1,2) (2,1) (w-1,1) (w,2); Black mirrored.
  p.queens[kWhite] |= (uint64_t{1} << sq(1, 2)) | (uint64_t{1} << sq(2, 1)) |
                      (uint64_t{1} << sq(w - 1, 1)) | (uint64_t{1} << sq(w, 2));
  p.queens[kBlack] |= (uint64_t{1} << sq(1, h - 1)) | (uint64_t{1} << sq(2, h)) |
                      (uint64_t{1} << sq(w - 1, h)) |
                      (uint64_t{1} << sq(w, h - 1));
  p.stm = kWhite;
  return p;
}

Position Position::OneQueen(int w, int h, int wx, int wy, int bx, int by) {
  Position p;
  p.w = w;
  p.h = h;
  p.queens[kWhite] = uint64_t{1} << (wy * w + wx);
  p.queens[kBlack] = uint64_t{1} << (by * w + bx);
  p.stm = kWhite;
  return p;
}

void Position::GenerateMoves(std::vector<Move>* out) const {
  out->clear();
  const Bitboard occ = Occupied();
  const Bitboard mask = BoardMask();
  Bitboard mine = queens[stm];
  while (mine) {
    const int from = __builtin_ctzll(mine);
    mine &= mine - 1;
    const int fx = from % w, fy = from / w;
    for (int d = 0; d < 8; d++) {
      int x = fx, y = fy;
      while (true) {
        x += kDx[d];
        y += kDy[d];
        if (x < 0 || x >= w || y < 0 || y >= h) break;
        const int to = y * w + x;
        if (occ & (uint64_t{1} << to)) break;
        // Queen at `to`; origin `from` is now empty. Shoot the arrow.
        const Bitboard occ2 =
            (occ & ~(uint64_t{1} << from)) | (uint64_t{1} << to);
        for (int d2 = 0; d2 < 8; d2++) {
          int ax = x, ay = y;
          while (true) {
            ax += kDx[d2];
            ay += kDy[d2];
            if (ax < 0 || ax >= w || ay < 0 || ay >= h) break;
            const int a = ay * w + ax;
            if (occ2 & (uint64_t{1} << a)) break;
            out->push_back(Move{static_cast<uint8_t>(from),
                                static_cast<uint8_t>(to),
                                static_cast<uint8_t>(a)});
          }
        }
      }
    }
  }
  (void)mask;
}

void Position::DoMove(Move m) {
  const Bitboard from_bb = uint64_t{1} << m.from;
  queens[stm] = (queens[stm] & ~from_bb) | (uint64_t{1} << m.to);
  burned |= uint64_t{1} << m.arrow;
  stm = Opp(stm);
}

bool Position::HasLegalMove() const {
  const Bitboard occ = Occupied();
  Bitboard mine = queens[stm];
  while (mine) {
    const int from = __builtin_ctzll(mine);
    mine &= mine - 1;
    const int fx = from % w, fy = from / w;
    for (int d = 0; d < 8; d++) {
      int x = fx + kDx[d], y = fy + kDy[d];
      if (x < 0 || x >= w || y < 0 || y >= h) continue;
      const int to = y * w + x;
      if (occ & (uint64_t{1} << to)) continue;
      // Can an arrow be shot from `to`? Any ray step landing on an empty
      // square, where `from` now counts as empty.
      const Bitboard occ2 = occ & ~(uint64_t{1} << from);
      for (int d2 = 0; d2 < 8; d2++) {
        const int ax = x + kDx[d2], ay = y + kDy[d2];
        if (ax < 0 || ax >= w || ay < 0 || ay >= h) continue;
        if (!(occ2 & (uint64_t{1} << (ay * w + ax)))) return true;
      }
    }
  }
  return false;
}

uint64_t Position::Hash() const {
  uint64_t h = 0;
  Bitboard b = queens[kWhite];
  while (b) {
    h ^= Zobrist(__builtin_ctzll(b), 0);
    b &= b - 1;
  }
  b = queens[kBlack];
  while (b) {
    h ^= Zobrist(__builtin_ctzll(b), 1);
    b &= b - 1;
  }
  b = burned;
  while (b) {
    h ^= Zobrist(__builtin_ctzll(b), 2);
    b &= b - 1;
  }
  if (stm == kBlack) h ^= ZobristStm();
  return h;
}

Position Position::Canonical() const {
  const bool square = (w == h);
  const int n_sym = square ? 8 : 4;
  Position best;
  bool have_best = false;
  for (int s = 0; s < n_sym; s++) {
    const bool swap_axes = square && (s & 4);
    const bool flip_x = s & 1;
    const bool flip_y = s & 2;
    auto map_sq = [&](int sq) {
      int x = sq % w, y = sq / w;
      if (swap_axes) {
        int t = x;
        x = y;
        y = t;
      }
      if (flip_x) x = w - 1 - x;
      if (flip_y) y = h - 1 - y;
      return y * w + x;
    };
    auto map_bb = [&](Bitboard bb) {
      Bitboard r = 0;
      while (bb) {
        r |= uint64_t{1} << map_sq(__builtin_ctzll(bb));
        bb &= bb - 1;
      }
      return r;
    };
    Position c;
    c.w = w;
    c.h = h;
    c.queens[kWhite] = map_bb(queens[kWhite]);
    c.queens[kBlack] = map_bb(queens[kBlack]);
    c.burned = map_bb(burned);
    c.stm = stm;
    if (c.stm == kBlack) {
      // Normalize to white-to-move by swapping colors (value preserving).
      Bitboard t = c.queens[kWhite];
      c.queens[kWhite] = c.queens[kBlack];
      c.queens[kBlack] = t;
      c.stm = kWhite;
    }
    if (!have_best || c.queens[kWhite] < best.queens[kWhite] ||
        (c.queens[kWhite] == best.queens[kWhite] &&
         (c.queens[kBlack] < best.queens[kBlack] ||
          (c.queens[kBlack] == best.queens[kBlack] &&
           c.burned < best.burned)))) {
      best = c;
      have_best = true;
    }
  }
  return best;
}

std::string Position::MoveString(Move m) const {
  auto name = [&](int sq) {
    char buf[4];
    std::snprintf(buf, sizeof buf, "%c%d", 'A' + sq % w, sq / w + 1);
    return std::string(buf);
  };
  return name(m.from) + "-" + name(m.to) + "x" + name(m.arrow);
}

std::string Position::ToString() const {
  std::string s;
  for (int y = h - 1; y >= 0; y--) {
    for (int x = 0; x < w; x++) {
      const Bitboard b = uint64_t{1} << (y * w + x);
      char c = '.';
      if (queens[kWhite] & b) c = 'W';
      if (queens[kBlack] & b) c = 'B';
      if (burned & b) c = 'x';
      s += c;
    }
    s += '\n';
  }
  s += stm == kWhite ? "White to move\n" : "Black to move\n";
  return s;
}

}  // namespace amazons
