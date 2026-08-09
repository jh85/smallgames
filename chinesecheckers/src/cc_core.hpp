// cc_core.hpp -- Core definitions for the Chinese Checkers strong solver.
//
// Implements the rules of two-player Chinese Checkers on the m x m diamond
// gameplay area exactly as defined in:
//   N. R. Sturtevant, "On Strongly Solving Chinese Checkers", ACG 2019.
//
// See README.md for the full list of rule interpretations / assumptions.
#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cc {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

// State values, 2 bits each. UNKNOWN must be 0 so that zero-initialized
// memory means "not yet proven" (drawn if still unknown at fixpoint).
enum Value : u8 { UNKNOWN = 0, WIN = 1, LOSS = 2, ILLEGAL = 3 };

inline const char* valueName(u8 v) {
  switch (v) {
    case UNKNOWN: return "draw/unknown";
    case WIN:     return "win";
    case LOSS:    return "loss";
    case ILLEGAL: return "illegal";
  }
  return "?";
}

inline u64 bit(int c) { return u64(1) << c; }
inline u64 below(int c) { return (u64(1) << c) - 1; }

// Bit-reverse of a 64-bit word (used for the 180-degree board rotation,
// which maps cell id -> N-1-id).
inline u64 reverse64(u64 x) {
  x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
  x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
  x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
  x = ((x >> 8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) << 8);
  x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
  return (x >> 32) | (x << 32);
}

// ---------------------------------------------------------------------------
// Board geometry.
//
// The m x m gameplay area is the central diamond of the Chinese Checkers
// board; cells are indexed on a skewed grid (x,y), 0 <= x,y < m, with the six
// hex neighbors (x+-1,y), (x,y+-1), (x+1,y-1), (x-1,y+1). Drawn as a diamond,
// row k (k = x+y, 0..2m-2) holds the cells with x+y == k.
//
// Cell ids are assigned row-major (row k ascending, x ascending inside a
// row). Player 1 starts at the top (ids 0..p-1) and moves toward the bottom
// goal (ids N-p..N-1); player 2 is the reverse. The 180-degree rotation maps
// id -> N-1-id; the left-right mirror maps (x,y) -> (y,x).
// ---------------------------------------------------------------------------
struct Game {
  int m = 0;       // board side (gameplay area is m x m)
  int p = 0;       // pieces per player (1..6)
  int N = 0;       // number of cells = m*m
  bool lrValid = false;  // left-right symmetry usable (start area mirror-symmetric)

  std::vector<std::array<int, 2>> coordOf;        // id -> {x,y}
  std::vector<std::array<int, 6>> nbr;            // id,dir -> neighbor id or -1
  std::vector<std::array<int, 6>> hopLand;        // id,dir -> hop landing id or -1
  std::vector<int> mirrorId;                      // id -> mirrored id
  u64 startMask = 0;   // P1 start area == P2 goal area (top)
  u64 goalMask = 0;    // P1 goal area (bottom)
  // Illegal-state part 2 masks (only used when p == 6): a goal is "blocked"
  // if its tip is empty and all four outer-edge cells of the goal triangle
  // are occupied by the *other* player's pieces.
  u64 goalTipB = 0, goalEdgeB = 0;   // bottom goal (P1's goal, blocked by P2)
  u64 goalTipT = 0, goalEdgeT = 0;   // top goal (P2's goal, blocked by P1)

  // Direction deltas for the six hex neighbors.
  static constexpr int DX[6] = {1, -1, 0, 0, 1, -1};
  static constexpr int DY[6] = {0, 0, 1, -1, -1, 1};

  // Start-area shapes. FIRSTK = first p cell ids (row-major). SYMMETRIC =
  // smallest mirror-symmetric corner shape with p cells (differs from
  // FIRSTK only for p in {2,4,5}); matches the symmetric-state reduction
  // reported for 4 pieces in Sturtevant's 2022 bitboard paper.
  enum AreaMode { FIRSTK = 0, SYMMETRIC = 1 };

