// Oware 6x2 rules on mover-normalised boards, and the perfect-hash position index.
//
// A Board always has the side to move owning pits 0..5 and the opponent owning
// pits 6..11; sowing advances (i+1) mod 12.  After a move the board is rotated by
// six pits so the new side to move again owns 0..5.  This is the P0/P1 game of
// oware_6x2_paper_rules.md seen from the mover's side (the rules are symmetric
// under that rotation).
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace oware {

constexpr int PITS = 12, ROW = 6, SEEDS = 48;

struct Board {
  uint8_t p[PITS];
  int rowSum(int first) const {
    int s = 0;
    for (int j = first; j < first + ROW; ++j) s += p[j];
    return s;
  }
  int total() const { return rowSum(0) + rowSum(ROW); }
};

// Legal pit choices for the mover (rules section 3).  Returns a 6-bit mask.
inline int legalMask(const Board& b) {
  int m = 0;
  if (b.rowSum(ROW) > 0) {
    for (int i = 0; i < ROW; ++i)
      if (b.p[i]) m |= 1 << i;
  } else {
    for (int i = 0; i < ROW; ++i)
      if (b.p[i] > 5 - i) m |= 1 << i;  // must reach the opponent's row
  }
  return m;
}

// Play pit i (must be legal).  Writes the successor, already rotated so that the
// new side to move owns pits 0..5, and returns the number of seeds captured
// (0 when nothing is captured or the capture is a cancelled grand slam).
inline int play(const Board& b, int i, Board& out) {
  uint8_t t[PITS];
  std::memcpy(t, b.p, PITS);
  int s = t[i];
  t[i] = 0;
  int pos = i;
  if (s >= 11) {  // full laps: every other pit gets one seed per lap
    int laps = s / 11;
    for (int j = 0; j < PITS; ++j)
      if (j != i) t[j] += laps;
    s -= laps * 11;
    if (!s) pos = i == 0 ? PITS - 1 : i - 1;  // a whole number of laps ends just before i
  }
  while (s) {
    pos = pos == PITS - 1 ? 0 : pos + 1;
    if (pos == i) continue;
    ++t[pos];
    --s;
  }
  int cap = 0;
  if (pos >= ROW && (t[pos] == 2 || t[pos] == 3)) {
    int j = pos;
    while (j >= ROW && (t[j] == 2 || t[j] == 3)) cap += t[j--];
    int opp = 0;
    for (int q = ROW; q < PITS; ++q) opp += t[q];
    if (cap == opp) {
      cap = 0;  // grand slam: sowing stands, whole capture cancelled
    } else {
      for (int q = j + 1; q <= pos; ++q) t[q] = 0;
    }
  }
  std::memcpy(out.p, t + ROW, ROW);
  std::memcpy(out.p + ROW, t, ROW);
  return cap;
}

// ---------------------------------------------------------------------------
// Position index.
//
// The indexed set for n seeds on the board is every board whose opponent row
// (pits 6..11) contains at least one empty pit.  The player who just moved
// emptied the pit they sowed from and sowing skips that pit, so every position
// that arises after a move is in the set; the set is closed under play().  The
// only reachable position outside it is the initial one.  Summed over n = 0..46
// and 48 this gives 889,063,398,405 positions (+1 for the initial position =
// the 889,063,398,406 of Romein & Bal 2003).
//
// The index is a minimal perfect hash obtained exactly as in Takeda & Hoki's
// first ZDD: a layered decision diagram over the pits whose node label is
// (pit, seeds still to place, "an empty opponent pit was seen"); every node
// stores its number of accepting paths and the rank of a path is the sum of
// the counts of the branches it skipped.  Because the opponent row and the
// mover row only interact through their sums, the diagram factors into two
// six-pit diagrams and the rank is
//     base[n][k] + rankZ(opp row, k) * count(n-k) + rank(mover row, n-k)
// where k is the opponent row sum.
// ---------------------------------------------------------------------------
class Index {
 public:
  Index();
  uint64_t layerSize(int n) const { return base_[n][n + 1]; }
  uint64_t rank(const Board& b, int n) const {
    int k = 0, r;
    uint32_t ro = 0, rm = 0;
    for (int j = ROW; j < PITS; ++j) k += b.p[j];
    r = k;
    int f = 0;
    for (int j = 0; j < ROW; ++j) {  // opponent row, zero required
      int a = b.p[ROW + j];
      ro += preZ_[j][r][f][a];
      f |= (a == 0);
      r -= a;
    }
    r = n - k;
    for (int j = 0; j < ROW; ++j) {
      int a = b.p[j];
      rm += preA_[j][r][a];
      r -= a;
    }
    return base_[n][k] + uint64_t(ro) * cntA_[n - k] + rm;
  }
  // Decompose an index into (opponent row sum, opponent row rank, mover row rank).
  void split(uint64_t idx, int n, int& k, uint32_t& ro, uint32_t& rm) const {
    int lo = 0, hi = n;
    while (lo < hi) {
      int mid = (lo + hi + 1) >> 1;
      if (base_[n][mid] <= idx) lo = mid; else hi = mid - 1;
    }
    k = lo;
    idx -= base_[n][k];
    ro = uint32_t(idx / cntA_[n - k]);
    rm = uint32_t(idx % cntA_[n - k]);
  }
  // Row tables: every composition in rank order, 6 bytes packed in a uint64.
  const uint64_t* rowsA(int k) const { return &rowA_[offA_[k]]; }
  const uint64_t* rowsZ(int k) const { return &rowZ_[offZ_[k]]; }
  uint32_t cntA(int k) const { return cntA_[k]; }
  uint32_t cntZ(int k) const { return cntZ_[k]; }
  Board unrank(uint64_t idx, int n) const {
    int k; uint32_t ro, rm;
    split(idx, n, k, ro, rm);
    Board b;
    uint64_t o = rowsZ(k)[ro], m = rowsA(n - k)[rm];
    std::memcpy(b.p, &m, ROW);
    std::memcpy(b.p + ROW, &o, ROW);
    return b;
  }

 private:
  uint32_t cntA_[SEEDS + 1], cntZ_[SEEDS + 1];
  uint64_t base_[SEEDS + 1][SEEDS + 2];
  uint32_t preA_[ROW][SEEDS + 1][SEEDS + 2];
  uint32_t preZ_[ROW][SEEDS + 1][2][SEEDS + 2];
  std::vector<uint64_t> rowA_, rowZ_;
  size_t offA_[SEEDS + 2], offZ_[SEEDS + 2];
};

inline Index::Index() {
  // paths[j][r][f]: accepting paths of the six-pit diagram from node (pit j,
  // r seeds left, zero-seen flag f).  Accept at j==6 iff r==0 (and f for Z).
  static uint32_t pa[ROW + 1][SEEDS + 1], pz[ROW + 1][SEEDS + 1][2];
  for (int r = 0; r <= SEEDS; ++r) {
    pa[ROW][r] = r == 0;
    pz[ROW][r][0] = 0;
    pz[ROW][r][1] = r == 0;
  }
  for (int j = ROW - 1; j >= 0; --j)
    for (int r = 0; r <= SEEDS; ++r) {
      pa[j][r] = 0;
      for (int a = 0; a <= r; ++a) pa[j][r] += pa[j + 1][r - a];
      for (int f = 0; f < 2; ++f) {
        pz[j][r][f] = 0;
        for (int a = 0; a <= r; ++a) pz[j][r][f] += pz[j + 1][r - a][f | (a == 0)];
      }
    }
  for (int j = 0; j < ROW; ++j)
    for (int r = 0; r <= SEEDS; ++r) {
      preA_[j][r][0] = 0;
      for (int a = 0; a <= r; ++a) preA_[j][r][a + 1] = preA_[j][r][a] + pa[j + 1][r - a];
      for (int f = 0; f < 2; ++f) {
        preZ_[j][r][f][0] = 0;
        for (int a = 0; a <= r; ++a)
          preZ_[j][r][f][a + 1] = preZ_[j][r][f][a] + pz[j + 1][r - a][f | (a == 0)];
      }
    }
  for (int k = 0; k <= SEEDS; ++k) {
    cntA_[k] = pa[0][k];
    cntZ_[k] = pz[0][k][0];
  }
  for (int n = 0; n <= SEEDS; ++n) {
    base_[n][0] = 0;
    for (int k = 0; k <= n; ++k)
      base_[n][k + 1] = base_[n][k] + uint64_t(cntZ_[k]) * cntA_[n - k];
  }
  // Enumerate compositions in lexicographic (= rank) order.
  size_t ta = 0, tz = 0;
  for (int k = 0; k <= SEEDS; ++k) {
    offA_[k] = ta; offZ_[k] = tz;
    ta += cntA_[k]; tz += cntZ_[k];
  }
  offA_[SEEDS + 1] = ta; offZ_[SEEDS + 1] = tz;
  rowA_.resize(ta);
  rowZ_.resize(tz);
  for (int k = 0; k <= SEEDS; ++k) {
    size_t ia = offA_[k], iz = offZ_[k];
    uint8_t c[ROW];
    for (c[0] = 0; c[0] <= k; ++c[0])
     for (c[1] = 0; c[0] + c[1] <= k; ++c[1])
      for (c[2] = 0; c[0] + c[1] + c[2] <= k; ++c[2])
       for (c[3] = 0; c[0] + c[1] + c[2] + c[3] <= k; ++c[3])
        for (c[4] = 0; c[0] + c[1] + c[2] + c[3] + c[4] <= k; ++c[4]) {
          c[5] = uint8_t(k - c[0] - c[1] - c[2] - c[3] - c[4]);
          uint64_t v = 0;
          std::memcpy(&v, c, ROW);
          rowA_[ia++] = v;
          if (!(c[0] && c[1] && c[2] && c[3] && c[4] && c[5])) rowZ_[iz++] = v;
        }
  }
}

}  // namespace oware