  Game(int m_, int p_, AreaMode areaMode = FIRSTK) : m(m_), p(p_), N(m_ * m_) {
    assert(m >= 3 && m <= 7 && N <= 49);
    assert(p >= 1 && p <= 6);
    std::vector<std::vector<int>> grid(m, std::vector<int>(m, -1));
    coordOf.resize(N);
    int id = 0;
    for (int k = 0; k <= 2 * (m - 1); k++) {
      for (int x = std::max(0, k - (m - 1)); x <= std::min(k, m - 1); x++) {
        int y = k - x;
        grid[x][y] = id;
        coordOf[id] = {x, y};
        id++;
      }
    }
    assert(id == N);
    nbr.assign(N, {});
    hopLand.assign(N, {});
    mirrorId.assign(N, 0);
    for (int c = 0; c < N; c++) {
      auto [x, y] = coordOf[c];
      mirrorId[c] = grid[y][x];
      for (int d = 0; d < 6; d++) {
        int nx = x + DX[d], ny = y + DY[d];
        nbr[c][d] = (nx >= 0 && nx < m && ny >= 0 && ny < m) ? grid[nx][ny] : -1;
        int lx = x + 2 * DX[d], ly = y + 2 * DY[d];
        hopLand[c][d] = (lx >= 0 && lx < m && ly >= 0 && ly < m) ? grid[lx][ly] : -1;
      }
    }
    if (areaMode == FIRSTK) {
      for (int i = 0; i < p; i++) startMask |= bit(i);
    } else {
      // Mirror-symmetric corner areas: p=2 -> row 1; p=4 -> rhombus
      // (tip, row 1, center of row 2); p=5 -> tip, row 1, ends of row 2.
      // Fill rows from the tip; a partially-filled row takes its center
      // cell first (only p=4 differs from FIRSTK: the rhombus, which is
      // what reproduces the paper's p=4 results and the 2022 paper's
      // symmetric-state counts).
      if (p == 4) startMask = bit(0) | bit(1) | bit(2) | bit(4);
      else for (int i = 0; i < p; i++) startMask |= bit(i);
    }
    goalMask = rotMask(startMask);
    // Rotation must map the start area onto the goal area (it always does,
    // because id ordering reverses under 180-degree rotation).
    assert(rotMask(startMask) == goalMask);
    lrValid = (mirrorMask(startMask) == startMask);

    if (p == 6) {
      goalTipB = bit(grid[m - 1][m - 1]);
      goalEdgeB = bit(grid[m - 2][m - 1]) | bit(grid[m - 1][m - 2]) |
                  bit(grid[m - 3][m - 1]) | bit(grid[m - 1][m - 3]);
      goalTipT = bit(grid[0][0]);
      goalEdgeT = bit(grid[1][0]) | bit(grid[0][1]) | bit(grid[2][0]) | bit(grid[0][2]);
    }
  }

  int idOf(int x, int y) const {
    for (int c = 0; c < N; c++)
      if (coordOf[c][0] == x && coordOf[c][1] == y) return c;
    return -1;
  }

  // 180-degree rotation of a cell mask: bit c -> bit N-1-c.
  u64 rotMask(u64 mask) const { return reverse64(mask) >> (64 - N); }

  u64 mirrorMask(u64 mask) const {
    u64 r = 0;
    while (mask) {
      int c = __builtin_ctzll(mask);
      mask &= mask - 1;
      r |= bit(mirrorId[c]);
    }
    return r;
  }

  // All destinations for the piece on cell `from`, given full occupancy
  // `occ` (which includes `from`). Single steps to empty neighbors plus the
  // closure of hop chains. The origin is vacated for the duration of the
  // move (so later hops in a chain may cross it) but is never a destination.
  u64 pieceDests(int from, u64 occ) const {
    u64 occ2 = occ & ~bit(from);
    u64 dests = 0;
    for (int d = 0; d < 6; d++) {
      int n = nbr[from][d];
      if (n >= 0 && !(occ2 >> n & 1)) dests |= bit(n);
    }
    // DFS over hop landings; `visited` prevents revisiting (incl. origin).
    u64 visited = bit(from);
    int stack[64];
    int sp = 0;
    stack[sp++] = from;
    while (sp) {
      int c = stack[--sp];
      for (int d = 0; d < 6; d++) {
        int over = nbr[c][d];
        if (over < 0 || !(occ2 >> over & 1)) continue;  // need adjacent piece
        int land = hopLand[c][d];
        if (land < 0 || (occ2 >> land & 1) || (visited >> land & 1)) continue;
        visited |= bit(land);
        dests |= bit(land);
        stack[sp++] = land;
      }
    }
    return dests;  // never contains `from`
  }

  // Static classification of a canonical state (a = P1 pieces = side to
  // move, moving top->bottom; b = P2 pieces).
  //   ILLEGAL part 1 (Def. 2): the mover's own win condition already holds.
  //   ILLEGAL part 2 (Def. 3, 6-piece rule): a goal tip is empty while both
  //     outer edges of that goal triangle are occupied by the opponent.
  //   LOSS: the opponent's win condition holds -> terminal, mover has lost.
  //   Win condition (Def. 1): the goal area is completely filled and at
  //     least one piece in it belongs to the goal's owner.
  u8 classify(u64 a, u64 b) const {
    u64 occ = a | b;
    if ((occ & goalMask) == goalMask && (a & goalMask)) return ILLEGAL;  // part 1
    if (p == 6) {
      if (!(occ & goalTipB) && (b & goalEdgeB) == goalEdgeB) return ILLEGAL;  // part 2
      if (!(occ & goalTipT) && (a & goalEdgeT) == goalEdgeT) return ILLEGAL;  // part 2
    }
    if ((occ & startMask) == startMask && (b & startMask)) return LOSS;  // terminal
    return UNKNOWN;
  }
};

// ---------------------------------------------------------------------------
// Combinatorial (colex) ranking of k-subsets.
// rank(c_0 < c_1 < ... < c_{k-1}) = sum_i C(c_i, i+1); a perfect ranking.
// ---------------------------------------------------------------------------
struct Ranker {
  u64 C[64][8];  // C[n][k], k <= 7

  Ranker() {
    for (int n = 0; n < 64; n++)
      for (int k = 0; k < 8; k++) {
        if (k == 0) C[n][k] = 1;
        else if (n == 0) C[n][k] = 0;
        else C[n][k] = C[n - 1][k - 1] + C[n - 1][k];
      }
  }

  u64 choose(int n, int k) const { return (k < 0 || k > 7 || n < 0) ? 0 : C[n][k]; }

  // Rank of the set bits of `mask` as a k-subset of {0..N-1}.
  u32 rankK(u64 mask) const {
    u64 r = 0;
    int i = 0;
    while (mask) {
      int c = __builtin_ctzll(mask);
      mask &= mask - 1;
      r += C[c][++i];
    }
    return (u32)r;
  }

  // Rank of b's cells as a k-subset of the cells NOT in a.
  u32 rank2(u64 a, u64 b) const {
    u64 r = 0;
    int i = 0;
    while (b) {
      int c = __builtin_ctzll(b);
      b &= b - 1;
      int pos = c - __builtin_popcountll(a & below(c));
      r += C[pos][++i];
    }
    return (u32)r;
  }

  // Inverse of rankK for k-subsets of {0..n-1}; returns the bit mask.
  u64 unrankK(u64 r, int k, int n) const {
    u64 mask = 0;
    for (int i = k; i >= 1; i--) {
      int c = n - 1;
      while (C[c][i] > r) c--;
      r -= C[c][i];
      mask |= bit(c);
      n = c;
    }
    return mask;
  }
};

// ---------------------------------------------------------------------------
// Index scheme over canonical states.
//
// Canonical states always have the side to move as "P1" (moving top to
// bottom): a physical state with P2 to move is first rotated 180 degrees
// with colors swapped (an exact value-preserving isomorphism).
//
// Index = base[rank(a)] + rank2(a, b). When left-right symmetry is valid,
// only blocks whose P1-set rank is <= its mirror's rank are stored; states
// in dropped blocks map through the mirror. Blocks with a self-symmetric
// P1 set store both mirror images of each P2 set (~0.2% redundancy on 6x6,
// giving the paper's ~1.998x reduction rather than exactly 2x).
// ---------------------------------------------------------------------------
struct Index {
  const Game& g;
  Ranker rk;
  bool lr = false;         // left-right reduction active
  u32 C1 = 0, C2 = 0;      // C(N,p), C(N-p,p)
  std::vector<i64> base;   // r1 -> state index of block start, or -1 if unstored
  std::vector<u32> mirR1;  // r1 -> rank of mirrored P1 set
  std::vector<u32> storedBlocks;  // list of stored r1, ascending (base order)
  u64 totalStored = 0;

  explicit Index(const Game& game, bool useLR = true) : g(game) {
    lr = useLR && g.lrValid;
    C1 = (u32)rk.choose(g.N, g.p);
    C2 = (u32)rk.choose(g.N - g.p, g.p);
    base.assign(C1, -1);
    mirR1.assign(C1, 0);
    u64 cum = 0;
    for (u32 r1 = 0; r1 < C1; r1++) {
      u64 s1 = rk.unrankK(r1, g.p, g.N);
      u32 mr1 = rk.rankK(g.mirrorMask(s1));
      mirR1[r1] = mr1;
      if (!lr || mr1 >= r1) {
        base[r1] = (i64)cum;
        cum += C2;
        storedBlocks.push_back(r1);
      }
    }
    totalStored = cum;
  }

  // Multiplicity of each stored state in the full set of canonical states.
  int blockWeight(u32 r1) const { return (lr && mirR1[r1] != r1) ? 2 : 1; }

  // Index of the canonical state (a = mover's pieces, b = opponent's).
  inline i64 indexOf(u64 a, u64 b) const {
    u32 r1 = rk.rankK(a);
    i64 bs = base[r1];
    if (bs < 0) {
      a = g.mirrorMask(a);
      b = g.mirrorMask(b);
      bs = base[mirR1[r1]];
    }
    return bs + rk.rank2(a, b);
  }

  // Index of the canonical form of a physical state where the side to move
  // is the *second* player (pieces `b`, first player's pieces `a`).
  inline i64 indexOfP2ToMove(u64 a, u64 b) const {
    return indexOf(g.rotMask(b), g.rotMask(a));
  }
};

// ---------------------------------------------------------------------------
// 2-bit atomic value table backed by anonymous mmap (zero pages = UNKNOWN).
// ---------------------------------------------------------------------------
struct Table {
  std::atomic<u64>* words = nullptr;
  u64 nStates = 0, nWords = 0;

  void alloc(u64 n) {
    nStates = n;
    nWords = (n * 2 + 63) / 64;
    void* mem = mmap(nullptr, nWords * 8, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { perror("mmap"); abort(); }
    madvise(mem, nWords * 8, MADV_HUGEPAGE);
    words = (std::atomic<u64>*)mem;
  }
  ~Table() {
    if (words) munmap((void*)words, nWords * 8);
  }

  inline u8 get(i64 i) const {
    return (u8)(words[i >> 5].load(std::memory_order_relaxed) >> ((i & 31) * 2)) & 3;
  }
  // Transition UNKNOWN -> v. Values only ever go 0 -> nonzero, so an OR-CAS
  // suffices. Returns true iff this call performed the transition.
  inline bool setIfUnknown(i64 i, u8 v) {
    std::atomic<u64>& w = words[i >> 5];
    int sh = (i & 31) * 2;
    u64 cur = w.load(std::memory_order_relaxed);
    while (((cur >> sh) & 3) == UNKNOWN) {
      if (w.compare_exchange_weak(cur, cur | (u64(v) << sh),
                                  std::memory_order_relaxed))
        return true;
    }
    return false;
  }

  bool saveTo(const std::string& path) const {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open ckpt"); return false; }
    const char* buf = (const char*)words;
    u64 total = nWords * 8, off = 0;
    while (off < total) {
      ssize_t w = write(fd, buf + off, std::min<u64>(total - off, 1u << 28));
      if (w <= 0) { perror("write ckpt"); close(fd); return false; }
      off += (u64)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp.c_str(), path.c_str()) != 0) { perror("rename ckpt"); return false; }
    return true;
  }
  bool loadFrom(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || (u64)st.st_size != nWords * 8) {
      fprintf(stderr, "checkpoint size mismatch for %s\n", path.c_str());
      close(fd);
      return false;
    }
    char* buf = (char*)words;
    u64 total = nWords * 8, off = 0;
    while (off < total) {
      ssize_t r = read(fd, buf + off, std::min<u64>(total - off, 1u << 28));
      if (r <= 0) { perror("read ckpt"); close(fd); return false; }
      off += (u64)r;
    }
    close(fd);
    return true;
  }
};

// Known results from Table 1 of the paper (full state space, both sides to
// move, ignoring symmetry). wins==losses per the paper.
struct KnownResult { int m, p; u64 positions, wins, draws, illegal; };
inline const KnownResult* knownResult(int m, int p) {
  static const KnownResult table[] = {
      {7, 1, 4704ULL, 2304ULL, 0ULL, 96ULL},
      {7, 2, 2542512ULL, 1265851ULL, 0ULL, 10810ULL},
      {7, 3, 559352640ULL, 279297470ULL, 180860ULL, 576840ULL},
      {7, 4, 63136929240ULL, 31532340944ULL, 51686042ULL, 20561310ULL},
      {4, 6, 3363360ULL, 1205441ULL, 547058ULL, 405420ULL},
      {5, 6, 9610154400ULL, 4749618788ULL, 47056118ULL, 63860706ULL},
      {6, 6, 2313100389600ULL, 1153000938173ULL, 5199820604ULL, 1898692650ULL},
  };
  for (const auto& k : table)
    if (k.m == m && k.p == p) return &k;
  return nullptr;
}

}  // namespace cc
