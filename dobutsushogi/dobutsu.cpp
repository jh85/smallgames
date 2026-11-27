// dobutsu.cpp — Strong solver for 4x3 through 9x3 Dobutsu Shogi (animal shogi) via a
// ZDD minimal perfect hash + retrograde analysis, following the architecture of NOCCA x NOCCA
// solver (Yamamoto & Hoki, GPW 2022) with a hand-piece extension.
//
// Two solvers produce the same table. 'solve' is a frontier-driven retrograde analysis:
// only the positions decided in the previous pass are expanded (through un-move
// generation), so the work is proportional to the number of decided positions rather
// than to passes x undecided positions. 'solve-forward' is the original algorithm, which
// re-evaluates every undecided position in every pass; it is kept as an independent
// cross-check.
//
// Position space ("pseudo-reachable", black to move canonically):
//   - R x 3 board, rows 0 (top, White's home) .. R-1 (bottom, Black's home). Black
//     moves toward row 0, White toward row R-1.
//   - Exactly one lion per side on the board. The black lion is never on row 0: a
//     position where the mover's own lion sits on the far rank cannot occur with that
//     player to move (the opponent's preceding turn was terminal).
//   - 2 elephants, 2 giraffes, 2 chicks total, each on the board (either owner; a chick
//     may be promoted to a hen) or in a hand (always as an unpromoted chick).
//
// ZDD encoding: items are R*3 squares x 10 piece-states (state 0 = empty is the default,
// i.e. no item chosen for the square) followed by 6 unary items for BLACK's hand only
// (E>=1, E>=2, G>=1, G>=2, C>=1, C>=2). White's hand is the per-type remainder and needs
// no items, keeping the hash bijective. Frontier signature during construction:
// (square-flag, black lion placed, white lion placed, #E, #G, #C on board).
//
// Rules and value conventions follow the reference Rust solver in
// dobutsushogi/ (which reproduces Tanaka 2009: 246,803,167 reachable positions, second
// player wins):
//   - Chick promotes to Hen (gold-general moves) on entering row 0; mandatory.
//   - Captures put the piece (hen demoted to chick) in the mover's hand; drops on any
//     empty square, no restrictions.
//   - Terminal WIN (for the mover): some black piece attacks the white lion (capture).
//   - Terminal LOSS: the white lion stands on row R-1 (Black's home rank) uncapturable —
//     the "try" rule, formalized one ply after the entering move.
//   - Additionally, moving the black lion to a row-0 square that no white piece attacks
//     is an immediate win (the resulting position is a terminal LOSS for White); this is
//     absorbed into pass 1 so that strict retrograde passes stay single-parity.
//   - A stalemated player (no moves at all) draws under the reference convention; such
//     positions are left unknown.
//
// The result is a packed 2-bit WDL table indexed by the ZDD rank: 0 = draw (once done),
// 1 = win, 2 = loss, from the perspective of the player to move in the canonical
// black-to-move encoding (white-to-move positions are flipped: rows reversed, colors and
// hands swapped).

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cinttypes>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <random>
#include <algorithm>
#include <limits>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using u128 = unsigned __int128;
using i64 = int64_t;

static void die(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  va_end(ap); exit(1);
}

static std::string commas(u64 v) {
  std::string s = std::to_string(v), r;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) {
    r += s[i];
    if (++c % 3 == 0 && i) r += ',';
  }
  std::reverse(r.begin(), r.end());
  return r;
}

static double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------------------------------
// Board and piece tables. States: 0 empty; 1..5 black L,E,G,C,H; 6..10 white L,E,G,C,H.
// ------------------------------------------------------------------------------------------
// Board geometry and representation limits. All fixed-size storage is derived from these
// constants. Attack masks and the independent u128 checker both safely cover a 9x3 board.
static constexpr int C = 3;
static constexpr int MIN_R = 4, MAX_R = 9;
static constexpr int PIECE_STATES = 10;  // non-empty states: black/white L,E,G,C,H
static constexpr int PIECE_KINDS = PIECE_STATES / 2;
static constexpr int BOARD_STATE_COUNT = PIECE_STATES + 1;  // including empty
static constexpr int HAND_TYPES = 3;     // elephant, giraffe, chick
static constexpr int PIECES_PER_TYPE = 2;
static constexpr int HAND_ITEMS = HAND_TYPES * PIECES_PER_TYPE;  // unary >=1, >=2 flags
static constexpr int SQUARE_ENCODING_BITS = 4;
static constexpr int HAND_ENCODING_BITS = 2;
static constexpr int MAXS = MAX_R * C;
static constexpr int MAXARR = MAXS + HAND_ITEMS;
static constexpr int MAX_ZDD_ITEMS = MAXS * PIECE_STATES + HAND_ITEMS;
static_assert(MAXS <= std::numeric_limits<u32>::digits,
              "attack masks need a wider integer type");
static_assert(MAXS <= std::numeric_limits<u8>::max(),
              "move targets need a wider integer type");
static_assert(MAXARR <= std::numeric_limits<u8>::max(),
              "ZDD array indices need a wider integer type");
static_assert(1 + SQUARE_ENCODING_BITS * MAXS
                + HAND_ENCODING_BITS * 2 * HAND_TYPES <= 128,
              "independent checker position keys exceed u128");
static int R = MIN_R, S = R * C;
static int NBOARD_ITEMS = S * PIECE_STATES;
static int NITEMS = NBOARD_ITEMS + HAND_ITEMS, ARR = S + HAND_ITEMS;

static int rowOf_[MAXS], flipSq_[MAXS], mirSq_[MAXS], swapSt_[BOARD_STATE_COUNT];
static std::vector<u8> MOV[PIECE_KINDS + 1][MAXS];  // black states 1..PIECE_KINDS
static std::vector<u8> RMOV[PIECE_KINDS + 1][MAXS]; // inverse: squares q with t in MOV[s][q]
static u32 ATT[BOARD_STATE_COUNT][MAXS];             // bit t: state attacks square t

struct Off { int dr, dc; };
static const Off L_OFF[] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
static const Off E_OFF[] = {{-1,-1},{-1,1},{1,-1},{1,1}};
static const Off G_OFF[] = {{-1,0},{1,0},{0,-1},{0,1}};
static const Off C_OFF[] = {{-1,0}};
static const Off H_OFF[] = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,0}};

static void initTables(int rows) {
  if (rows < MIN_R || rows > MAX_R)
    die("board rows must be between %d and %d", MIN_R, MAX_R);
  R = rows; S = R * C;
  NBOARD_ITEMS = S * PIECE_STATES;
  NITEMS = NBOARD_ITEMS + HAND_ITEMS;
  ARR = S + HAND_ITEMS;
  for (int k = 0; k <= PIECE_KINDS; ++k)
    for (int q = 0; q < MAXS; ++q) { MOV[k][q].clear(); RMOV[k][q].clear(); }
  for (int q = 0; q < S; ++q) {
    int r = q / C, c = q % C;
    rowOf_[q] = r;
    flipSq_[q] = (R - 1 - r) * C + c;
    mirSq_[q] = r * C + (C - 1 - c);
  }
  swapSt_[0] = 0;
  for (int s = 1; s <= PIECE_KINDS; ++s) {
    swapSt_[s] = s + PIECE_KINDS;
    swapSt_[s + PIECE_KINDS] = s;
  }
  auto offs = [](int kind, int& n) -> const Off* {
    switch (kind) {
      case 0: n = 8; return L_OFF;
      case 1: n = 4; return E_OFF;
      case 2: n = 4; return G_OFF;
      case 3: n = 1; return C_OFF;
      default: n = 6; return H_OFF;
    }
  };
  for (int s = 1; s <= PIECE_STATES; ++s) {
    int kind = (s - 1) % PIECE_KINDS, white = s > PIECE_KINDS, n;
    const Off* o = offs(kind, n);
    for (int q = 0; q < S; ++q) {
      int r = q / C, c = q % C;
      u32 m = 0;
      for (int i = 0; i < n; ++i) {
        int dr = white ? -o[i].dr : o[i].dr, dc = o[i].dc;
        int r2 = r + dr, c2 = c + dc;
        if (r2 < 0 || r2 >= R || c2 < 0 || c2 >= C) continue;
        int t = r2 * C + c2;
        m |= (u32)1 << t;
        if (!white) MOV[s][q].push_back((u8)t);
      }
      ATT[s][q] = m;
    }
  }
  for (int s = 1; s <= PIECE_KINDS; ++s)
    for (int q = 0; q < S; ++q)
      for (u8 t : MOV[s][q]) RMOV[s][t].push_back((u8)q);
}

// Left/right reflection leaves hand contents unchanged.  Reduced tables choose the
// lexicographically smaller board when rows are scanned top-to-bottom and the two outer
// columns are compared.  (The middle column is fixed by reflection.)  The particular
// ordering is immaterial; it only needs to select exactly one member of every orbit.
static int compareReflection(const u8* arr) {
  for (int r = 0; r < R; ++r) {
    int left = arr[r * C], right = arr[r * C + C - 1];
    if (left != right) return left < right ? -1 : 1;
  }
  return 0;
}

static void mirrorPosition(const u8* src, u8* dst) {
  for (int q = 0; q < S; ++q) dst[mirSq_[q]] = src[q];
  memcpy(dst + S, src + S, HAND_ITEMS);
}

static const u8* reflectionRepresentative(const u8* src, u8* scratch,
                                           bool reduceLR) {
  if (!reduceLR || compareReflection(src) <= 0) return src;
  mirrorPosition(src, scratch);
  return scratch;
}

// ------------------------------------------------------------------------------------------
// ZDD over S*10+6 items; canonical build by memoized recursion.
// arr layout: [0..S-1] board square states (0..10), then 6 black-hand unary flags
// (E>=1, E>=2, G>=1, G>=2, C>=1, C>=2), default all 0.
// ------------------------------------------------------------------------------------------
struct Zdd {
  std::vector<u8>  idx_, val_;
  std::vector<u32> lo_, hi_;
  std::vector<u64> cnt_, cntlo_;
  u32 root = 0;
  u64 total = 0;
  bool reduceLR = false;

  // Base signature occupies bits 0..8.  In reflection-reduced mode bit 9 says an
  // earlier row has already established left<right, and bits 10..13 remember the
  // state in the left square until the right square of that row is processed.
  static constexpr int SIG_BITS = 14;
  explicit Zdd(bool reduced = false) : reduceLR(reduced) {}
  struct UniqueKey {
    u32 d, lo, hi;
    bool operator==(const UniqueKey&) const = default;
  };
  struct UniqueHash {
    size_t operator()(const UniqueKey& k) const noexcept {
      u64 h = (u64)k.d * 0x9E3779B97F4A7C15ull;
      h ^= (u64)k.lo * 0xC2B2AE3D27D4EB4Full;
      h ^= (u64)k.hi * 0x165667B19E3779F9ull;
      return (size_t)(h ^ (h >> 32));
    }
  };

  std::unordered_map<u64, u32> memo;
  std::unordered_map<UniqueKey, u32, UniqueHash> uniq;

  // sig: f | lb<<1 | lw<<2 | nE<<3 | nG<<5 | nC<<7  (9 bits)
  bool advance(int d, u32 sig, int b, u32& out) const {
    int f = sig & 1, lb = (sig >> 1) & 1, lw = (sig >> 2) & 1;
    int nE = (sig >> 3) & 3, nG = (sig >> 5) & 3, nC = (sig >> 7) & 3;
    int less = (sig >> 9) & 1, left = (sig >> 10) & 15;
    if (d < NBOARD_ITEMS) {
      int q = d / PIECE_STATES, s = d % PIECE_STATES + 1;
      if (b) {
        if (f) return false;
        switch (s) {
          case 1: if (lb || rowOf_[q] == 0) return false; lb = 1; break;
          case 6: if (lw) return false; lw = 1; break;
          case 2: case 7: if (nE >= PIECES_PER_TYPE) return false; ++nE; break;
          case 3: case 8: if (nG >= PIECES_PER_TYPE) return false; ++nG; break;
          default: if (nC >= PIECES_PER_TYPE) return false; ++nC; break;  // 4,5,9,10
        }
        f = 1;
        if (reduceLR && q % C == 0) left = s;
        if (reduceLR && q % C == C - 1 && !less) {
          if (left > s) return false;
          if (left < s) less = 1;
        }
      }
      if (s == PIECE_STATES) {
        // If no item was selected in the right square, its state is empty (zero).
        if (reduceLR && q % C == C - 1) {
          if (!f && !less && left > 0) return false;
          left = 0;
        }
        f = 0;  // square boundary: all-zero means empty
      }
    } else {
      int h = d - NBOARD_ITEMS, type = h / PIECES_PER_TYPE;
      bool first = h % PIECES_PER_TYPE == 0;
      int avail = PIECES_PER_TYPE - (type == 0 ? nE : type == 1 ? nG : nC);
      if (b) {
        if (first) { if (avail < 1) return false; f = 1; }
        else       { if (!f || avail < PIECES_PER_TYPE) return false; f = 0; }
      } else {
        f = 0;
      }
      if (h % PIECES_PER_TYPE == PIECES_PER_TYPE - 1) f = 0;
    }
    out = (u32)f | (u32)lb << 1 | (u32)lw << 2 | (u32)nE << 3
        | (u32)nG << 5 | (u32)nC << 7 | (u32)less << 9 | (u32)left << 10;
    return true;
  }

  u32 rec(int d, u32 sig) {
    if (d == NITEMS) {
      return (((sig >> 1) & 1) && ((sig >> 2) & 1)) ? 1u : 0u;
    }
    u64 key = (u64)d << SIG_BITS | sig;
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    u32 s2, lo = 0, hi = 0;
    if (advance(d, sig, 0, s2)) lo = rec(d + 1, s2);
    if (advance(d, sig, 1, s2)) hi = rec(d + 1, s2);
    u32 res;
    if (hi == 0) res = lo;
    else {
      UniqueKey hkey{(u32)d, lo, hi};
      auto it2 = uniq.find(hkey);
      if (it2 != uniq.end()) res = it2->second;
      else {
        if (idx_.size() >= std::numeric_limits<u32>::max())
          die("ZDD node count exceeds its 32-bit representation");
        res = (u32)idx_.size();
        int ai = d < NBOARD_ITEMS ? d / PIECE_STATES : S + (d - NBOARD_ITEMS);
        int av = d < NBOARD_ITEMS ? d % PIECE_STATES + 1 : 1;
        idx_.push_back((u8)ai); val_.push_back((u8)av);
        lo_.push_back(lo); hi_.push_back(hi);
        uniq.emplace(hkey, res);
      }
    }
    memo.emplace(key, res);
    return res;
  }

  void build() {
    idx_.assign(2, (u8)ARR); val_.assign(2, 0);
    lo_.assign(2, 0); hi_.assign(2, 0);
    root = rec(0, 0);
    memo.clear(); uniq.clear();
    cnt_.assign(idx_.size(), 0);
    cnt_[0] = 0; cnt_[1] = 1;
    for (u32 i = 2; i < (u32)idx_.size(); ++i) {
      u64 loCount = cnt_[lo_[i]], hiCount = cnt_[hi_[i]];
      if (hiCount > std::numeric_limits<u64>::max() - loCount)
        die("position count exceeds the 64-bit rank representation");
      cnt_[i] = loCount + hiCount;
    }
    cntlo_.assign(idx_.size(), 0);
    for (u32 i = 2; i < (u32)idx_.size(); ++i) cntlo_[i] = cnt_[lo_[i]];
    total = cnt_[root];
  }

  // Independent DP count of the same set (validation).
  static u64 dpCount() {
    // state: lb | lw<<1 | nE<<2 | nG<<4 | nC<<6  (8 bits)
    static constexpr int DP_STATE_COUNT = 1 << 8;
    std::vector<u128> dp(DP_STATE_COUNT, 0), nx;
    dp[0] = 1;
    for (int q = 0; q < S; ++q) {
      nx.assign(DP_STATE_COUNT, 0);
      for (int st = 0; st < DP_STATE_COUNT; ++st) {
        u128 v = dp[st];
        if (!v) continue;
        int lb = st & 1, lw = (st >> 1) & 1, nE = (st >> 2) & 3, nG = (st >> 4) & 3, nC = (st >> 6) & 3;
        nx[st] += v;  // empty
        for (int s = 1; s <= PIECE_STATES; ++s) {
          int lb2 = lb, lw2 = lw, nE2 = nE, nG2 = nG, nC2 = nC;
          if (s == 1) { if (lb || rowOf_[q] == 0) continue; lb2 = 1; }
          else if (s == 6) { if (lw) continue; lw2 = 1; }
          else if (s == 2 || s == 7) { if (nE >= PIECES_PER_TYPE) continue; ++nE2; }
          else if (s == 3 || s == 8) { if (nG >= PIECES_PER_TYPE) continue; ++nG2; }
          else { if (nC >= PIECES_PER_TYPE) continue; ++nC2; }
          nx[lb2 | lw2 << 1 | nE2 << 2 | nG2 << 4 | nC2 << 6] += v;
        }
      }
      dp.swap(nx);
    }
    u128 total = 0;
    for (int st = 0; st < DP_STATE_COUNT; ++st) {
      if (!(st & 1) || !((st >> 1) & 1)) continue;
      int nE = (st >> 2) & 3, nG = (st >> 4) & 3, nC = (st >> 6) & 3;
      total += dp[st] * (u128)(PIECES_PER_TYPE + 1 - nE)
                        * (PIECES_PER_TYPE + 1 - nG)
                        * (PIECES_PER_TYPE + 1 - nC);
    }
    if (total > std::numeric_limits<u64>::max())
      die("DP position count exceeds the 64-bit rank representation");
    return (u64)total;
  }

  // Independent count of positions fixed by left/right reflection.  Burnside's lemma
  // then gives the number of reflection orbits as (full + fixed) / 2.
  static u64 dpMirrorFixedCount() {
    static constexpr int DP_STATE_COUNT = 1 << 8;
    std::vector<u128> dp(DP_STATE_COUNT, 0), nx;
    dp[0] = 1;
    auto place = [](int st, int piece, int row, int copies, int& out) {
      int lb = st & 1, lw = (st >> 1) & 1;
      int nE = (st >> 2) & 3, nG = (st >> 4) & 3, nC = (st >> 6) & 3;
      if (!piece) { out = st; return true; }
      if (piece == 1) {
        if (copies != 1 || lb || row == 0) return false;
        lb = 1;
      } else if (piece == 6) {
        if (copies != 1 || lw) return false;
        lw = 1;
      } else if (piece == 2 || piece == 7) {
        if (nE + copies > PIECES_PER_TYPE) return false;
        nE += copies;
      } else if (piece == 3 || piece == 8) {
        if (nG + copies > PIECES_PER_TYPE) return false;
        nG += copies;
      } else {
        if (nC + copies > PIECES_PER_TYPE) return false;
        nC += copies;
      }
      out = lb | lw << 1 | nE << 2 | nG << 4 | nC << 6;
      return true;
    };
    for (int r = 0; r < R; ++r) {
      nx.assign(DP_STATE_COUNT, 0);
      for (int st = 0; st < DP_STATE_COUNT; ++st) {
        if (!dp[st]) continue;
        for (int outer = 0; outer <= PIECE_STATES; ++outer) {
          int st2;
          if (!place(st, outer, r, 2, st2)) continue;
          for (int middle = 0; middle <= PIECE_STATES; ++middle) {
            int st3;
            if (place(st2, middle, r, 1, st3)) nx[st3] += dp[st];
          }
        }
      }
      dp.swap(nx);
    }
    u128 total = 0;
    for (int st = 0; st < DP_STATE_COUNT; ++st) {
      if (!(st & 1) || !((st >> 1) & 1)) continue;
      int nE = (st >> 2) & 3, nG = (st >> 4) & 3, nC = (st >> 6) & 3;
      total += dp[st] * (u128)(PIECES_PER_TYPE + 1 - nE)
                        * (PIECES_PER_TYPE + 1 - nG)
                        * (PIECES_PER_TYPE + 1 - nC);
    }
    if (total > std::numeric_limits<u64>::max())
      die("fixed-position count exceeds the 64-bit representation");
    return (u64)total;
  }

  u64 rank(const u8* arr) const {
    u32 n = root; u64 k = 0;
    while (n > 1) {
      if (arr[idx_[n]] == val_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }

  u64 rankCanonical(const u8* arr, u8* scratch) const {
    return rank(reflectionRepresentative(arr, scratch, reduceLR));
  }

  void unrank(u64 k, u8* arr) const {
    memset(arr, 0, ARR);
    u32 n = root;
    while (n > 1) {
      if (cntlo_[n] <= k) { k -= cntlo_[n]; arr[idx_[n]] = val_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    assert(n == 1 && k == 0);
  }

  void buildPrefix(const u8* arr, u32* nodeAt, u64* kAt) const {
    u32 n = root; u64 k = 0; int last = -1;
    for (;;) {
      int t = (n > 1) ? (int)idx_[n] : ARR;
      for (int j = last + 1; j <= t; ++j) { nodeAt[j] = n; kAt[j] = k; }
      last = t;
      if (n <= 1) break;
      if (arr[idx_[n]] == val_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
  }

  u64 rankFrom(u32 n, u64 k, const u8* arr) const {
    while (n > 1) {
      if (arr[idx_[n]] == val_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }
};

struct ZddIter {
  const Zdd* z = nullptr;
  int sp = 0;
  u32 nstack[MAX_ZDD_ITEMS + 1];
  u8 br[MAX_ZDD_ITEMS + 1];
  u8 pos[MAXARR];

  void descend(u32 m) {
    while (m > 1) {
      nstack[sp] = m;
      if (z->lo_[m] == 0) { br[sp] = 1; pos[z->idx_[m]] = z->val_[m]; ++sp; m = z->hi_[m]; }
      else { br[sp] = 0; ++sp; m = z->lo_[m]; }
    }
    assert(m == 1);
  }
  void initAt(const Zdd& zz, u64 k) {
    z = &zz; sp = 0;
    memset(pos, 0, ARR);
    u32 n = z->root;
    while (n > 1) {
      nstack[sp] = n;
      if (z->cntlo_[n] <= k) { k -= z->cntlo_[n]; br[sp] = 1; pos[z->idx_[n]] = z->val_[n]; n = z->hi_[n]; }
      else { br[sp] = 0; n = z->lo_[n]; }
      ++sp;
    }
    assert(n == 1 && k == 0);
  }
  bool next() {
    while (sp) {
      --sp;
      u32 n = nstack[sp];
      if (br[sp]) { pos[z->idx_[n]] = 0; }
      else { br[sp] = 1; pos[z->idx_[n]] = z->val_[n]; ++sp; descend(z->hi_[n]); return true; }
    }
    return false;
  }
};

// ------------------------------------------------------------------------------------------
// Game logic on canonical (black-to-move) positions in arr form.
// ------------------------------------------------------------------------------------------
static inline void handCounts(const u8* arr, int* hb) {
  hb[0] = arr[S + 0] + arr[S + 1]; hb[1] = arr[S + 2] + arr[S + 3]; hb[2] = arr[S + 4] + arr[S + 5];
}
static inline void whiteHand(const u8* arr, const int* hb, int* hw) {
  int on[HAND_TYPES] = {0, 0, 0};
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    if (s == 2 || s == 7) ++on[0];
    else if (s == 3 || s == 8) ++on[1];
    else if (s == 4 || s == 5 || s == 9 || s == 10) ++on[2];
  }
  for (int t = 0; t < HAND_TYPES; ++t) hw[t] = PIECES_PER_TYPE - on[t] - hb[t];
}
// child canonical form: flip rows, swap colors, hands swap (black hand := old white hand)
static inline void buildFlip(const u8* arr, const int* hw, u8* fpos) {
  for (int q = 0; q < S; ++q) fpos[flipSq_[q]] = (u8)swapSt_[arr[q]];
  for (int t = 0; t < HAND_TYPES; ++t) {
    fpos[S + 2 * t] = hw[t] >= 1;
    fpos[S + 1 + 2 * t] = hw[t] >= 2;
  }
}

// Pass-1 intrinsic value: 1 = win (can capture white lion, or safe lion try to row 0),
// 2 = loss (white lion on row R-1, uncapturable), 0 = neither (incl. stalemate = draw).
static int pass1Eval(const u8* arr) {
  int wl = -1, bl = -1;
  u32 attB = 0;
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    if (s == 0) continue;
    if (s == 6) wl = q;
    else if (s == 1) bl = q;
    if (s <= 5) attB |= ATT[s][q];
  }
  if ((attB >> wl) & 1) return 1;
  if (rowOf_[wl] == R - 1) return 2;
  for (u8 t : MOV[1][bl]) {
    if (rowOf_[t] != 0) continue;
    int st = arr[t];
    if (st >= 1 && st <= 6) continue;      // own piece or white lion (capture handled above)
    bool attacked = false;
    for (int u = 0; u < S; ++u) {
      int su = arr[u];    // any white piece, INCLUDING the white lion, can capture the trier
      if (su >= 6 && su <= 10 && u != (int)t && ((ATT[su][u] >> t) & 1)) { attacked = true; break; }
    }
    if (!attacked) return 1;               // safe try: White faces a terminal loss
  }
  return 0;
}

// True only when play has already ended in this position. Unlike pass1Eval(), this does
// not treat the availability of a safe lion try as terminal: a reachability traversal
// must still include every other legal move from such a position.
static bool gameAlreadyOver(const u8* arr) {
  int wl = -1;
  u32 attB = 0;
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    if (s == 6) wl = q;
    if (s >= 1 && s <= 5) attB |= ATT[s][q];
  }
  assert(wl >= 0);
  return ((attB >> wl) & 1) || rowOf_[wl] == R - 1;
}

// ------------------------------------------------------------------------------------------
// Signature-indexed ranking.
//
// The ZDD rank of a position is the number of members that precede it in the ZDD's order:
// square by square, where at one square the states are ordered empty, 10, 9, ..., 1, and
// finally the hand counts in ascending E, G, C order. The number of completions of a
// prefix depends only on a small signature of that prefix — (black lion placed, white
// lion placed, #E, #G, #C on the board) and, in reflection-reduced mode, whether an
// earlier row already decided left < right plus the state of the current row's left
// square. The rank is therefore the sum of one table entry per square. This yields exactly
// the ZDD's ranks, but without chasing about ten ZDD nodes per square, and the table
// addresses do not depend on earlier loads. The tables come from an independent DP and are
// cross-checked against the ZDD by 'selftest'.
// ------------------------------------------------------------------------------------------
static constexpr int NSIG = 4 * 27;                // lb | lw << 1 | (nE + 3 nG + 9 nC) << 2
static constexpr int NVAR_LR = 2 + PIECE_STATES;   // 0: already less; 1 + left: still tied
static const u8 SIG_DELTA[BOARD_STATE_COUNT] = {0, 1, 4, 12, 36, 36, 2, 4, 12, 36, 36};

struct SigRank {
  bool lr = false;
  int nv = 1;
  size_t sqStride = 0;       // table entries per square: nv * NSIG * BOARD_STATE_COUNT
  std::vector<u64> T;        // [S][nv][NSIG][state]: members preceding `state` at a square
  std::vector<u64> CNT;      // [S + 1][nv][NSIG]: completions of a prefix
  u64 total = 0;
  u8 handM1[NSIG], handM2[NSIG], handN[NSIG];
  u8 handOf[NSIG][27][HAND_TYPES];   // hand counts for a hand rank, avoiding divisions

  u64& cnt(int q, int v, int b) { return CNT[((size_t)q * nv + v) * NSIG + b]; }
  u64 cnt(int q, int v, int b) const { return CNT[((size_t)q * nv + v) * NSIG + b]; }
  const u64* row(int q, int v, int b) const {
    return &T[(size_t)q * sqStride + ((size_t)v * NSIG + b) * BOARD_STATE_COUNT];
  }

  // Signature after placing state s at square q; false if the piece cannot be placed.
  static bool place(int q, int b, int s, int& b2) {
    int n = b >> 2;
    bool ok;
    int delta = SIG_DELTA[s];
    switch (s) {
      case 0: ok = true; break;
      case 1: ok = !(b & 1) && rowOf_[q] != 0; break;
      case 6: ok = !(b & 2); break;
      case 2: case 7: ok = n % 3 < PIECES_PER_TYPE; break;
      case 3: case 8: ok = n / 3 % 3 < PIECES_PER_TYPE; break;
      default: ok = n / 9 < PIECES_PER_TYPE; break;
    }
    b2 = b + delta;
    return ok;
  }

  // Reflection variant after state s at square q. Returns false when the position stops
  // being the selected member of its orbit (left > right in the first unequal row).
  bool nextVar(int q, int v, int s, int& v2) const {
    v2 = 0;
    if (!lr) return true;
    int col = q % C;
    if (col == 0) { v2 = v ? 1 + s : 0; return true; }
    if (col < C - 1) { v2 = v; return true; }
    if (!v) return true;
    int left = v - 1;
    if (s < left) return false;
    v2 = s == left ? 1 : 0;
    return true;
  }

  void build(bool reduced) {
    lr = reduced;
    nv = lr ? NVAR_LR : 1;
    sqStride = (size_t)nv * NSIG * BOARD_STATE_COUNT;
    T.assign((size_t)S * sqStride, 0);
    CNT.assign((size_t)(S + 1) * nv * NSIG, 0);
    for (int b = 0; b < NSIG; ++b) {
      int nE = (b >> 2) % 3, nG = (b >> 2) / 3 % 3, nC = (b >> 2) / 9;
      handM2[b] = (u8)(PIECES_PER_TYPE + 1 - nC);
      handM1[b] = (u8)((PIECES_PER_TYPE + 1 - nG) * (PIECES_PER_TYPE + 1 - nC));
      handN[b] = (u8)((PIECES_PER_TYPE + 1 - nE) * handM1[b]);
      for (int h = 0; h < 27; ++h) {
        handOf[b][h][0] = (u8)(h / handM1[b]);
        handOf[b][h][1] = (u8)(h % handM1[b] / handM2[b]);
        handOf[b][h][2] = (u8)(h % handM2[b]);
      }
      if ((b & 3) == 3)
        for (int v = 0; v < std::min(nv, 2); ++v) cnt(S, v, b) = handN[b];
    }
    for (int q = S - 1; q >= 0; --q)
      for (int v = 0; v < nv; ++v) {
        if (lr && q % C == 0 && v > 1) continue;   // the left square only has two variants
        for (int b = 0; b < NSIG; ++b) {
          u64* out = &T[(size_t)q * sqStride + ((size_t)v * NSIG + b) * BOARD_STATE_COUNT];
          u128 acc = 0;
          for (int j = 0; j <= PIECE_STATES; ++j) {
            int s = j == 0 ? 0 : PIECE_STATES + 1 - j;   // ZDD order: empty, 10, 9, ..., 1
            out[s] = (u64)acc;
            int b2, v2;
            if (place(q, b, s, b2) && nextVar(q, v, s, v2)) acc += cnt(q + 1, v2, b2);
          }
          if (acc > std::numeric_limits<u64>::max())
            die("position count exceeds the 64-bit rank representation");
          cnt(q, v, b) = (u64)acc;
        }
      }
    total = cnt(0, lr ? 1 : 0, 0);
  }

  // Sum the table entries of rows r0..R-1 of the board `a` and add the hand rank. (k, b,
  // tied) describe the prefix above row r0. `a` must be the selected orbit member.
  inline u64 rankRows(const u8* a, int r0, u64 k, u32 b, u32 tied, const int* hand) const {
    const size_t st = sqStride;
    const u64* t = T.data() + (size_t)r0 * C * st;
    for (int q = r0 * C; q < S; q += C, t += C * st) {
      u32 l = a[q], m = a[q + 1], r = a[q + 2];
      u32 v = tied * (1 + l);
      k += t[(tied * NSIG + b) * BOARD_STATE_COUNT + l];
      b += SIG_DELTA[l];
      k += t[st + (v * NSIG + b) * BOARD_STATE_COUNT + m];
      b += SIG_DELTA[m];
      k += t[2 * st + (v * NSIG + b) * BOARD_STATE_COUNT + r];
      b += SIG_DELTA[r];
      tied &= (u32)(l == r);
    }
    return k + (u64)hand[0] * handM1[b] + (u64)hand[1] * handM2[b] + (u64)hand[2];
  }

  u64 rank(const u8* arr) const {
    int hb[HAND_TYPES];
    handCounts(arr, hb);
    return rankRows(arr, 0, 0, 0, lr ? 1 : 0, hb);
  }
};
static_assert(C == 3, "SigRank::rankRows is written for three columns");

// Position iterator over ranks. seek() costs time proportional to the number of trailing
// squares that differ from the current position, so both dense scans and sparse jumps are
// cheap. kpre[q]..kend[q] is the rank interval sharing the current squares 0..q-1; in
// particular [kpre[S], kend[S]) are the ranks that share the whole board.
struct SigIter {
  const SigRank* sr = nullptr;
  u8 pos[MAXARR];
  u64 kpre[MAXS + 1], kend[MAXS + 1];
  u8 sig[MAXS + 1], var[MAXS + 1];

  void init(const SigRank& s) {
    sr = &s;
    memset(pos, 0, sizeof pos);
    for (int q = 1; q <= S; ++q) { kpre[q] = 1; kend[q] = 0; }   // empty intervals
    kpre[0] = 0; kend[0] = s.total; sig[0] = 0; var[0] = s.lr ? 1 : 0;
  }

  void seek(u64 k) {
    int q = S;
    while (k < kpre[q] || k >= kend[q]) --q;
    u64 r = k - kpre[q];
    for (; q < S; ++q) {
      const u64* row = sr->row(q, var[q], sig[q]);
      int c = 0;
      for (int s = 1; s <= PIECE_STATES; ++s) c += row[s] <= r;
      int s = c ? PIECE_STATES + 1 - c : 0;
      pos[q] = (u8)s;
      r -= row[s];
      int v2;
      sr->nextVar(q, var[q], s, v2);
      kpre[q + 1] = k - r;
      sig[q + 1] = (u8)(sig[q] + SIG_DELTA[s]);
      var[q + 1] = (u8)v2;
      kend[q + 1] = kpre[q + 1] + sr->cnt(q + 1, v2, sig[q + 1]);
    }
    const u8* h = sr->handOf[sig[S]][k - kpre[S]];
    pos[S + 0] = h[0] >= 1; pos[S + 1] = h[0] >= 2;
    pos[S + 2] = h[1] >= 1; pos[S + 3] = h[1] >= 2;
    pos[S + 4] = h[2] >= 1; pos[S + 5] = h[2] >= 2;
  }
};

// Ranks boards that differ from a base board in one or two squares. The per-row prefix
// sums of the base are kept for both orientations (each computed on first use), so a
// modified board is ranked by summing only from its first changed row, in whichever
// orientation is the selected member of its orbit.
struct RankCtx {
  const SigRank* sr = nullptr;
  u8 w[2][MAXS];                 // the base board and its mirror image
  bool has[2];
  u64 kRow[2][MAX_R];
  u32 bRow[2][MAX_R], tRow[2][MAX_R];
  int8_t cmp[MAX_R];             // sign(left - right) per row of the base
  u8 nextDiff[MAX_R + 2];        // first row >= r with cmp != 0, or R

  // An orientation that is not the selected member has meaningless sums below its first
  // violating row. They are never used: a modified board that is the selected member in
  // some orientation shares that orientation's rows above its first change, so those rows
  // contain no violation.
  void prefix(int o) {
    const size_t st = sr->sqStride;
    const u64* t = sr->T.data();
    const u8* a = w[o];
    u64 k = 0;
    u32 b = 0, tied = sr->lr ? 1 : 0;
    for (int r = 0, q = 0; r < R; ++r, q += C, t += C * st) {
      kRow[o][r] = k; bRow[o][r] = b; tRow[o][r] = tied;
      u32 l = a[q], m = a[q + 1], rt = a[q + 2];
      u32 v = tied * (1 + l);
      k += t[(tied * NSIG + b) * BOARD_STATE_COUNT + l];
      b += SIG_DELTA[l];
      k += t[st + (v * NSIG + b) * BOARD_STATE_COUNT + m];
      b += SIG_DELTA[m];
      k += t[2 * st + (v * NSIG + b) * BOARD_STATE_COUNT + rt];
      b += SIG_DELTA[rt];
      tied &= (u32)(l == rt);
    }
    has[o] = true;
  }

  void build(const u8* base) {
    memcpy(w[0], base, S);
    has[0] = has[1] = false;
    if (!sr->lr) return;
    for (int q = 0; q < S; ++q) w[1][mirSq_[q]] = base[q];
    nextDiff[R] = nextDiff[R + 1] = (u8)R;
    for (int r = R - 1; r >= 0; --r) {
      int l = base[r * C], rt = base[r * C + C - 1];
      cmp[r] = (int8_t)((l > rt) - (l < rt));
      nextDiff[r] = cmp[r] ? (u8)r : nextDiff[r + 1];
    }
  }

  // sign(left - right) of row r of the base with squares q1 := s1 and q2 := s2
  inline int rowSign(int r, int q1, int s1, int q2, int s2) const {
    int ql = r * C, qr = ql + C - 1;
    int l = w[0][ql], rt = w[0][qr];
    if (q1 == ql) l = s1; else if (q1 == qr) rt = s1;
    if (q2 == ql) l = s2; else if (q2 == qr) rt = s2;
    return (l > rt) - (l < rt);
  }

  // 1 if the modified base (changed rows r1 <= r2) must be mirrored to be the selected
  // member of its orbit.
  inline int orient(int r1, int r2, int q1, int s1, int q2, int s2) const {
    if (!sr->lr) return 0;
    int nd = nextDiff[0];
    if (nd < r1) return cmp[nd] > 0;
    int sign = rowSign(r1, q1, s1, q2, s2);
    if (sign) return sign > 0;
    if (r2 != r1) {
      nd = nextDiff[r1 + 1];
      if (nd < r2) return cmp[nd] > 0;
      sign = rowSign(r2, q1, s1, q2, s2);
      if (sign) return sign > 0;
    }
    nd = nextDiff[r2 + 1];
    return nd < R && cmp[nd] > 0;
  }

  // Rank of the base with squares q1 := s1 and q2 := s2 (q2 < 0: one square only) and the
  // given explicit hand.
  inline u64 rank2(int q1, u8 s1, int q2, u8 s2, const int* hand) {
    int r1 = rowOf_[q1], r2 = q2 < 0 ? r1 : rowOf_[q2];
    if (r1 > r2) std::swap(r1, r2);
    int o = orient(r1, r2, q1, s1, q2, s2);
    if (!has[o]) prefix(o);
    u8* a = w[o];
    if (o) { q1 = mirSq_[q1]; if (q2 >= 0) q2 = mirSq_[q2]; }
    if (q2 < 0) q2 = q1, s2 = s1;
    u8 old1 = a[q1], old2 = a[q2];
    a[q1] = s1; a[q2] = s2;
    u64 k = sr->rankRows(a, r1, kRow[o][r1], bRow[o][r1], tRow[o][r1], hand);
    a[q1] = old1; a[q2] = old2;
    return k;
  }
  inline u64 rank1(int q, u8 s, const int* hand) { return rank2(q, s, -1, 0, hand); }
};

// An upper bound on the number of moves: 36 board moves plus 3 x 25 drops.
static constexpr int MAX_KIDS = 128;

// The legal moves of the side to move, expressed as changes to the child's base board f
// (the position with rows flipped and colours swapped): a board move empties f1 and puts
// `state` on f2; a drop (f1 == f2) puts `state` on f1. Every child has the explicit hand
// hw, the opponent's hand, which no move changes.
struct KidMove { u8 f1, f2, state; };

struct MoveList {
  int n;
  bool any;                  // a legal move exists, counting those left out by skipWon
  int hw[HAND_TYPES];
  u8 f[MAXS];
  KidMove m[MAX_KIDS];
};

// Lists the moves of `arr` (black to move) in a fixed order. With skipWon, moves after
// which the opponent can capture the mover's lion at once are left out: pass 1 decided
// every such child as a win for the opponent, so the solver needs no lookup for them.
static void listMoves(const u8* arr, bool skipWon, MoveList& L) {
  static const u8 TYPE_OF[BOARD_STATE_COUNT] = {3, 3, 0, 1, 2, 2, 3, 0, 1, 2, 2};
  int on[HAND_TYPES + 1] = {0, 0, 0, 0}, hb[HAND_TYPES];
  u8 mine[MAXS], theirs[MAXS], empty[MAXS];
  int nm = 0, nt = 0, ne = 0, bl = -1;
  u32 attW = 0;
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    L.f[flipSq_[q]] = (u8)swapSt_[s];
    ++on[TYPE_OF[s]];
    if (!s) empty[ne++] = (u8)q;
    else if (s <= PIECE_KINDS) { mine[nm++] = (u8)q; if (s == 1) bl = q; }
    else { theirs[nt++] = (u8)q; attW |= ATT[s][q]; }
  }
  handCounts(arr, hb);
  for (int t = 0; t < HAND_TYPES; ++t) L.hw[t] = PIECES_PER_TYPE - on[t] - hb[t];
  u32 checkers = 0;          // squares of the white pieces attacking the black lion
  if (!skipWon) attW = 0;
  else if ((attW >> bl) & 1)
    for (int i = 0; i < nt; ++i) {
      int q = theirs[i];
      if ((ATT[arr[q]][q] >> bl) & 1) checkers |= (u32)1 << q;
    }
  int n = 0;
  bool any = false;
  for (int i = 0; i < nm; ++i) {
    int q = mine[i], s = arr[q];
    for (u8 t : MOV[s][q]) {
      int tt = arr[t];
      if (tt >= 1 && tt <= 6) continue;            // own piece or white lion
      any = true;
      // A piece never attacks its own square, so attW already excludes a captured piece
      // for lion moves; for other moves the captured checker is masked out.
      if (s == 1 ? (attW >> t) & 1 : (checkers & ~((u32)1 << t)) != 0) continue;
      if (n == MAX_KIDS) die("more than %d moves", MAX_KIDS);
      int ns = (s == 4 && rowOf_[t] == 0) ? 5 : s;
      L.m[n++] = {(u8)flipSq_[q], (u8)flipSq_[t], (u8)(ns + PIECE_KINDS)};
    }
  }
  for (int type = 0; type < HAND_TYPES; ++type) {
    if (!hb[type] || !ne) continue;
    any = true;
    if (checkers) break;                           // a drop never answers a check
    u8 dropped = (u8)(2 + type + PIECE_KINDS);     // after the flip: white E/G/C
    for (int i = 0; i < ne; ++i) {
      if (n == MAX_KIDS) die("more than %d moves", MAX_KIDS);
      u8 fq = (u8)flipSq_[empty[i]];
      L.m[n++] = {fq, fq, dropped};
    }
  }
  L.n = n; L.any = any;
}

// cx must have been built on L.f.
static inline u64 rankMove(RankCtx& cx, const MoveList& L, int i) {
  const KidMove& m = L.m[i];
  return m.f1 == m.f2 ? cx.rank1(m.f1, m.state, L.hw)
                      : cx.rank2(m.f1, 0, m.f2, m.state, L.hw);
}

// Ranks of the first `limit` children of `arr` as selected orbit members; `complete`
// tells whether those were all of them.
static int genChildren(const u8* arr, RankCtx& cx, u64* out, int limit, bool skipWon,
                       bool& complete, bool& any) {
  MoveList L;
  listMoves(arr, skipWon, L);
  any = L.any;
  complete = L.n <= limit;
  int n = std::min(L.n, limit);
  if (n) cx.build(L.f);
  for (int i = 0; i < n; ++i) out[i] = rankMove(cx, L, i);
  return n;
}

// Ranks of the predecessors of `arr`: every position Y, not already won by capturing the
// lion, from which one black move leads to `arr` (up to reflection). This is the exact
// inverse of genChildren, which 'selftest' verifies in both directions. With Z the
// position after the move seen from the mover's side (rows flipped, colours swapped), a
// mover's piece is moved back to an empty source square — optionally restoring a captured
// piece from the mover's hand — or is taken back into the hand if it could have been
// dropped. Predecessors in which another mover's piece attacks the opposing lion are
// skipped without ranking: pass 1 decided them as wins.
template <class Emit>
static inline void genPreds(const u8* arr, RankCtx& cx, Emit&& emit) {
  int hb[HAND_TYPES], hw[HAND_TYPES];
  handCounts(arr, hb);
  whiteHand(arr, hb, hw);            // hw: the mover's hand after the move
  u8 z[MAXS], mover[MAXS];
  int nm = 0, wl = -1, bl = -1;
  for (int q = 0; q < S; ++q) z[flipSq_[q]] = (u8)swapSt_[arr[q]];
  for (int q = 0; q < S; ++q) {
    int s = z[q];
    if (s == 6) wl = q;
    else if (s >= 1 && s <= PIECE_KINDS) { mover[nm++] = (u8)q; if (s == 1) bl = q; }
  }
  int natt = 0;
  for (int i = 0; i < nm; ++i) natt += (ATT[z[mover[i]]][mover[i]] >> wl) & 1;
  if (natt >= 2) return;             // an attacker remains whichever piece is moved back
  bool lionFar = rowOf_[bl] == 0;    // only the lion itself can have just arrived there
  bool built = false;
  for (int i = 0; i < nm; ++i) {
    int t = mover[i], s = z[t];
    if (natt - (int)((ATT[s][t] >> wl) & 1) > 0) continue;
    if (lionFar && s != 1) continue;
    if (!built) { cx.build(z); built = true; }
    if (s >= 2 && s <= 4) {          // un-drop
      int h[HAND_TYPES] = {hw[0], hw[1], hw[2]};
      ++h[s - 2];
      emit(cx.rank1(t, 0, h));
    }
    auto from = [&](int so, int q) {
      if (z[q] || (so == 1 && rowOf_[q] == 0) || ((ATT[so][q] >> wl) & 1)) return;
      emit(cx.rank2(q, (u8)so, t, 0, hw));
      for (int type = 0; type < HAND_TYPES; ++type) {
        if (!hw[type]) continue;
        int h[HAND_TYPES] = {hw[0], hw[1], hw[2]};
        --h[type];
        emit(cx.rank2(q, (u8)so, t, (u8)(2 + type + PIECE_KINDS), h));
        if (type == 2) emit(cx.rank2(q, (u8)so, t, (u8)PIECE_STATES, h));  // captured hen
      }
    };
    if (s == 4) {                    // a chick on the far rank can only have been dropped
      if (rowOf_[t] != 0 && t + C < S) from(4, t + C);
    } else {
      for (u8 q : RMOV[s][t]) from(s, q);
      if (s == 5 && rowOf_[t] == 0) from(4, t + C);   // promotion on arrival
    }
  }
}

// ------------------------------------------------------------------------------------------
// Bulk table I/O and small bit helpers.
// ------------------------------------------------------------------------------------------
static void preadAllAt(int fd, void* dst, size_t bytes, off_t offset, const char* what) {
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, (u8*)dst + done, bytes - done, offset + (off_t)done);
    if (n <= 0) die("%s read failed", what);
    done += (size_t)n;
  }
}

static void pwriteAllAt(int fd, const void* src, size_t bytes, off_t offset,
                        const char* what) {
  size_t done = 0;
  while (done < bytes) {
    ssize_t n = pwrite(fd, (const u8*)src + done, bytes - done, offset + (off_t)done);
    if (n <= 0) die("%s write failed", what);
    done += (size_t)n;
  }
}

static void syncCurrentDirectory() {
  int fd = open(".", O_RDONLY | O_DIRECTORY);
  if (fd >= 0) { fsync(fd); close(fd); }
}

static u32 compressEvenBits(u64 x) {
  x &= 0x5555555555555555ull;
  x = (x | (x >> 1)) & 0x3333333333333333ull;
  x = (x | (x >> 2)) & 0x0f0f0f0f0f0f0f0full;
  x = (x | (x >> 4)) & 0x00ff00ff00ff00ffull;
  x = (x | (x >> 8)) & 0x0000ffff0000ffffull;
  x = (x | (x >> 16)) & 0x00000000ffffffffull;
  return (u32)x;
}

static u64 spreadEvenBits(u32 x) {
  u64 v = x;
  v = (v | (v << 16)) & 0x0000ffff0000ffffull;
  v = (v | (v << 8)) & 0x00ff00ff00ff00ffull;
  v = (v | (v << 4)) & 0x0f0f0f0f0f0f0f0full;
  v = (v | (v << 2)) & 0x3333333333333333ull;
  v = (v | (v << 1)) & 0x5555555555555555ull;
  return v;
}

// Large tables are transferred in parallel pieces. Block-aligned pieces bypass the page
// cache (O_DIRECT) when the file system allows it, so a multi-hundred-gigabyte checkpoint
// does not compete for memory with the tables it is saving.
static constexpr u64 IO_PIECE_WORDS = 1ull << 22;   // 32 MiB
static constexpr size_t IO_ALIGN = 4096;
static constexpr int IO_THREADS = 16;

template <class Fn>
static void runThreads(int n, Fn&& fn) {
  std::vector<std::thread> pool;
  for (int t = 1; t < n; ++t) pool.emplace_back([&fn, t] { fn(t); });
  fn(0);
  for (auto& th : pool) th.join();
}

// fill(firstWord, count, dst) produces the words to write.
template <class Fill>
static void bulkWriteWords(const std::string& path, off_t offset, u64 nwords, Fill&& fill) {
  int fdB = open(path.c_str(), O_WRONLY);
  if (fdB < 0) die("cannot open %s for writing", path.c_str());
  int fdD = offset % (off_t)IO_ALIGN == 0 ? open(path.c_str(), O_WRONLY | O_DIRECT) : -1;
  u64 pieces = nwords / IO_PIECE_WORDS + (nwords % IO_PIECE_WORDS != 0);
  std::atomic<u64> next{0};
  runThreads((int)std::min<u64>(IO_THREADS, std::max<u64>(pieces, 1)), [&](int) {
    void* mem = nullptr;
    if (posix_memalign(&mem, IO_ALIGN, IO_PIECE_WORDS * sizeof(u64))) die("out of memory");
    for (;;) {
      u64 p = next.fetch_add(1, std::memory_order_relaxed);
      if (p >= pieces) break;
      u64 first = p * IO_PIECE_WORDS, count = std::min(IO_PIECE_WORDS, nwords - first);
      fill(first, count, (u64*)mem);
      size_t bytes = count * sizeof(u64);
      off_t at = offset + (off_t)(first * sizeof(u64));
      if (fdD >= 0 && bytes % IO_ALIGN == 0
          && pwrite(fdD, mem, bytes, at) == (ssize_t)bytes)
        continue;
      pwriteAllAt(fdB, mem, bytes, at, "table");
    }
    free(mem);
  });
  if (fdD >= 0) close(fdD);
  if (fsync(fdB)) die("fsync failed");
  close(fdB);
}

// dst must be page-aligned (the tables are mmap()ed).
static void bulkReadWords(const std::string& path, off_t offset, u64* dst, u64 nwords) {
  int fdB = open(path.c_str(), O_RDONLY);
  if (fdB < 0) die("cannot open %s", path.c_str());
  int fdD = offset % (off_t)IO_ALIGN == 0 ? open(path.c_str(), O_RDONLY | O_DIRECT) : -1;
  u64 pieces = nwords / IO_PIECE_WORDS + (nwords % IO_PIECE_WORDS != 0);
  std::atomic<u64> next{0};
  runThreads((int)std::min<u64>(IO_THREADS, std::max<u64>(pieces, 1)), [&](int) {
    for (;;) {
      u64 p = next.fetch_add(1, std::memory_order_relaxed);
      if (p >= pieces) break;
      u64 first = p * IO_PIECE_WORDS, count = std::min(IO_PIECE_WORDS, nwords - first);
      size_t bytes = count * sizeof(u64);
      off_t at = offset + (off_t)(first * sizeof(u64));
      if (fdD >= 0 && bytes % IO_ALIGN == 0
          && pread(fdD, dst + first, bytes, at) == (ssize_t)bytes)
        continue;
      preadAllAt(fdB, dst + first, bytes, at, "table");
    }
  });
  if (fdD >= 0) close(fdD);
  close(fdB);
}

// Spread the big tables over all NUMA nodes: they are accessed at random by every thread
// and do not fit on one node at the largest board sizes. Best effort.
static void interleaveMemory() {
#if defined(__linux__) && defined(SYS_set_mempolicy)
  FILE* f = fopen("/sys/devices/system/node/online", "r");
  if (!f) return;
  char buf[256] = {0};
  bool ok = fgets(buf, sizeof buf, f) != nullptr;
  fclose(f);
  if (!ok) return;
  unsigned long mask[16] = {0};
  const int maxNode = (int)(sizeof mask * 8);
  int nodes = 0;
  for (char* p = buf; *p && *p != '\n';) {
    char* e;
    long a = strtol(p, &e, 10), b = a;
    if (e == p) return;
    if (*e == '-') { p = e + 1; b = strtol(p, &e, 10); if (e == p) return; }
    for (long i = a; i <= b && i < maxNode; ++i) {
      mask[i / (8 * sizeof(long))] |= 1ul << (i % (8 * sizeof(long)));
      ++nodes;
    }
    p = *e == ',' ? e + 1 : e;
  }
  const int MPOL_INTERLEAVE_MODE = 3;
  if (nodes > 1) syscall(SYS_set_mempolicy, MPOL_INTERLEAVE_MODE, mask, (unsigned long)maxNode);
#endif
}

static u64* mapWords(u64 nwords, const char* what) {
  if (nwords > std::numeric_limits<size_t>::max() / sizeof(u64))
    die("%s is too large for this platform", what);
  size_t bytes = std::max<size_t>(nwords * sizeof(u64), 1);
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) die("mmap of %s failed", what);
  madvise(p, bytes, MADV_HUGEPAGE);
  return (u64*)p;
}

// Prints a progress line now and then during long phases.
struct Progress {
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
  std::thread th;
  Progress(std::string what, const std::atomic<u64>& counter, u64 total, double every) {
    th = std::thread([this, what, &counter, total, every] {
      double start = now_s();
      std::unique_lock<std::mutex> lock(mu);
      while (!cv.wait_for(lock, std::chrono::duration<double>(every), [this] { return stop; })) {
        u64 done = std::min(counter.load(std::memory_order_relaxed), total);
        printf("    [%s] %.1f%% after %.0fs\n", what.c_str(),
               total ? 100.0 * (double)done / (double)total : 100.0, now_s() - start);
        fflush(stdout);
      }
    });
  }
  ~Progress() {
    { std::lock_guard<std::mutex> lock(mu); stop = true; }
    cv.notify_all();
    th.join();
  }
};

// ------------------------------------------------------------------------------------------
// Packed 2-bit WDL table (as in the NOCCA solver).
// ------------------------------------------------------------------------------------------
static constexpr size_t HDR_BYTES = 8192;
static constexpr u32 TABLE_FORMAT_V1 = 1;
static constexpr u32 TABLE_FORMAT_V2 = 2;
static constexpr u32 TABLE_FORMAT_V3 = 3;
static constexpr u32 TABLE_FLAG_REACHABILITY = 1u << 0;
static constexpr u32 TABLE_FLAG_REFLECTION_REDUCED = 1u << 1;
static constexpr int TABLE_BITS_PER_POSITION = 2;
static constexpr u64 TABLE_POSITIONS_PER_WORD =
    std::numeric_limits<u64>::digits / TABLE_BITS_PER_POSITION;
static constexpr u64 TABLE_LOW_BIT_MASK = 0x5555555555555555ull;
// Version-1 compatibility field, not a solver pass limit. Later passes remain in the text
// log and passesDone even when this optional in-header history is full.
static constexpr size_t PASS_HIST_CAPACITY = 256;
struct Hdr {
  char magic[8];       // "DOBUTWDL"
  u32 version, W, L, P;
  u64 N, nwords;
  u32 passesDone, done;
  u64 histW[PASS_HIST_CAPACITY], histL[PASS_HIST_CAPACITY];
  // Version 2/3 extension. Version 1 files have zeroes here because the 8 KiB header was
  // zero-filled; placing these fields after the legacy histograms preserves all offsets.
  u64 reachWords, reachableCount;
  u32 reachDone, reserved;
};
static_assert(sizeof(Hdr) <= HDR_BYTES, "table header exceeds its on-disk allocation");

static u64 reachWordCount(u64 n) { return n / 64 + (n % 64 != 0); }

static bool tableHasReachability(const Hdr& h) {
  return (h.version == TABLE_FORMAT_V2 || h.version == TABLE_FORMAT_V3)
      && (h.P & TABLE_FLAG_REACHABILITY) && h.reachDone == 1;
}

static bool tableIsReflectionReduced(const Hdr& h) {
  return h.version == TABLE_FORMAT_V3 && (h.P & TABLE_FLAG_REFLECTION_REDUCED);
}

static bool validTableHeader(const Hdr& h, u64 n, u64 nwords,
                             bool expectReduced = false) {
  if (memcmp(h.magic, "DOBUTWDL", 8) || h.W != (u32)C || h.L != (u32)R
      || h.N != n || h.nwords != nwords)
    return false;
  if (h.version == TABLE_FORMAT_V1)
    return !expectReduced && h.P == 0;
  if (h.version == TABLE_FORMAT_V2)
    return !expectReduced && h.P == TABLE_FLAG_REACHABILITY && h.reachDone == 1
        && h.reachWords == reachWordCount(n) && h.reachableCount <= n;
  if (h.version == TABLE_FORMAT_V3) {
    if (!expectReduced || !(h.P & TABLE_FLAG_REFLECTION_REDUCED)
        || (h.P & ~(TABLE_FLAG_REACHABILITY | TABLE_FLAG_REFLECTION_REDUCED)))
      return false;
    if (!(h.P & TABLE_FLAG_REACHABILITY))
      return h.reachDone == 0 && h.reachWords == 0 && h.reachableCount == 0;
    return h.reachDone == 1 && h.reachWords == reachWordCount(n)
        && h.reachableCount <= n;
  }
  return false;
}

struct Table {
  u64* words = nullptr;
  u64 nwords = 0, N = 0;
  Hdr hdr{};

  void alloc(u64 n) {
    N = n;
    nwords = n / TABLE_POSITIONS_PER_WORD + (n % TABLE_POSITIONS_PER_WORD != 0);
    words = mapWords(nwords, "table");
  }
  inline u32 get(u64 i) const {
    u64 w = __atomic_load_n(&words[i / TABLE_POSITIONS_PER_WORD], __ATOMIC_RELAXED);
    int shift = (int)(i % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
    return (u32)((w >> shift) & 3);
  }
  // Atomically replaces `path`. With `frontier` (one bit per position), the positions
  // whose bit is set are written as state 3; see RetroSolver's checkpoints.
  void save(const std::string& path, const u64* frontier = nullptr) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open %s failed", tmp.c_str());
    u8 hb[HDR_BYTES]; memset(hb, 0, sizeof hb);
    memcpy(hb, &hdr, sizeof hdr);
    pwriteAllAt(fd, hb, HDR_BYTES, 0, "header");
    close(fd);
    const u64* src = words;
    bulkWriteWords(tmp, (off_t)HDR_BYTES, nwords,
                   [src, frontier](u64 first, u64 count, u64* dst) {
      if (!frontier) { memcpy(dst, src + first, count * sizeof(u64)); return; }
      for (u64 i = 0; i < count; ++i) {
        u64 wi = first + i;
        u64 m = spreadEvenBits((u32)(frontier[wi >> 1] >> (32 * (wi & 1))));
        dst[i] = src[wi] | m | (m << 1);
      }
    });
    if (rename(tmp.c_str(), path.c_str())) die("rename failed");
    syncCurrentDirectory();
  }
  bool load(const std::string& path, u64 n, bool reduced = false) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    u8 hb[HDR_BYTES];
    preadAllAt(fd, hb, HDR_BYTES, 0, "header");
    close(fd);
    memcpy(&hdr, hb, sizeof hdr);
    if (!validTableHeader(hdr, n, nwords, reduced))
      die("table mismatch");
    bulkReadWords(path, (off_t)HDR_BYTES, words, nwords);
    return true;
  }
};

// ------------------------------------------------------------------------------------------
// Solver: shared setup plus the original forward-iteration algorithm ('solve-forward'),
// which re-evaluates every undecided position in every pass through the ZDD ranks.
// ------------------------------------------------------------------------------------------
struct Solver {
  Zdd Z; Table T;
  bool reflectionReduced = false;
  int threads = (int)std::max(1u, std::thread::hardware_concurrency());
  std::string tablePath;
  u64 initIdx = 0;
  std::atomic<u64> chunkCtr{0};
  static const u64 CHUNK = 1ull << 20;

  explicit Solver(bool reduced = false) : Z(reduced), reflectionReduced(reduced) {}

  static void initialPosition(u8* arr) {
    memset(arr, 0, MAXARR);
    arr[0 * C + 0] = 7; arr[0 * C + 1] = 6; arr[0 * C + 2] = 8;             // white E L G
    arr[1 * C + 1] = 9;                                                      // white C
    arr[(R - 2) * C + 1] = 4;                                                // black C
    arr[(R - 1) * C + 0] = 3; arr[(R - 1) * C + 1] = 1; arr[(R - 1) * C + 2] = 2;  // black G L E
  }

  void setup(const std::string& path) {
    tablePath = path;
    double t0 = now_s();
    Z.build();
    u64 dp = Zdd::dpCount();
    u64 fixed = reflectionReduced ? Zdd::dpMirrorFixedCount() : 0;
    u64 expected = reflectionReduced ? (dp + fixed) / 2 : dp;
    printf("[zdd] nodes=%s (incl. 2 leaves), |set|=%s, %s crosscheck=%s (%s), %.2fs\n",
           commas(Z.idx_.size()).c_str(), commas(Z.total).c_str(),
           reflectionReduced ? "orbit-count" : "DP",
           commas(expected).c_str(), Z.total == expected ? "OK" : "MISMATCH!", now_s() - t0);
    if (reflectionReduced)
      printf("[zdd] full=%s mirror-fixed=%s reflection orbits=%s\n",
             commas(dp).c_str(), commas(fixed).c_str(), commas(expected).c_str());
    if (Z.total != expected) die("ZDD count crosscheck failed");
    u8 arr[MAXARR], scratch[MAXARR];
    initialPosition(arr);
    initIdx = Z.rankCanonical(arr, scratch);
    if (initIdx == UINT64_MAX) die("initial position not in set");
    printf("[zdd] initial position index = %s\n", commas(initIdx).c_str());
    T.alloc(Z.total);
    memcpy(T.hdr.magic, "DOBUTWDL", 8);
    T.hdr.version = reflectionReduced ? TABLE_FORMAT_V3 : TABLE_FORMAT_V1;
    T.hdr.W = C; T.hdr.L = R;
    T.hdr.P = reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0;
    T.hdr.N = Z.total; T.hdr.nwords = T.nwords;
  }

  struct EvalWorkspace {
    u8 fpos[MAXARR], child[MAXARR];
    u8 mirrorBase[MAXARR], mirrorChild[MAXARR];
    u32 nodeAt[MAXARR + 1], mirrorNodeAt[MAXARR + 1];
    u64 kAt[MAXARR + 1], mirrorKAt[MAXARR + 1];
  };

  inline u64 childRank(const EvalWorkspace& w, int firstChanged,
                       int mirrorFirstChanged) const {
    if (!reflectionReduced)
      return Z.rankFrom(w.nodeAt[firstChanged], w.kAt[firstChanged], w.child);
    if (compareReflection(w.child) <= 0)
      return Z.rankFrom(w.nodeAt[firstChanged], w.kAt[firstChanged], w.child);
    return Z.rankFrom(w.mirrorNodeAt[mirrorFirstChanged],
                      w.mirrorKAt[mirrorFirstChanged], w.mirrorChild);
  }

  // Children evaluation for pass >= 2. Returns 1 win, 2 loss, 0 unknown.
  inline int passKEval(const u8* arr, EvalWorkspace& w) const {
    int hb[HAND_TYPES], hw[HAND_TYPES];
    handCounts(arr, hb);
    whiteHand(arr, hb, hw);
    buildFlip(arr, hw, w.fpos);
    Z.buildPrefix(w.fpos, w.nodeAt, w.kAt);
    memcpy(w.child, w.fpos, ARR);
    if (reflectionReduced) {
      mirrorPosition(w.fpos, w.mirrorBase);
      Z.buildPrefix(w.mirrorBase, w.mirrorNodeAt, w.mirrorKAt);
      memcpy(w.mirrorChild, w.mirrorBase, ARR);
    }
    bool allWin = true;
    int nchild = 0;
    for (int q = 0; q < S; ++q) {
      int s = arr[q];
      if (s < 1 || s > 5) continue;
      int f1 = flipSq_[q];
      for (u8 t : MOV[s][q]) {
        int tt = arr[t];
        if (tt >= 1 && tt <= 6) continue;           // own piece or white lion
        int ns = (s == 4 && rowOf_[t] == 0) ? 5 : s;
        int f2 = flipSq_[t];
        w.child[f1] = 0; w.child[f2] = (u8)(ns + 5);
        int c1 = f1 < f2 ? f1 : f2;
        int mc1 = 0;
        if (reflectionReduced) {
          int mf1 = mirSq_[f1], mf2 = mirSq_[f2];
          w.mirrorChild[mf1] = 0; w.mirrorChild[mf2] = (u8)(ns + 5);
          mc1 = std::min(mf1, mf2);
        }
        u64 k = childRank(w, c1, mc1);
        w.child[f1] = w.fpos[f1]; w.child[f2] = w.fpos[f2];
        if (reflectionReduced) {
          int mf1 = mirSq_[f1], mf2 = mirSq_[f2];
          w.mirrorChild[mf1] = w.mirrorBase[mf1];
          w.mirrorChild[mf2] = w.mirrorBase[mf2];
        }
        if (k == UINT64_MAX) die("child rank failed — logic bug");
        u32 v = T.get(k);
        ++nchild;
        if (v == 2) return 1;
        if (v != 1) allWin = false;
      }
    }
    for (int t3 = 0; t3 < HAND_TYPES; ++t3) {
      if (!hb[t3]) continue;
      u8 dropped = (u8)(2 + t3 + 5);                 // after flip: white E/G/C
      for (int q = 0; q < S; ++q) {
        if (arr[q]) continue;
        int f1 = flipSq_[q];
        w.child[f1] = dropped;
        int mf1 = 0;
        if (reflectionReduced) {
          mf1 = mirSq_[f1];
          w.mirrorChild[mf1] = dropped;
        }
        u64 k = childRank(w, f1, mf1);
        w.child[f1] = w.fpos[f1];
        if (reflectionReduced) w.mirrorChild[mf1] = w.mirrorBase[mf1];
        if (k == UINT64_MAX) die("drop child rank failed — logic bug");
        u32 v = T.get(k);
        ++nchild;
        if (v == 2) return 1;
        if (v != 1) allWin = false;
      }
    }
    if (!nchild) return 0;                           // stalemate: draw by convention
    return allWin ? 2 : 0;
  }

  void worker(int pass, u64* cw, u64* cl) {
    EvalWorkspace workspace;
    ZddIter it;
    u64 lw = 0, ll = 0;
    for (;;) {
      u64 c = chunkCtr.fetch_add(1, std::memory_order_relaxed);
      u64 lo = c * CHUNK;
      if (lo >= Z.total) break;
      u64 hi = std::min(lo + CHUNK, Z.total);
      it.initAt(Z, lo);
      u64 idx = lo;
      while (idx < hi) {
        u64 wi = idx / TABLE_POSITIONS_PER_WORD;
        u64 spanEnd = std::min(hi, (wi + 1) * TABLE_POSITIONS_PER_WORD);
        u64 w = __atomic_load_n(&T.words[wi], __ATOMIC_RELAXED);
        u64 add = 0;
        for (; idx < spanEnd; ++idx) {
          int sh = (int)(idx % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
          if (((w >> sh) & 3) == 0) {
            int r = (pass == 1) ? pass1Eval(it.pos)
                                : passKEval(it.pos, workspace);
            if (r) {
              add |= (pass == 1 ? (u64)r : 3ull) << sh;
              if (r == 1) ++lw; else ++ll;
            }
          }
          if (idx + 1 < hi) { bool ok = it.next(); (void)ok; assert(ok); }
        }
        if (add) __atomic_store_n(&T.words[wi], w | add, __ATOMIC_RELAXED);
      }
    }
    *cw = lw; *cl = ll;
  }

  u64 sweepConvert(u32 val) {
    std::atomic<u64> cnt{0};
    std::vector<std::thread> th;
    u64 per = (T.nwords + threads - 1) / threads;
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&, t] {
        u64 b = t * per, e = std::min(T.nwords, b + per), c = 0;
        for (u64 i = b; i < e; ++i) {
          u64 w = T.words[i];
          u64 m = w & (w >> 1) & TABLE_LOW_BIT_MASK;
          if (!m) continue;
          c += __builtin_popcountll(m);
          T.words[i] = (val == 1) ? (w & ~(m << 1)) : (w & ~m);
        }
        cnt += c;
      });
    for (auto& x : th) x.join();
    return cnt.load();
  }

  void tally(u64& wins, u64& losses, u64& unk) {
    u64 cw = 0, cl = 0;
    for (u64 i = 0; i < T.nwords; ++i) {
      u64 w = T.words[i];
      u64 hiB = (w >> 1) & TABLE_LOW_BIT_MASK, loB = w & TABLE_LOW_BIT_MASK;
      cw += __builtin_popcountll(loB & ~hiB);
      cl += __builtin_popcountll(hiB & ~loB);
    }
    wins = cw; losses = cl; unk = Z.total - cw - cl;
  }

  void solve(int ckptEvery) {
    bool resumed = T.load(tablePath, Z.total, reflectionReduced);
    int startPass = resumed ? (int)T.hdr.passesDone + 1 : 1;
    if (resumed) {
      if (T.hdr.version == TABLE_FORMAT_V2 && !T.hdr.done)
        die("cannot resume an incomplete solve after a K plane was attached");
      if (T.hdr.done) { printf("[solve] table already complete.\n"); return; }
      printf("[solve] resuming after pass %u\n", T.hdr.passesDone);
    }
    for (int pass = startPass;; ++pass) {
      double t0 = now_s();
      chunkCtr = 0;
      std::vector<std::thread> th;
      std::vector<u64> cw(threads, 0), cl(threads, 0);
      for (int t = 0; t < threads; ++t)
        th.emplace_back([&, t] { worker(pass, &cw[t], &cl[t]); });
      for (auto& x : th) x.join();
      u64 nw = 0, nl = 0;
      for (int t = 0; t < threads; ++t) { nw += cw[t]; nl += cl[t]; }
      if (pass > 1 && nw && nl)
        die("pass %d found both wins and losses — parity broken", pass);
      if (pass > 1 && nw + nl) {
        u64 conv = sweepConvert(nw ? 1 : 2);
        if (conv != nw + nl) die("sweep mismatch");
      }
      if ((size_t)pass < PASS_HIST_CAPACITY) {
        T.hdr.histW[pass] = nw;
        T.hdr.histL[pass] = nl;
      }
      printf("[pass %3d] wins +%-13s losses +%-13s  %6.1fs\n",
             pass, commas(nw).c_str(), commas(nl).c_str(), now_s() - t0);
      fflush(stdout);
      u32 iv = T.get(initIdx);
      static bool reported = false;
      if ((iv == 1 || iv == 2) && !reported) {
        reported = true;
        printf("[solve] *** initial position decided at pass %d: %s for the first player (Sente) ***\n",
               pass, iv == 1 ? "WIN" : "LOSS");
        fflush(stdout);
      }
      bool finished = (pass > 1 && nw + nl == 0);
      T.hdr.passesDone = pass;
      if (finished) T.hdr.done = 1;
      if (finished || pass % ckptEvery == 0) T.save(tablePath);
      if (finished) break;
    }
    u64 wins, losses, draws;
    tally(wins, losses, draws);
    printf("[done] pseudo-reachable totals: wins=%s losses=%s draws=%s total=%s\n",
           commas(wins).c_str(), commas(losses).c_str(), commas(draws).c_str(),
           commas(wins + losses + draws).c_str());
    u32 iv = T.get(initIdx);
    printf("[done] initial position: %s\n",
           iv == 1 ? "WIN for Sente (first player)" :
           iv == 2 ? "LOSS for Sente — GOTE (second player) WINS" : "DRAW");
    fflush(stdout);
  }
};

// ------------------------------------------------------------------------------------------
// Frontier-driven retrograde solver ('solve').
//
// Passes are numbered exactly as in the forward solver, and every pass decides exactly the
// same positions, so the resulting table and its per-pass history are identical.
//
//   pass 1       intrinsic values (pass1Eval), evaluated once per board.
//   pass 2       every undecided position whose children are all pass-1 wins is a loss.
//   odd  k >= 3  every undecided predecessor of a loss decided in pass k-1 is a win.
//   even k >= 4  an undecided predecessor of a win decided in pass k-1 is a loss if all
//                its children are wins.
//
// Besides the two-bit table, one bit per position (the plane F) marks the positions
// decided in the previous pass. A pass first expands those positions through genPreds and
// marks their undecided predecessors with the transient state 3; a second sweep turns the
// marks into wins, or verifies them (all children won) into losses. A position that is
// still undecided cannot have a lost child from an earlier pass — it would have been
// marked a win one pass after that child — so "all children decided as wins" is the whole
// loss test, and marks of the current pass (state 3 or a fresh loss) count as "not won".
//
// Checkpoints are ordinary tables in which the positions decided by the last completed
// pass are stored as state 3; their value follows from the parity of that pass.
// ------------------------------------------------------------------------------------------
static constexpr u32 CKPT_FRONTIER_MARKED = 1;   // Hdr::reserved of such a checkpoint

struct RetroSolver {
  Solver& sv;
  Table& T;
  SigRank SR;
  u64 N = 0;
  int threads;
  u64* F = nullptr;
  u64 fwords = 0;
  double progressEvery = 600.0;

  static constexpr u64 CHUNK = 1ull << 18;   // positions per work unit; a multiple of 64
  static constexpr int SLOTS = 12;           // candidates verified concurrently per thread
  static constexpr u32 RING = 64;            // marks delayed behind their prefetch

  explicit RetroSolver(Solver& s) : sv(s), T(s.T), threads(s.threads) {}

  void setup() {
    N = sv.Z.total;
    SR.build(sv.reflectionReduced);
    if (SR.total != N) die("signature-rank table count does not match the ZDD");
    fwords = reachWordCount(N);
    F = mapWords(fwords, "frontier plane");
  }

  static inline u64 load(const u64* p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
  static inline void store(u64* p, u64 v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
  static inline u64 mask3(u64 w) { return w & (w >> 1) & TABLE_LOW_BIT_MASK; }
  static inline u64 mask0(u64 w) { return ~(w | (w >> 1)) & TABLE_LOW_BIT_MASK; }

  bool claim(std::atomic<u64>& next, u64& lo, u64& hi) const {
    u64 c = next.fetch_add(1, std::memory_order_relaxed);
    if (c > N / CHUNK) return false;
    lo = c * CHUNK;
    if (lo >= N) return false;
    hi = std::min(lo + CHUNK, N);
    return true;
  }
  u64 chunkCount() const { return N / CHUNK + (N % CHUNK != 0); }

  // ---- pass 1 --------------------------------------------------------------------------
  void setRange(u64 k, u64 e, u64 v) {
    while (k < e) {
      u64 wi = k / TABLE_POSITIONS_PER_WORD;
      u64 stop = std::min(e, (wi + 1) * TABLE_POSITIONS_PER_WORD), span = stop - k;
      u64 bits = span == TABLE_POSITIONS_PER_WORD ? ~0ull : (1ull << (2 * span)) - 1;
      T.words[wi] |= ((bits << (2 * (k % TABLE_POSITIONS_PER_WORD))) & TABLE_LOW_BIT_MASK) * v;
      k = stop;
    }
  }

  void pass1(u64& wins, u64& losses) {
    std::atomic<u64> next{0}, cw{0}, cl{0};
    Progress progress("pass 1", next, chunkCount(), progressEvery);
    runThreads(threads, [&](int) {
      SigIter it;
      it.init(SR);
      u64 lw = 0, ll = 0, lo, hi;
      while (claim(next, lo, hi)) {
        for (u64 k = lo; k < hi;) {
          it.seek(k);
          u64 e = std::min(hi, it.kend[S]);     // all hands of this board share its value
          int v = pass1Eval(it.pos);
          if (v) { setRange(k, e, (u64)v); (v == 1 ? lw : ll) += e - k; }
          k = e;
        }
      }
      cw += lw; cl += ll;
    });
    wins = cw.load(); losses = cl.load();
  }

  // ---- expansion of the previous pass's decisions --------------------------------------
  void expand(const char* label) {
    std::atomic<u64> next{0};
    Progress progress(label, next, chunkCount(), progressEvery);
    runThreads(threads, [&](int) {
      SigIter it;
      it.init(SR);
      RankCtx cx;
      cx.sr = &SR;
      u64 ring[RING];
      u32 head = 0, count = 0;
      auto mark = [&](u64 r) {
        u64* p = &T.words[r / TABLE_POSITIONS_PER_WORD];
        int sh = (int)(r % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
        // Only 0 -> 3 transitions happen in this phase, so a plain OR is exact.
        if (((load(p) >> sh) & 3) == 0) __atomic_fetch_or(p, 3ull << sh, __ATOMIC_RELAXED);
      };
      auto emit = [&](u64 r) {
        __builtin_prefetch(&T.words[r / TABLE_POSITIONS_PER_WORD], 1, 3);
        if (count == RING) {
          mark(ring[head]);
          ring[head] = r;
          head = (head + 1) % RING;
        } else {
          ring[(head + count++) % RING] = r;
        }
      };
      u64 lo, hi;
      while (claim(next, lo, hi)) {
        for (u64 fi = lo / 64, fe = (hi + 63) / 64; fi < fe; ++fi) {
          u64 w = F[fi];
          if (!w) continue;
          F[fi] = 0;
          do {
            int b = __builtin_ctzll(w);
            w &= w - 1;
            it.seek(fi * 64 + (u64)b);
            genPreds(it.pos, cx, emit);
          } while (w);
        }
      }
      for (u32 i = 0; i < count; ++i) mark(ring[(head + i) % RING]);
    });
  }

  // ---- win pass: every mark becomes a win ----------------------------------------------
  u64 convertWins() {
    std::atomic<u64> next{0}, total{0};
    runThreads(threads, [&](int) {
      u64 lo, hi, local = 0;
      while (claim(next, lo, hi)) {
        for (u64 fi = lo / 64, fe = (hi + 63) / 64; fi < fe; ++fi) {
          u64 bits = 0;
          for (u64 h = 0; h < 2 && 2 * fi + h < T.nwords; ++h) {
            u64 w = T.words[2 * fi + h], m = mask3(w);
            if (!m) continue;
            T.words[2 * fi + h] = w ^ (m << 1);            // 3 -> 1
            bits |= (u64)compressEvenBits(m) << (32 * h);
          }
          if (bits) { F[fi] = bits; local += (u64)__builtin_popcountll(bits); }
        }
      }
      total += local;
    });
    return total.load();
  }

  // ---- loss pass: verify candidates ----------------------------------------------------
  // Candidates are the positions in state `cand`: 3 (marked by expand) or, for pass 2, 0
  // (every undecided position). A candidate is a loss if it has a move and every child is
  // a win. Each thread works on several candidates in turn; a candidate has one child
  // lookup in flight, prefetched a full turn before it is read, and the next child is
  // ranked only once the previous one turned out to be a win — most candidates are
  // refuted by their first or second child.
  struct Slot {
    u64 rank, kid;           // the candidate and its child being looked up
    int next;                // index of the next move to rank
    bool active;
    RankCtx cx;
    MoveList ml;
  };

  u64 verify(u32 cand, const char* label) {
    std::atomic<u64> next{0}, found{0};
    Progress progress(label, next, chunkCount(), progressEvery);
    runThreads(threads, [&](int) {
      SigIter it;
      it.init(SR);
      std::vector<Slot> slots(SLOTS);
      for (Slot& s : slots) { s.active = false; s.cx.sr = &SR; }
      u64 lo = 0, hi = 0, wi = 0, wend = 0, cur = 0, mask = 0, local = 0;
      bool more = true;
      int active = 0;
      auto pull = [&](u64& rank) {
        for (;;) {
          while (mask) {
            int b = __builtin_ctzll(mask);
            mask &= mask - 1;
            rank = cur * TABLE_POSITIONS_PER_WORD + (u64)(b >> 1);
            if (rank < N) return true;           // skip the padding of the last word
          }
          if (wi == wend) {
            if (!claim(next, lo, hi)) return false;
            wi = lo / TABLE_POSITIONS_PER_WORD;
            wend = (hi + TABLE_POSITIONS_PER_WORD - 1) / TABLE_POSITIONS_PER_WORD;
          }
          u64 w = load(&T.words[wi]);
          mask = cand == 3 ? mask3(w) : mask0(w);
          cur = wi++;
        }
      };
      // Only the thread that claimed a chunk writes its words during this phase.
      auto setField = [&](u64 rank, u64 val) {
        u64* p = &T.words[rank / TABLE_POSITIONS_PER_WORD];
        int sh = (int)(rank % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
        store(p, (load(p) & ~(3ull << sh)) | (val << sh));
      };
      auto resolve = [&](u64 rank, bool loss) {
        if (loss) { setField(rank, 2); F[rank / 64] |= 1ull << (rank % 64); ++local; }
        else if (cand == 3) setField(rank, 0);
      };
      auto launch = [&](Slot& s) {
        s.kid = rankMove(s.cx, s.ml, s.next++);
        __builtin_prefetch(&T.words[s.kid / TABLE_POSITIONS_PER_WORD], 0, 3);
      };
      for (int c = 0; more || active; c = c + 1 == SLOTS ? 0 : c + 1) {
        Slot& s = slots[c];
        if (s.active) {
          // Marks and fresh losses of this pass are not wins, so "!= 1" refutes.
          if (T.get(s.kid) != 1) resolve(s.rank, false);
          else if (s.next < s.ml.n) { launch(s); continue; }
          else resolve(s.rank, true);
          s.active = false;
          --active;
        }
        while (more) {
          u64 rank;
          if (!pull(rank)) { more = false; break; }
          it.seek(rank);
          listMoves(it.pos, true, s.ml);
          if (!s.ml.n) {                 // every move loses the lion at once, or stalemate
            resolve(rank, s.ml.any);
            continue;
          }
          s.rank = rank; s.next = 0; s.active = true;
          ++active;
          s.cx.build(s.ml.f);
          launch(s);
          break;
        }
      }
      found += local;
    });
    return found.load();
  }

  // ---- checkpoints ---------------------------------------------------------------------
  // Rebuild F from a frontier-marked checkpoint and give the marked positions their value.
  u64 restoreFrontier(u32 pass) {
    std::atomic<u64> next{0}, total{0};
    runThreads(threads, [&](int) {
      u64 lo, hi, local = 0;
      while (claim(next, lo, hi)) {
        for (u64 fi = lo / 64, fe = (hi + 63) / 64; fi < fe; ++fi) {
          u64 bits = 0;
          for (u64 h = 0; h < 2 && 2 * fi + h < T.nwords; ++h) {
            u64 w = T.words[2 * fi + h], m = mask3(w);
            if (!m) continue;
            T.words[2 * fi + h] = w ^ (pass % 2 ? m << 1 : m);   // 3 -> 1 or 3 -> 2
            bits |= (u64)compressEvenBits(m) << (32 * h);
          }
          if (bits) { F[fi] = bits; local += (u64)__builtin_popcountll(bits); }
        }
      }
      total += local;
    });
    return total.load();
  }

  void checkpoint(int pass) {
    double t0 = now_s();
    T.hdr.reserved = CKPT_FRONTIER_MARKED;
    T.save(sv.tablePath, pass >= 2 ? F : nullptr);
    T.hdr.reserved = 0;
    printf("[solve] checkpoint after pass %d written in %.1fs\n", pass, now_s() - t0);
    fflush(stdout);
  }

  void solve(double ckptMinutes) {
    bool resumed = T.load(sv.tablePath, N, sv.reflectionReduced);
    int startPass = 1;
    if (resumed) {
      if (T.hdr.version == TABLE_FORMAT_V2 && !T.hdr.done)
        die("cannot resume an incomplete solve after a K plane was attached");
      if (T.hdr.done) { printf("[solve] table already complete.\n"); return; }
      if (T.hdr.passesDone >= 2 && T.hdr.reserved != CKPT_FRONTIER_MARKED)
        die("this checkpoint was written by solve-forward; resume it with solve-forward");
      startPass = (int)T.hdr.passesDone + 1;
      printf("[solve] resuming after pass %u\n", T.hdr.passesDone);
      if (T.hdr.passesDone >= 2) {
        u32 p = T.hdr.passesDone;
        u64 n = restoreFrontier(p);
        if (p < PASS_HIST_CAPACITY && n != T.hdr.histW[p] + T.hdr.histL[p])
          die("checkpoint frontier does not match its recorded pass history");
        printf("[solve] restored %s positions decided in pass %u\n", commas(n).c_str(), p);
      }
      T.hdr.reserved = 0;
      fflush(stdout);
    }
    double lastCkpt = now_s();
    bool reported = false;
    for (int pass = startPass;; ++pass) {
      double t0 = now_s(), tExpand = 0;
      u64 nw = 0, nl = 0;
      char label[32];
      if (pass == 1) {
        pass1(nw, nl);
      } else if (pass == 2) {
        nl = verify(0, "pass 2");
      } else {
        snprintf(label, sizeof label, "pass %d expand", pass);
        expand(label);
        tExpand = now_s() - t0;
        if (pass % 2) nw = convertWins();
        else {
          snprintf(label, sizeof label, "pass %d verify", pass);
          nl = verify(3, label);
        }
      }
      if (pass > 1 && nw && nl)
        die("pass %d found both wins and losses — parity broken", pass);
      if ((size_t)pass < PASS_HIST_CAPACITY) {
        T.hdr.histW[pass] = nw;
        T.hdr.histL[pass] = nl;
      }
      printf("[pass %3d] wins +%-13s losses +%-13s  %6.1fs", pass, commas(nw).c_str(),
             commas(nl).c_str(), now_s() - t0);
      if (pass > 2) printf("  (expand %.1fs)", tExpand);
      printf("\n");
      u32 iv = T.get(sv.initIdx);
      if ((iv == 1 || iv == 2) && !reported) {
        reported = true;
        printf("[solve] *** initial position decided by pass %d: %s for the first player (Sente) ***\n",
               pass, iv == 1 ? "WIN" : "LOSS");
      }
      fflush(stdout);
      bool finished = pass > 1 && nw + nl == 0;
      T.hdr.passesDone = (u32)pass;
      if (finished) {
        T.hdr.done = 1;
        T.save(sv.tablePath);
        break;
      }
      if (now_s() - lastCkpt >= ckptMinutes * 60.0) {
        checkpoint(pass);
        lastCkpt = now_s();
      }
    }
    u64 wins, losses, draws;
    sv.tally(wins, losses, draws);
    printf("[done] pseudo-reachable totals: wins=%s losses=%s draws=%s total=%s\n",
           commas(wins).c_str(), commas(losses).c_str(), commas(draws).c_str(),
           commas(wins + losses + draws).c_str());
    u32 iv = T.get(sv.initIdx);
    printf("[done] initial position: %s\n",
           iv == 1 ? "WIN for Sente (first player)" :
           iv == 2 ? "LOSS for Sente — GOTE (second player) WINS" : "DRAW");
    fflush(stdout);
  }
};

// ------------------------------------------------------------------------------------------
// Forward reachability. The finished K plane is one bit per rank and is appended to a
// completed WDL table. The breadth-first traversal keeps a two-bit state per rank in
// memory: 0 = unseen, 1 = reached and expanded, 2 = frontier of the current level,
// 3 = found during the current level. Level boundaries are checkpointed to
// reach_work_*.bin, so an interrupted traversal resumes from its last checkpoint.
// ------------------------------------------------------------------------------------------
static constexpr size_t REACH_WORK_HDR_BYTES = 4096;

struct ReachWorkHdr {
  char magic[8];                    // "DOBURCH2"
  u32 W, L, flags, level;
  u64 N, stateWords, reachableCount, frontierCount;
};
static_assert(sizeof(ReachWorkHdr) <= REACH_WORK_HDR_BYTES);

class Reachability {
  Zdd Z;
  SigRank SR;
  bool reflectionReduced;
  int threads;
  double ckptMinutes;
  std::string tablePath, workPath;
  u64* states = nullptr;
  u64 N = 0, stateWords = 0;
  u32 level = 0;
  u64 reachable = 0, frontier = 0;

  static constexpr u64 CHUNK = 1ull << 18;
  static constexpr u32 RING = 64;

  u64 expectedReachableCount() const {
    // This solver's ZDD ranks both left/right orientations separately. The published
    // 246,803,167 count is after reflect_lr canonicalization; the corresponding full-rank
    // 4x3 set has 493,573,042 positions (33,292 of the published orbits are fixed mirrors).
    // The 5x3 reflection-orbit count comes from the independently validated Rust traversal.
    if (R == 4) return reflectionReduced ? 246803167ull : 493573042ull;
    if (R == 5 && reflectionReduced) return 3359910526ull;
    return 0;
  }

  u32 workFlags() const { return reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0; }

  void expandLevel() {
    std::atomic<u64> next{0};
    char label[32];
    snprintf(label, sizeof label, "reach level %u", level + 1);
    Progress progress(label, next, N / CHUNK + 1, 600.0);
    runThreads(threads, [&](int) {
      SigIter it;
      it.init(SR);
      RankCtx cx;
      cx.sr = &SR;
      u64 kids[MAX_KIDS], ring[RING];
      u32 head = 0, count = 0;
      auto mark = [&](u64 r) {
        u64* p = &states[r / TABLE_POSITIONS_PER_WORD];
        int sh = (int)(r % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
        // Only 0 -> 3 transitions happen during an expansion, so a plain OR is exact.
        if (((__atomic_load_n(p, __ATOMIC_RELAXED) >> sh) & 3) == 0)
          __atomic_fetch_or(p, 3ull << sh, __ATOMIC_RELAXED);
      };
      auto emit = [&](u64 r) {
        __builtin_prefetch(&states[r / TABLE_POSITIONS_PER_WORD], 1, 3);
        if (count == RING) {
          mark(ring[head]);
          ring[head] = r;
          head = (head + 1) % RING;
        } else {
          ring[(head + count++) % RING] = r;
        }
      };
      for (;;) {
        u64 lo = next.fetch_add(1, std::memory_order_relaxed) * CHUNK;
        if (lo >= N) break;
        u64 hi = std::min(lo + CHUNK, N);
        for (u64 wi = lo / TABLE_POSITIONS_PER_WORD,
                 we = (hi + TABLE_POSITIONS_PER_WORD - 1) / TABLE_POSITIONS_PER_WORD;
             wi < we; ++wi) {
          u64 w = __atomic_load_n(&states[wi], __ATOMIC_RELAXED);
          u64 m = (w >> 1) & ~w & TABLE_LOW_BIT_MASK;          // fields equal to 2
          while (m) {
            int b = __builtin_ctzll(m);
            m &= m - 1;
            it.seek(wi * TABLE_POSITIONS_PER_WORD + (u64)(b >> 1));
            if (gameAlreadyOver(it.pos)) continue;
            bool complete, any;
            int n = genChildren(it.pos, cx, kids, MAX_KIDS, false, complete, any);
            if (!complete) die("more than %d children", MAX_KIDS);
            for (int j = 0; j < n; ++j) emit(kids[j]);
          }
        }
      }
      for (u32 i = 0; i < count; ++i) mark(ring[(head + i) % RING]);
    });
  }

  // Frontier (2) becomes reached (1); newly found (3) becomes the next frontier (2).
  u64 commitLevel() {
    std::atomic<u64> found{0};
    u64 per = stateWords / (u64)threads + 1;
    runThreads(threads, [&](int t) {
      u64 begin = std::min(stateWords, (u64)t * per), end = std::min(stateWords, begin + per);
      u64 local = 0;
      for (u64 i = begin; i < end; ++i) {
        u64 w = states[i];
        u64 high = (w >> 1) & TABLE_LOW_BIT_MASK, low = w & TABLE_LOW_BIT_MASK;
        u64 f2 = high & ~low, f3 = high & low;
        if (!(f2 | f3)) continue;
        states[i] = w ^ (f2 * 3) ^ f3;
        local += (u64)__builtin_popcountll(f3);
      }
      found += local;
    });
    return found.load();
  }

  void saveWork() {
    double t0 = now_s();
    std::string tmp = workPath + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("cannot create %s", tmp.c_str());
    u8 hb[REACH_WORK_HDR_BYTES];
    memset(hb, 0, sizeof hb);
    ReachWorkHdr h{};
    memcpy(h.magic, "DOBURCH2", 8);
    h.W = C; h.L = R; h.flags = workFlags(); h.level = level;
    h.N = N; h.stateWords = stateWords; h.reachableCount = reachable;
    h.frontierCount = frontier;
    memcpy(hb, &h, sizeof h);
    pwriteAllAt(fd, hb, sizeof hb, 0, "reach work header");
    close(fd);
    const u64* src = states;
    bulkWriteWords(tmp, (off_t)REACH_WORK_HDR_BYTES, stateWords,
                   [src](u64 first, u64 count, u64* dst) {
                     memcpy(dst, src + first, count * sizeof(u64));
                   });
    if (rename(tmp.c_str(), workPath.c_str())) die("reach work rename failed");
    syncCurrentDirectory();
    printf("[reach] checkpoint after level %u written in %.1fs\n", level, now_s() - t0);
    fflush(stdout);
  }

  bool loadWork() {
    int fd = open(workPath.c_str(), O_RDONLY);
    if (fd < 0) return false;
    u8 hb[REACH_WORK_HDR_BYTES];
    preadAllAt(fd, hb, sizeof hb, 0, "reach work header");
    close(fd);
    ReachWorkHdr h{};
    memcpy(&h, hb, sizeof h);
    if (!memcmp(h.magic, "DOBURCHW", 8))
      die("%s was written by the older disk-based traversal; delete it and the "
          "reach_frontier_* files, then run 'reach' again", workPath.c_str());
    if (memcmp(h.magic, "DOBURCH2", 8) || h.W != (u32)C || h.L != (u32)R
        || h.flags != workFlags() || h.N != N || h.stateWords != stateWords)
      die("reach work file does not match this board/code");
    bulkReadWords(workPath, (off_t)REACH_WORK_HDR_BYTES, states, stateWords);
    level = h.level; reachable = h.reachableCount; frontier = h.frontierCount;
    printf("[reach] resuming after level %u: %s reached, frontier %s\n", level,
           commas(reachable).c_str(), commas(frontier).c_str());
    return true;
  }

  void finalizeTable() {
    u64 expected = expectedReachableCount();
    if (expected && reachable != expected)
      die("reachable count mismatch: got %s expected %s; table was not modified",
          commas(reachable).c_str(), commas(expected).c_str());

    int fd = open(tablePath.c_str(), O_RDWR);
    if (fd < 0) die("cannot open completed WDL table %s", tablePath.c_str());
    u8 headerBytes[HDR_BYTES];
    preadAllAt(fd, headerBytes, HDR_BYTES, 0, "WDL header");
    Hdr h{}; memcpy(&h, headerBytes, sizeof h);
    if (!validTableHeader(h, N, stateWords, reflectionReduced) || !h.done)
      die("reachability requires a completed matching WDL table");
    if (tableHasReachability(h)) {
      if (h.reachableCount != reachable) die("existing K-plane count mismatch");
      close(fd);
      printf("[reach] WDL table already contains a validated K plane\n");
      return;
    }

    u64 kwords = reachWordCount(N);
    off_t reachOffset = (off_t)HDR_BYTES + (off_t)(stateWords * sizeof(u64));
    off_t finalBytes = reachOffset + (off_t)(kwords * sizeof(u64));
    struct stat st{};
    if (fstat(fd, &st)) die("WDL table stat failed");
    if (st.st_size < reachOffset) die("WDL table is truncated");
    if (st.st_size != reachOffset)
      printf("[reach] replacing an incomplete trailing K plane\n");
    if (ftruncate(fd, finalBytes)) die("WDL+K table truncate failed");

    std::atomic<u64> setBits{0};
    const u64* src = states;
    const u64 words = stateWords;
    bulkWriteWords(tablePath, reachOffset, kwords,
                   [&setBits, src, words](u64 first, u64 count, u64* dst) {
      u64 local = 0;
      for (u64 i = 0; i < count; ++i) {
        u64 j = first + i;
        u64 a = 2 * j < words ? src[2 * j] : 0;
        u64 b = 2 * j + 1 < words ? src[2 * j + 1] : 0;
        // After the traversal every reachable field is 01b.
        u64 bits = (u64)compressEvenBits(a | (a >> 1))
                 | (u64)compressEvenBits(b | (b >> 1)) << 32;
        dst[i] = bits;
        local += (u64)__builtin_popcountll(bits);
      }
      setBits.fetch_add(local, std::memory_order_relaxed);
    });
    if (setBits.load() != reachable) die("K-plane bit count mismatch after writing");

    h.version = reflectionReduced ? TABLE_FORMAT_V3 : TABLE_FORMAT_V2;
    h.P = TABLE_FLAG_REACHABILITY
        | (reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0);
    h.reachWords = kwords;
    h.reachableCount = reachable;
    h.reachDone = 1;
    memcpy(headerBytes, &h, sizeof h);
    pwriteAllAt(fd, headerBytes, HDR_BYTES, 0, "WDL+K header");
    if (fsync(fd)) die("WDL+K header fsync failed");
    close(fd);
    printf("[reach] appended K plane: %s reachable bits, table format version %u\n",
           commas(reachable).c_str(), h.version);
  }

  bool completedTableAlreadyHasReach() {
    int fd = open(tablePath.c_str(), O_RDONLY);
    if (fd < 0) die("cannot open %s — run 'solve' first", tablePath.c_str());
    u8 hb[HDR_BYTES]; preadAllAt(fd, hb, sizeof hb, 0, "WDL header");
    close(fd);
    Hdr h{}; memcpy(&h, hb, sizeof h);
    if (!validTableHeader(h, N, stateWords, reflectionReduced) || !h.done)
      die("reachability requires a completed matching WDL table");
    if (!tableHasReachability(h)) return false;
    printf("[reach] table already has K plane: %s reachable of %s positions\n",
           commas(h.reachableCount).c_str(), commas(h.N).c_str());
    return true;
  }

 public:
  Reachability(int nthreads, const std::string& path, bool reduced, double ckptMin)
      : Z(reduced), reflectionReduced(reduced), threads(nthreads), ckptMinutes(ckptMin),
        tablePath(path) {
    char b[128];
    snprintf(b, sizeof b, "reach_work_%dx%d%s.bin", R, C, reduced ? "_lr" : "");
    workPath = b;
  }

  void run() {
    double started = now_s();
    Z.build();
    u64 dp = Zdd::dpCount();
    u64 expected = reflectionReduced
                 ? (dp + Zdd::dpMirrorFixedCount()) / 2 : dp;
    if (Z.total != expected) die("ZDD count crosscheck failed");
    SR.build(reflectionReduced);
    if (SR.total != Z.total) die("signature-rank table count does not match the ZDD");
    N = Z.total;
    stateWords = N / TABLE_POSITIONS_PER_WORD + (N % TABLE_POSITIONS_PER_WORD != 0);
    printf("[reach] %dx%d, %d threads, %s universe %s positions\n", R, C, threads,
           reflectionReduced ? "reflection-orbit" : "full-rank",
           commas(N).c_str());
    if (completedTableAlreadyHasReach()) { unlink(workPath.c_str()); return; }
    states = mapWords(stateWords, "reach state map");
    if (!loadWork()) {
      u8 initial[MAXARR], scratch[MAXARR];
      Solver::initialPosition(initial);
      u64 rank = Z.rankCanonical(initial, scratch);
      if (rank == UINT64_MAX) die("initial position rank failed");
      states[rank / TABLE_POSITIONS_PER_WORD] |=
          2ull << ((rank % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION);
      reachable = frontier = 1;
    }
    fflush(stdout);
    double lastCkpt = now_s();
    while (frontier) {
      double t0 = now_s();
      expandLevel();
      frontier = commitLevel();
      reachable += frontier;
      ++level;
      printf("[reach level %3u] +%-13s = %-15s %7.1fs\n", level, commas(frontier).c_str(),
             commas(reachable).c_str(), now_s() - t0);
      fflush(stdout);
      if (frontier && now_s() - lastCkpt >= ckptMinutes * 60.0) {
        saveWork();
        lastCkpt = now_s();
      }
    }
    printf("[reach] traversal complete: %s reachable positions\n",
           commas(reachable).c_str());
    finalizeTable();
    unlink(workPath.c_str());
    syncCurrentDirectory();
    printf("[reach] total time %.1fs\n", now_s() - started);
  }
};

static void auditReachabilityFile(const std::string& tablePath, int threads,
                                  bool reflectionReduced) {
  Zdd Z(reflectionReduced); Z.build();
  u64 nwords = Z.total / TABLE_POSITIONS_PER_WORD
             + (Z.total % TABLE_POSITIONS_PER_WORD != 0);
  int fd = open(tablePath.c_str(), O_RDONLY);
  if (fd < 0) die("cannot open %s", tablePath.c_str());
  u8 hb[HDR_BYTES]; preadAllAt(fd, hb, sizeof hb, 0, "WDL header");
  Hdr h{}; memcpy(&h, hb, sizeof h);
  if (!validTableHeader(h, Z.total, nwords, reflectionReduced)
      || !tableHasReachability(h))
    die("table has no valid K plane");
  off_t reachOffset = (off_t)HDR_BYTES + (off_t)(nwords * sizeof(u64));
  off_t expectedBytes = reachOffset + (off_t)(h.reachWords * sizeof(u64));
  struct stat st{}; if (fstat(fd, &st)) die("table stat failed");
  if (st.st_size != expectedBytes) die("WDL+K table file size mismatch");

  static constexpr u64 CHUNK_WORDS = 1ull << 17;
  std::atomic<u64> cursor{0}, bitCount{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
    (void)t;
    std::vector<u64> buf;
    u64 local = 0;
    for (;;) {
      u64 begin = cursor.fetch_add(CHUNK_WORDS, std::memory_order_relaxed);
      if (begin >= h.reachWords) break;
      u64 end = std::min(h.reachWords, begin + CHUNK_WORDS);
      buf.resize((size_t)(end - begin));
      preadAllAt(fd, buf.data(), buf.size() * sizeof(u64),
                 reachOffset + (off_t)(begin * sizeof(u64)), "K plane");
      for (u64 w : buf) local += __builtin_popcountll(w);
    }
    bitCount.fetch_add(local, std::memory_order_relaxed);
  });
  for (auto& t : pool) t.join();
  if (bitCount.load() != h.reachableCount)
    die("K-plane popcount mismatch: header=%" PRIu64 " actual=%" PRIu64,
        h.reachableCount, bitCount.load());

  u8 initial[MAXARR], scratch[MAXARR]; Solver::initialPosition(initial);
  u64 initialRank = Z.rankCanonical(initial, scratch), word = 0;
  preadAllAt(fd, &word, sizeof word,
             reachOffset + (off_t)((initialRank / 64) * sizeof(u64)), "initial K bit");
  if (!((word >> (initialRank % 64)) & 1)) die("initial position is not marked reachable");
  if (Z.total % 64) {
    u64 last = 0;
    preadAllAt(fd, &last, sizeof last,
               reachOffset + (off_t)((h.reachWords - 1) * sizeof(u64)), "last K word");
    if (last >> (Z.total % 64)) die("K plane has set padding bits");
  }
  close(fd);
  printf("[reach-audit] %s set bits, initial bit set, padding clear, file size valid\n",
         commas(bitCount.load()).c_str());
}

// ------------------------------------------------------------------------------------------
// Cross-validation against the Rust reference dump: records of (u128 position, u8 status).
// Rust encoding: 4 bits/square (same state numbering and order), then 2 bits per hand count
// in E,G,C black then E,G,C white order. Status: 0 draw, 1 win, 2 lose.
// ------------------------------------------------------------------------------------------
static void verifyRust(const Zdd& Z, const Table& T, const char* dumpPath,
                       const char* tablePath) {
  FILE* f = fopen(dumpPath, "rb");
  if (!f) die("cannot open %s", dumpPath);
  int kfd = -1; void* tableMap = nullptr; size_t tableMapBytes = 0;
  const u64* kplane = nullptr;
  if (tableHasReachability(T.hdr)) {
    kfd = open(tablePath, O_RDONLY);
    if (kfd < 0) die("cannot open K-plane table %s", tablePath);
    struct stat st{}; if (fstat(kfd, &st)) die("K-plane table stat failed");
    tableMapBytes = (size_t)st.st_size;
    tableMap = mmap(nullptr, tableMapBytes, PROT_READ, MAP_SHARED, kfd, 0);
    if (tableMap == MAP_FAILED) die("K-plane table mmap failed");
    kplane = (const u64*)((const u8*)tableMap + HDR_BYTES
                         + T.hdr.nwords * sizeof(u64));
  }
  auto isReachable = [&](u64 rank) {
    return kplane && ((kplane[rank / 64] >> (rank % 64)) & 1);
  };
  const size_t REC = 17;
  std::vector<u8> buf(REC * 1000000);
  u64 n = 0, mism = 0, tw = 0, tl = 0, td = 0, mirrorFixed = 0;
  u8 arr[MAXARR], mir[MAXARR], rankScratch[MAXARR];
  for (;;) {
    size_t got = fread(buf.data(), REC, 1000000, f);
    if (!got) break;
    for (size_t i = 0; i < got; ++i) {
      const u8* rec = &buf[i * REC];
      u128 pos = 0;
      for (int b = 15; b >= 0; --b) pos = pos << 8 | rec[b];
      u8 status = rec[16];
      memset(arr, 0, MAXARR);
      for (int q = 0; q < S; ++q)
        arr[q] = (u8)((pos >> (SQUARE_ENCODING_BITS * q)) & 0xF);
      int hs = SQUARE_ENCODING_BITS * S;
      int hbE = (int)((pos >> hs) & 3), hbG = (int)((pos >> (hs + 2)) & 3), hbC = (int)((pos >> (hs + 4)) & 3);
      int hwE = (int)((pos >> (hs + 6)) & 3), hwG = (int)((pos >> (hs + 8)) & 3), hwC = (int)((pos >> (hs + 10)) & 3);
      arr[S + 0] = hbE >= 1; arr[S + 1] = hbE >= 2;
      arr[S + 2] = hbG >= 1; arr[S + 3] = hbG >= 2;
      arr[S + 4] = hbC >= 1; arr[S + 5] = hbC >= 2;
      // consistency: derived white hand must match the dump
      int hb[HAND_TYPES], hw[HAND_TYPES];
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      if (hw[0] != hwE || hw[1] != hwG || hw[2] != hwC)
        die("white-hand mismatch in record %" PRIu64, n + i);
      u64 k = Z.rankCanonical(arr, rankScratch);
      if (k == UINT64_MAX) die("reachable position not in pseudo set (record %" PRIu64 ")", n + i);
      u32 v = T.get(k);
      u32 exp = status == 1 ? 1 : status == 2 ? 2 : 0;
      if (v != exp) {
        if (++mism <= 5)
          printf("[verify-rust] MISMATCH record %" PRIu64 ": rust=%u mine=%u\n", n + i, status, v);
        continue;
      }
      // mirror-value consistency
      mirrorPosition(arr, mir);
      bool fixed = memcmp(arr, mir, ARR) == 0;
      u64 km = Z.rankCanonical(mir, rankScratch);
      if (km == UINT64_MAX || T.get(km) != v) die("mirror value mismatch at record %" PRIu64, n + i);
      if (fixed) ++mirrorFixed;
      if (kplane && (!isReachable(k) || (!Z.reduceLR && !isReachable(km))))
        die("K plane misses a reachable mirror orbit at record %" PRIu64, n + i);
      if (v == 1) ++tw; else if (v == 2) ++tl; else ++td;
    }
    n += got;
  }
  fclose(f);
  printf("[verify-rust] %s records compared, %s mismatches\n", commas(n).c_str(), commas(mism).c_str());
  printf("[verify-rust] reachable W/L/D per my table: %s / %s / %s\n",
         commas(tw).c_str(), commas(tl).c_str(), commas(td).c_str());
  // Expected reachable totals from the reference repo README (Tanaka-matching counts).
  u64 eN = R == 4 ? 246803167ull : 3359910526ull;
  u64 eW = R == 4 ? 196773087ull : 2597975993ull;
  u64 eL = R == 4 ? 47347380ull : 683720498ull;
  u64 eD = R == 4 ? 2682700ull : 78214035ull;
  printf("[verify-rust] expected (repo README):       %s / %s / %s\n",
         commas(eW).c_str(), commas(eL).c_str(), commas(eD).c_str());
  bool ok = !mism && n == eN && tw == eW && tl == eL && td == eD;
  if (kplane) {
    u64 expanded = 2 * n - mirrorFixed;
    printf("[verify-rust] mirror-fixed orbits=%s; expanded full-rank count=%s\n",
           commas(mirrorFixed).c_str(), commas(expanded).c_str());
    u64 expectedReach = Z.reduceLR ? n : expanded;
    if (expectedReach != T.hdr.reachableCount) {
      printf("[verify-rust] K-plane count does not match reference orbits\n");
      ok = false;
    }
    munmap(tableMap, tableMapBytes); close(kfd);
  }
  printf("[verify-rust] %s\n", ok ? "FULL MATCH — table validated against independent solver"
                                  : "MISMATCH — investigate!");
}

// Exhaustively compare a reflection-reduced table against an existing full-rank table.
// Both orientations are checked, so this validates the complete pseudo-position universe,
// including positions absent from the forward-reachable Rust dump.
static void compareFullTable(const Solver& reduced, const std::string& fullPath,
                             int threads) {
  if (!reduced.reflectionReduced) die("compare-full requires --symmetry lr");
  Zdd full(false); full.build();
  u64 fullWords = full.total / TABLE_POSITIONS_PER_WORD
                + (full.total % TABLE_POSITIONS_PER_WORD != 0);
  int fd = open(fullPath.c_str(), O_RDONLY);
  if (fd < 0) die("cannot open full-rank table %s", fullPath.c_str());
  u8 hb[HDR_BYTES]; preadAllAt(fd, hb, sizeof hb, 0, "full table header");
  Hdr h{}; memcpy(&h, hb, sizeof h);
  if (!validTableHeader(h, full.total, fullWords, false) || !h.done)
    die("%s is not a completed matching full-rank table", fullPath.c_str());
  size_t mapBytes = HDR_BYTES + (size_t)fullWords * sizeof(u64);
  void* map = mmap(nullptr, mapBytes, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) die("full table mmap failed");
  const u64* words = (const u64*)((const u8*)map + HDR_BYTES);
  auto fullValue = [&](u64 rank) {
    u64 w = words[rank / TABLE_POSITIONS_PER_WORD];
    int shift = (int)(rank % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
    return (u32)((w >> shift) & 3);
  };

  static constexpr u64 CHUNK = 1ull << 18;
  std::atomic<u64> cursor{0}, mismatches{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) pool.emplace_back([&] {
    ZddIter it;
    u8 mirrored[MAXARR];
    for (;;) {
      u64 lo = cursor.fetch_add(CHUNK, std::memory_order_relaxed);
      if (lo >= reduced.Z.total) break;
      u64 hi = std::min(reduced.Z.total, lo + CHUNK);
      it.initAt(reduced.Z, lo);
      for (u64 rank = lo; rank < hi; ++rank) {
        u32 expected = reduced.T.get(rank);
        u64 raw = full.rank(it.pos);
        if (raw == UINT64_MAX || fullValue(raw) != expected)
          mismatches.fetch_add(1, std::memory_order_relaxed);
        mirrorPosition(it.pos, mirrored);
        u64 rawMirror = full.rank(mirrored);
        if (rawMirror == UINT64_MAX || fullValue(rawMirror) != expected)
          mismatches.fetch_add(1, std::memory_order_relaxed);
        if (rank + 1 < hi) it.next();
      }
    }
  });
  for (auto& t : pool) t.join();
  munmap(map, mapBytes); close(fd);
  printf("[compare-full] %s reflection orbits (%s oriented checks), %s mismatches\n",
         commas(reduced.Z.total).c_str(), commas(2 * reduced.Z.total).c_str(),
         commas(mismatches.load()).c_str());
  if (mismatches.load()) die("reduced/full table comparison failed");
}

// ------------------------------------------------------------------------------------------
// Full fixpoint audit: every position's stored value must be consistent with its
// intrinsic value / children values.
// ------------------------------------------------------------------------------------------
static void audit(Solver& Sv, int threads) {
  std::atomic<u64> bad{0};
  std::atomic<u64> chunk{0};
  const u64 CH = 1 << 20;
  Progress progress("audit", chunk, Sv.Z.total / CH + 1, 1800.0);
  std::vector<std::thread> th;
  for (int t = 0; t < threads; ++t)
    th.emplace_back([&] {
      Solver::EvalWorkspace workspace;
      ZddIter it;
      for (;;) {
        u64 c = chunk.fetch_add(1);
        u64 lo = c * CH;
        if (lo >= Sv.Z.total) break;
        u64 hi = std::min(lo + CH, Sv.Z.total);
        it.initAt(Sv.Z, lo);
        for (u64 idx = lo; idx < hi; ++idx) {
          u32 stored = Sv.T.get(idx);
          int r1 = pass1Eval(it.pos);
          u32 expect = r1 ? (u32)r1
                          : (u32)Sv.passKEval(it.pos, workspace);
          if (stored != expect) ++bad;
          if (idx + 1 < hi) it.next();
        }
      }
    });
  for (auto& x : th) x.join();
  printf("[audit] fixpoint audit over %s positions: %s inconsistencies\n",
         commas(Sv.Z.total).c_str(), commas(bad.load()).c_str());
}

// ------------------------------------------------------------------------------------------
// Display / probe
// ------------------------------------------------------------------------------------------
static const char* PIECE_CH = ".LEGCHlegch";  // index by state; upper = black (Sente)

static void printPos(const u8* arr) {
  int hb[HAND_TYPES], hw[HAND_TYPES];
  handCounts(arr, hb);
  whiteHand(arr, hb, hw);
  printf("  White (gote) hand: E=%d G=%d C=%d   [moves toward row %d]\n",
         hw[0], hw[1], hw[2], R - 1);
  for (int r = 0; r < R; ++r) {
    printf("   row %d | ", r);
    for (int c = 0; c < C; ++c) putchar(PIECE_CH[arr[r * C + c]]);
    printf(" |%s\n", r == 0 ? "  <- White home; Black tries here"
                  : r == R - 1 ? "  <- Black home; White tries here" : "");
  }
  printf("  Black (sente) hand: E=%d G=%d C=%d   [moves toward row 0]\n", hb[0], hb[1], hb[2]);
}

struct Prober {
  Zdd Z; Hdr hdr{}; int fd = -1;
  explicit Prober(bool reduced = false) : Z(reduced) {}
  void open_(const std::string& path) {
    Z.build();
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) die("cannot open table %s — run 'solve' first", path.c_str());
    u8 hb[HDR_BYTES];
    if (read(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header read failed");
    memcpy(&hdr, hb, sizeof hdr);
    u64 nwords = Z.total / TABLE_POSITIONS_PER_WORD
               + (Z.total % TABLE_POSITIONS_PER_WORD != 0);
    if (!validTableHeader(hdr, Z.total, nwords, Z.reduceLR))
      die("table mismatch");
  }
  u32 get(u64 i) const {
    u64 w = 0;
    off_t offset = (off_t)(HDR_BYTES + (i / TABLE_POSITIONS_PER_WORD) * sizeof(u64));
    if (pread(fd, &w, sizeof w, offset) != (ssize_t)sizeof w) die("pread failed");
    int shift = (int)(i % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
    return (u32)((w >> shift) & 3);
  }
  u32 value(const u8* arr) const {
    u8 scratch[MAXARR];
    u64 k = Z.rankCanonical(arr, scratch);
    if (k == UINT64_MAX) die("position is not in the position space");
    return get(k);
  }
  bool hasReachability() const { return tableHasReachability(hdr); }
  bool reachable(const u8* arr) const {
    if (!hasReachability()) die("table has no reachability K plane — run 'reach'");
    u8 scratch[MAXARR];
    u64 k = Z.rankCanonical(arr, scratch);
    if (k == UINT64_MAX) die("position is not in the position space");
    u64 word = 0;
    off_t reachOffset = (off_t)HDR_BYTES + (off_t)(hdr.nwords * sizeof(u64));
    off_t offset = reachOffset + (off_t)((k / 64) * sizeof(u64));
    if (pread(fd, &word, sizeof word, offset) != (ssize_t)sizeof word)
      die("K-plane pread failed");
    return (word >> (k % 64)) & 1;
  }
};

static const char* wdlName(u32 v, bool done) {
  return v == 1 ? "WIN" : v == 2 ? "LOSS" : done ? "DRAW" : "UNKNOWN";
}

// Per-move analysis of a canonical black-to-move position.
static void analyzeMoves(const Prober& P, const u8* arr) {
  bool done = P.hdr.done;
  int hb[HAND_TYPES], hw[HAND_TYPES];
  handCounts(arr, hb);
  whiteHand(arr, hb, hw);
  u8 child[MAXARR], canon[MAXARR];
  int n = 0;
  auto emit = [&](const std::string& mv, const u8* childArr, int capturedType) {
    // childArr: position after Black's move, still Black-oriented; capturedType -1 or 0..2
    int chb[HAND_TYPES] = {hb[0], hb[1], hb[2]};
    if (capturedType >= 0) ++chb[capturedType];
    // evaluate: is it a terminal for White? build canonical (flip) form
    int cw[HAND_TYPES];
    { u8 tmp[MAXARR]; memcpy(tmp, childArr, S);
      tmp[S + 0] = chb[0] >= 1; tmp[S + 1] = chb[0] >= 2;
      tmp[S + 2] = chb[1] >= 1; tmp[S + 3] = chb[1] >= 2;
      tmp[S + 4] = chb[2] >= 1; tmp[S + 5] = chb[2] >= 2;
      int thb[HAND_TYPES]; handCounts(tmp, thb); whiteHand(tmp, thb, cw);
      buildFlip(tmp, cw, canon); }
    const char* res;
    u32 v = P.value(canon);
    res = v == 2 ? "Win" : v == 1 ? "Loss" : done ? "Draw" : "Unknown";
    printf("  %-14s : %s\n", mv.c_str(), res);
    ++n;
  };
  char buf[64];
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    if (s < 1 || s > 5) continue;
    for (u8 t : MOV[s][q]) {
      int tt = arr[t];
      if (tt >= 1 && tt <= 5) continue;
      if (tt == 6) {
        snprintf(buf, sizeof buf, "%c(%d,%d)x LION", PIECE_CH[s], q / C, q % C);
        printf("  %-14s : Win (captures the Lion)\n", buf);
        ++n;
        continue;
      }
      int ns = (s == 4 && rowOf_[t] == 0) ? 5 : s;
      memcpy(child, arr, S);
      child[q] = 0; child[t] = (u8)ns;
      int cap = tt >= 7 ? (tt == 7 ? 0 : tt == 8 ? 1 : 2) : -1;
      snprintf(buf, sizeof buf, "%c(%d,%d)-(%d,%d)%s%s", PIECE_CH[s], q / C, q % C,
               (int)t / C, (int)t % C, tt >= 7 ? "x" : "", ns != s ? "+" : "");
      // safe-try win?
      if (s == 1 && rowOf_[t] == 0) {
        bool attacked = false;
        for (int u = 0; u < S; ++u) {
          int su = arr[u];
          if (su >= 6 && su <= 10 && u != (int)t && ((ATT[su][u] >> t) & 1)) { attacked = true; break; }
        }
        if (!attacked) {
          printf("  %-14s : Win (safe try)\n", buf);
          ++n;
          continue;
        }
      }
      emit(buf, child, cap);
    }
  }
  for (int t3 = 0; t3 < HAND_TYPES; ++t3) {
    if (!hb[t3]) continue;
    for (int q = 0; q < S; ++q) {
      if (arr[q]) continue;
      memcpy(child, arr, S);
      child[q] = (u8)(2 + t3);
      snprintf(buf, sizeof buf, "%c*(%d,%d)", PIECE_CH[2 + t3], q / C, q % C);
      // dropping consumes from hand
      int hb2[HAND_TYPES] = {hb[0], hb[1], hb[2]};
      --hb2[t3];
      u8 tmp[MAXARR]; memcpy(tmp, child, S);
      tmp[S + 0] = hb2[0] >= 1; tmp[S + 1] = hb2[0] >= 2;
      tmp[S + 2] = hb2[1] >= 1; tmp[S + 3] = hb2[1] >= 2;
      tmp[S + 4] = hb2[2] >= 1; tmp[S + 5] = hb2[2] >= 2;
      int thb[HAND_TYPES], cw[HAND_TYPES]; handCounts(tmp, thb); whiteHand(tmp, thb, cw);
      buildFlip(tmp, cw, canon);
      u32 v = P.value(canon);
      printf("  %-14s : %s\n", buf, v == 2 ? "Win" : v == 1 ? "Loss" : done ? "Draw" : "Unknown");
      ++n;
    }
  }
  printf("  (%d legal moves)\n", n);
}

// ------------------------------------------------------------------------------------------
// Independent depth-bounded minimax (own move logic; no ZDD, no symmetry) for spot checks.
// ------------------------------------------------------------------------------------------
namespace bf {
  // position: board[S] states + hands hb[3], hw[3]; side: 0=black to move, 1=white
  struct P {
    u8 b[MAXS]; u8 hb[HAND_TYPES], hw[HAND_TYPES];
    u128 key(int side) const {   // exact packed key: 4 bits/square + 2 bits/hand + side
      u128 k = (u128)side;
      for (int q = 0; q < S; ++q) k = k << SQUARE_ENCODING_BITS | b[q];
      for (int t = 0; t < HAND_TYPES; ++t) k = k << HAND_ENCODING_BITS | hb[t];
      for (int t = 0; t < HAND_TYPES; ++t) k = k << HAND_ENCODING_BITS | hw[t];
      return k;
    }
  };
  struct KeyHash {
    size_t operator()(const u128& x) const noexcept {
      u64 lo = (u64)x, hi = (u64)(x >> 64);
      u64 h = lo * 0x9E3779B97F4A7C15ull ^ (hi + 0x9E3779B97F4A7C15ull) * 0xC2B2AE3D27D4EB4Full;
      return (size_t)(h ^ (h >> 32));
    }
  };
  struct MemoKey {
    u128 pos;
    u32 depth;
    bool operator==(const MemoKey&) const = default;
  };
  struct MemoHash {
    size_t operator()(const MemoKey& x) const noexcept {
      size_t h = KeyHash{}(x.pos);
      u64 d = (u64)x.depth * 0x9E3779B97F4A7C15ull;
      return h ^ (size_t)(d ^ (d >> 32));
    }
  };
  static bool mine(int s, int side) { return side == 0 ? (s >= 1 && s <= 5) : (s >= 6 && s <= 10); }
  static int lionOf(int side) { return side == 0 ? 1 : 6; }
  static int tryRow(int side) { return side == 0 ? 0 : R - 1; }
  static bool attacks(const P& p, int side, int target) {
    for (int q = 0; q < S; ++q) {
      int s = p.b[q];
      if (!mine(s, side)) continue;
      int bs = side == 0 ? s : s - 5;   // black-equivalent kind
      u32 m = side == 0 ? ATT[bs][q] : ATT[bs + 5][q];
      if ((m >> target) & 1) return true;
    }
    return false;
  }
  static int findLion(const P& p, int side) {
    for (int q = 0; q < S; ++q) if (p.b[q] == lionOf(side)) return q;
    return -1;
  }
  struct Move { int from, to, drop; };   // drop: -1 board move, else hand type 0..2
  static void genMoves(const P& p, int side, std::vector<Move>& out) {
    out.clear();
    for (int q = 0; q < S; ++q) {
      int s = p.b[q];
      if (!mine(s, side)) continue;
      int bs = side == 0 ? s : s - 5;
      const std::vector<u8>& tv = MOV[bs][q];
      if (side == 0) {
        for (u8 t : tv) {
          int tt = p.b[t];
          if (mine(tt, 0) || tt == 6) continue;
          out.push_back({q, (int)t, -1});
        }
      } else {
        // white targets: mirror rows of black movement from flipped square
        int fq = flipSq_[q];
        for (u8 ft : MOV[bs][fq]) {
          int t = flipSq_[ft];
          int tt = p.b[t];
          if (mine(tt, 1) || tt == 1) continue;
          out.push_back({q, t, -1});
        }
      }
    }
    const u8* h = side == 0 ? p.hb : p.hw;
    for (int t3 = 0; t3 < HAND_TYPES; ++t3) {
      if (!h[t3]) continue;
      for (int q = 0; q < S; ++q)
        if (!p.b[q]) out.push_back({-1, q, t3});
    }
  }
  static P apply(const P& p, int side, const Move& m) {
    P r = p;
    if (m.drop >= 0) {
      r.b[m.to] = (u8)(2 + m.drop + (side ? 5 : 0));
      if (side == 0) --r.hb[m.drop]; else --r.hw[m.drop];
      return r;
    }
    int s = p.b[m.from], tt = p.b[m.to];
    int kind = side == 0 ? s : s - 5;
    if (kind == 4 && rowOf_[m.to] == tryRow(side)) kind = 5;
    r.b[m.from] = 0;
    r.b[m.to] = (u8)(kind + (side ? 5 : 0));
    if (tt) {   // capture (opponent piece, never a lion here)
      int ok = side == 0 ? tt - 5 : tt;   // opponent's kind 1..5
      int ht = ok == 2 ? 0 : ok == 3 ? 1 : 2;
      if (side == 0) ++r.hb[ht]; else ++r.hw[ht];
    }
    return r;
  }
  // terminal for `side` to move: win if side attacks opp lion; loss if opp lion on side's
  // home row (opponent completed a try) and not capturable
  static int terminal(const P& p, int side) {
    int ol = findLion(p, 1 - side);
    if (attacks(p, side, ol)) return 1;
    if (rowOf_[ol] == tryRow(1 - side)) return 2;
    return 0;
  }
  static std::unordered_map<MemoKey, bool, MemoHash> memoW, memoL;
  static bool canWin(const P& p, int side, int d);
  static bool willLose(const P& p, int side, int d) {
    int t = terminal(p, side);
    if (t) return t == 2;
    if (d == 0) return false;
    MemoKey mk{p.key(side), (u32)d};
    auto it = memoL.find(mk);
    if (it != memoL.end()) return it->second;
    std::vector<Move> mv;
    genMoves(p, side, mv);
    bool res = !mv.empty();
    for (const Move& m : mv) {
      if (!canWin(apply(p, side, m), 1 - side, d - 1)) { res = false; break; }
    }
    memoL[mk] = res;
    return res;
  }
  static bool canWin(const P& p, int side, int d) {
    int t = terminal(p, side);
    if (t) return t == 1;
    if (d == 0) return false;
    MemoKey mk{p.key(side), (u32)d};
    auto it = memoW.find(mk);
    if (it != memoW.end()) return it->second;
    std::vector<Move> mv;
    genMoves(p, side, mv);
    bool res = false;
    for (const Move& m : mv) {
      if (willLose(apply(p, side, m), 1 - side, d - 1)) { res = true; break; }
    }
    memoW[mk] = res;
    return res;
  }

  static void spotCheck(const Zdd& Z, const Prober& P2, int samples, int depth) {
    std::mt19937_64 rng(4242);
    u8 arr[MAXARR];
    int nW = 0, nL = 0, nD = 0, decided = 0;
    for (int i = 0; i < samples; ++i) {
      u64 k = rng() % Z.total;
      Z.unrank(k, arr);
      P p{};
      memcpy(p.b, arr, S);
      int hb[HAND_TYPES], hw[HAND_TYPES];
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      for (int t = 0; t < HAND_TYPES; ++t) { p.hb[t] = (u8)hb[t]; p.hw[t] = (u8)hw[t]; }
      u32 tv = P2.get(k);
      bool cw = canWin(p, 0, depth), wl = willLose(p, 0, depth);
      if (cw && tv != 1) die("bf: canWin but table=%u at k=%" PRIu64, tv, k);
      if (wl && tv != 2) die("bf: willLose but table=%u at k=%" PRIu64, tv, k);
      if (tv == 0 && (cw || wl)) die("bf: draw contradicted at k=%" PRIu64, k);
      if (cw || wl) ++decided;
      if (tv == 1) ++nW; else if (tv == 2) ++nL; else ++nD;
      if (memoW.size() + memoL.size() > 80000000) { memoW.clear(); memoL.clear(); }
    }
    printf("[bf] %d random positions spot-checked to depth %d (%d bf-decided), "
           "sample W/L/D %d/%d/%d — all consistent\n", samples, depth, decided, nW, nL, nD);
  }
}

// ------------------------------------------------------------------------------------------
// Self-tests of the signature ranks, the rank iterator, and the move / un-move generators
// against the ZDD and a plain reference move generator.
// ------------------------------------------------------------------------------------------
static void setHandFlags(u8* arr, const int* h) {
  for (int t = 0; t < HAND_TYPES; ++t) {
    arr[S + 2 * t] = h[t] >= 1;
    arr[S + 2 * t + 1] = h[t] >= 2;
  }
}

// Children of `arr` in genChildren's order, each ranked from an explicitly built position.
static void referenceChildren(const Zdd& Z, const u8* arr, std::vector<u64>& out,
                              std::vector<u8>& wonAtOnce) {
  out.clear(); wonAtOnce.clear();
  int hb[HAND_TYPES], hw[HAND_TYPES];
  handCounts(arr, hb);
  u8 tmp[MAXARR], canon[MAXARR], scratch[MAXARR];
  auto emit = [&](const int* hand) {
    setHandFlags(tmp, hand);
    whiteHand(tmp, hand, hw);
    buildFlip(tmp, hw, canon);
    u64 k = Z.rankCanonical(canon, scratch);
    assert(k != UINT64_MAX);
    out.push_back(k);
    // "won at once" means the first test of pass1Eval: the lion can be captured
    int wl = -1; u32 attB = 0;
    for (int q = 0; q < S; ++q) {
      int s = canon[q];
      if (s == 6) wl = q;
      if (s >= 1 && s <= PIECE_KINDS) attB |= ATT[s][q];
    }
    wonAtOnce.push_back((u8)((attB >> wl) & 1));
  };
  for (int q = 0; q < S; ++q) {
    int s = arr[q];
    if (s < 1 || s > PIECE_KINDS) continue;
    for (u8 t : MOV[s][q]) {
      int tt = arr[t];
      if (tt >= 1 && tt <= 6) continue;
      memcpy(tmp, arr, ARR);
      tmp[q] = 0;
      tmp[t] = (u8)((s == 4 && rowOf_[t] == 0) ? 5 : s);
      int h[HAND_TYPES] = {hb[0], hb[1], hb[2]};
      if (tt >= 7) ++h[tt == 7 ? 0 : tt == 8 ? 1 : 2];
      emit(h);
    }
  }
  for (int type = 0; type < HAND_TYPES; ++type) {
    if (!hb[type]) continue;
    for (int q = 0; q < S; ++q) {
      if (arr[q]) continue;
      memcpy(tmp, arr, ARR);
      tmp[q] = (u8)(2 + type);
      int h[HAND_TYPES] = {hb[0], hb[1], hb[2]};
      --h[type];
      emit(h);
    }
  }
}

static void selftestRetro(const Zdd& Z, std::mt19937_64& rng) {
  SigRank SR;
  SR.build(Z.reduceLR);
  assert(SR.total == Z.total);
  u8 arr[MAXARR], arr2[MAXARR];
  SigIter it;
  it.init(SR);
  for (int i = 0; i < 200000; ++i) {
    u64 k = rng() % Z.total;
    Z.unrank(k, arr);
    assert(SR.rank(arr) == k);
    it.seek(k);
    assert(memcmp(it.pos, arr, ARR) == 0);
    assert(it.kpre[S] <= k && k < it.kend[S]);
  }
  for (int rep = 0; rep < 200; ++rep) {     // forward runs with mixed gaps, then a jump back
    u64 k = rng() % Z.total;
    for (int j = 0; j < 400 && k < Z.total; ++j) {
      it.seek(k);
      Z.unrank(k, arr);
      assert(memcmp(it.pos, arr, ARR) == 0);
      u64 gap = j % 3 == 0 ? 1 : j % 3 == 1 ? 1 + rng() % 40 : 1 + rng() % 100000;
      k += gap;
    }
    it.seek(rng() % Z.total);
  }
  {                                          // whole-board intervals partition the ranks
    u64 k = rng() % (Z.total - 100000), end = k + 100000;
    it.seek(k);
    for (u64 e = it.kend[S]; e < end; e = it.kend[S]) {
      it.seek(e);
      assert(it.kpre[S] == e);
      Z.unrank(e - 1, arr); Z.unrank(e, arr2);
      assert(memcmp(arr, arr2, S) != 0);
    }
  }
  printf("[selftest] signature ranks + iterator match the ZDD\n");

  RankCtx cx;
  cx.sr = &SR;
  std::vector<u64> ref, preds;
  std::vector<u8> won;
  u64 kids[MAX_KIDS], kids2[MAX_KIDS], kids3[2];
  int nChildren = 0, nPreds = 0, nSkipped = 0;
  auto attacksLion = [](const u8* a) {
    int wl = -1; u32 attB = 0;
    for (int q = 0; q < S; ++q) {
      int s = a[q];
      if (s == 6) wl = q;
      if (s >= 1 && s <= PIECE_KINDS) attB |= ATT[s][q];
    }
    return (bool)((attB >> wl) & 1);
  };
  for (int i = 0; i < 40000; ++i) {
    u64 k = rng() % Z.total;
    Z.unrank(k, arr);
    // Positions whose white lion already stands on Black's home rank are terminal; their
    // "children" would leave the position space and are never generated by the solvers.
    bool tried = false;
    for (int q = (R - 1) * C; q < S; ++q) tried |= arr[q] == 6;
    if (!tried) {
      // (a) children: generator == reference, in order; skipWon drops exactly the
      //     children whose lion can be captured at once
      referenceChildren(Z, arr, ref, won);
      bool complete, any;
      int n = genChildren(arr, cx, kids, MAX_KIDS, false, complete, any);
      assert(complete && n == (int)ref.size() && any == !ref.empty());
      for (int j = 0; j < n; ++j) assert(kids[j] == ref[j]);
      int n2 = genChildren(arr, cx, kids2, MAX_KIDS, true, complete, any);
      assert(complete && any == !ref.empty());
      int j2 = 0;
      for (int j = 0; j < n; ++j) {
        if (won[j]) { ++nSkipped; continue; }
        assert(j2 < n2 && kids2[j2] == ref[j]);
        ++j2;
      }
      assert(j2 == n2);
      int n3 = genChildren(arr, cx, kids3, 2, true, complete, any);   // limited generation
      assert(n3 == std::min(n2, 2) && complete == (n2 <= 2));
      for (int j = 0; j < n3; ++j) assert(kids3[j] == kids2[j]);
      nChildren += n;
      // (b) every child lists this position among its predecessors (unless this position
      //     already wins by capturing the lion, which genPreds leaves out by design)
      if (!attacksLion(arr) && i % 4 == 0) {
        for (int j = 0; j < n; ++j) {
          Z.unrank(kids[j], arr2);
          bool found = false;
          genPreds(arr2, cx, [&](u64 r) { found |= r == k; });
          assert(found);
        }
      }
    }
    // (c) every generated predecessor really has this position as a child
    if (i % 4 == 1) {
      preds.clear();
      genPreds(arr, cx, [&](u64 r) { preds.push_back(r); });
      for (u64 pk : preds) {
        assert(pk < Z.total);
        Z.unrank(pk, arr2);
        assert(!attacksLion(arr2));
        referenceChildren(Z, arr2, ref, won);
        assert(std::find(ref.begin(), ref.end(), k) != ref.end());
        ++nPreds;
      }
    }
  }
  printf("[selftest] move generator == reference (%d children, %d won at once); "
         "un-move generator is its exact inverse (%d predecessors)\n",
         nChildren, nSkipped, nPreds);
}

// ------------------------------------------------------------------------------------------
static bool parseHand(const std::string& tok, int* h) {
  h[0] = h[1] = h[2] = 0;
  if (tok == "-") return true;
  for (char ch : tok) {
    if (ch == 'E' || ch == 'e') ++h[0];
    else if (ch == 'G' || ch == 'g') ++h[1];
    else if (ch == 'C' || ch == 'c') ++h[2];
    else return false;
  }
  return h[0] <= PIECES_PER_TYPE && h[1] <= PIECES_PER_TYPE
      && h[2] <= PIECES_PER_TYPE;
}

int main(int argc, char** argv) {

  if (argc < 2) {
    fprintf(stderr,
      "usage: %s COMMAND [--board ROWSx3] [--symmetry none|lr] [--table PATH]\n"
      "                       [--threads N] [--ckpt-min MINUTES] [--ckpt K] [arguments]\n"
      "       COMMAND: selftest | solve | solve-forward | reach | reach-audit | info |\n"
      "                verify-rust [dump] | compare-full [full-table] | audit |\n"
      "                bfcheck [samples depth] | analyze |\n"
      "                probe <b|w> <ROWS*3 cells> <sente-hand|-> <gote-hand|->\n"
      "       supported boards: %dx3 through %dx3\n"
      "       cells: '.' empty; L E G C H = Sente/black; l e g c h = Gote/white\n"
      "       hands: letters from EGC, e.g. 'EC', or '-'\n",
      argv[0], MIN_R, MAX_R);
    return 1;
  }
  std::string cmd = argv[1];
  int threads = (int)std::max(1u, std::thread::hardware_concurrency());
  int ckpt = 10, rows = MIN_R;
  double ckptMinutes = 120.0;
  bool reflectionReduced = false;
  std::string explicitTable;
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--ckpt" && i + 1 < argc) ckpt = atoi(argv[++i]);
    else if (a == "--ckpt-min" && i + 1 < argc) ckptMinutes = atof(argv[++i]);
    else if (a == "--symmetry" && i + 1 < argc) {
      std::string mode = argv[++i];
      if (mode == "lr") reflectionReduced = true;
      else if (mode == "none") reflectionReduced = false;
      else die("--symmetry must be 'none' or 'lr'");
    } else if (a == "--table" && i + 1 < argc) {
      explicitTable = argv[++i];
    }
    else if (a == "--board" && i + 1 < argc) {
      int rr = 0, cc = 0; char trailing = 0;
      if (sscanf(argv[++i], "%dx%d%c", &rr, &cc, &trailing) != 2
          || cc != C || rr < MIN_R || rr > MAX_R)
        die("--board must be between %dx%d and %dx%d", MIN_R, C, MAX_R, C);
      rows = rr;
    } else rest.push_back(a);
  }
  if (threads < 1) die("--threads must be at least 1");
  if (ckpt < 1) die("--ckpt must be at least 1");
  if (!(ckptMinutes >= 0)) die("--ckpt-min must be nonnegative");
  initTables(rows);
  char pathbuf[128];
  snprintf(pathbuf, sizeof pathbuf, reflectionReduced ? "wdl_%dx%d_lr.bin"
                                                       : "wdl_%dx%d.bin", R, C);
  std::string tablePath = explicitTable.empty() ? pathbuf : explicitTable;
  snprintf(pathbuf, sizeof pathbuf, "rust_dump%s.bin", R == 4 ? "" : "_5x3");
  std::string dumpPath = pathbuf;

  if (cmd == "selftest") {
    Zdd Z(reflectionReduced); Z.build();
    u64 dp = Zdd::dpCount();
    u64 fixed = reflectionReduced ? Zdd::dpMirrorFixedCount() : 0;
    u64 expected = reflectionReduced ? (dp + fixed) / 2 : dp;
    printf("[selftest] nodes=%s count=%s expected=%s %s%s\n",
           commas(Z.idx_.size()).c_str(), commas(Z.total).c_str(),
           commas(expected).c_str(), Z.total == expected ? "OK" : "MISMATCH",
           reflectionReduced ? " (reflection orbits)" : "");
    if (reflectionReduced)
      printf("[selftest] full=%s mirror-fixed=%s\n",
             commas(dp).c_str(), commas(fixed).c_str());
    assert(Z.total == expected);
    assert(S == R * C && ARR == S + HAND_ITEMS);
    assert(NITEMS == S * PIECE_STATES + HAND_ITEMS && NITEMS <= MAX_ZDD_ITEMS);
    u32 validSquares = S == std::numeric_limits<u32>::digits
                     ? std::numeric_limits<u32>::max() : ((u32)1 << S) - 1;
    for (int q = 0; q < S; ++q) {
      assert(flipSq_[flipSq_[q]] == q);
      assert(mirSq_[mirSq_[q]] == q);
      for (int s = 1; s <= PIECE_STATES; ++s) assert((ATT[s][q] & ~validSquares) == 0);
      for (int s = 1; s <= PIECE_STATES / 2; ++s) {
        u32 moveMask = 0;
        for (u8 t : MOV[s][q]) moveMask |= (u32)1 << t;
        assert(moveMask == ATT[s][q]);
      }
    }
    printf("[selftest] board geometry + representation limits OK\n");
    std::mt19937_64 rng(777);
    u8 arr[MAXARR], arr2[MAXARR], scratch[MAXARR];
    for (int i = 0; i < 50000; ++i) {
      u64 k = rng() % Z.total;
      Z.unrank(k, arr);
      // validate: one lion each, black lion not on row 0, counts
      int nl = 0, nwl = 0, hb[HAND_TYPES], hw[HAND_TYPES];
      for (int q = 0; q < S; ++q) {
        if (arr[q] == 1) { ++nl; assert(rowOf_[q] != 0); }
        if (arr[q] == 6) ++nwl;
      }
      assert(nl == 1 && nwl == 1);
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      for (int t = 0; t < HAND_TYPES; ++t)
        assert(hw[t] >= 0 && hw[t] <= PIECES_PER_TYPE);
      assert(Z.rank(arr) == k);
      if (reflectionReduced) {
        assert(compareReflection(arr) <= 0);
        mirrorPosition(arr, arr2);
        assert(Z.rankCanonical(arr2, scratch) == k);
      }
    }
    printf("[selftest] rank/unrank + invariants OK\n");
    ZddIter it;
    for (int rep = 0; rep < 5; ++rep) {
      u64 k0 = rng() % (Z.total - 3000);
      it.initAt(Z, k0);
      for (u64 k = k0; k < k0 + 3000; ++k) {
        Z.unrank(k, arr);
        assert(memcmp(arr, it.pos, ARR) == 0);
        if (k + 1 < Z.total) assert(it.next());
      }
    }
    printf("[selftest] iterator OK\n");
    // cached child rank vs full rank
    u32 nodeAt[MAXARR + 1], mirrorNodeAt[MAXARR + 1];
    u64 kAt[MAXARR + 1], mirrorKAt[MAXARR + 1];
    u8 fpos[MAXARR], childbuf[MAXARR], mirrorBase[MAXARR], mirrorChild[MAXARR];
    int checked = 0;
    for (int i = 0; i < 3000; ++i) {
      u64 k = rng() % Z.total;
      Z.unrank(k, arr);
      if (pass1Eval(arr) != 0) continue;
      int hb[HAND_TYPES], hw[HAND_TYPES];
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      buildFlip(arr, hw, fpos);
      Z.buildPrefix(fpos, nodeAt, kAt);
      if (!reflectionReduced)
        assert(kAt[ARR] == Z.rank(fpos));
      else {
        mirrorPosition(fpos, mirrorBase);
        Z.buildPrefix(mirrorBase, mirrorNodeAt, mirrorKAt);
        memcpy(mirrorChild, mirrorBase, ARR);
      }
      memcpy(childbuf, fpos, ARR);
      for (int q = 0; q < S; ++q) {
        int s = arr[q];
        if (s < 1 || s > 5) continue;
        for (u8 t : MOV[s][q]) {
          int tt = arr[t];
          if (tt >= 1 && tt <= 6) continue;
          int ns = (s == 4 && rowOf_[t] == 0) ? 5 : s;
          int f1 = flipSq_[q], f2 = flipSq_[t];
          childbuf[f1] = 0; childbuf[f2] = (u8)(ns + 5);
          memcpy(arr2, childbuf, ARR);
          u64 full = Z.rankCanonical(arr2, scratch);
          u64 kc;
          if (!reflectionReduced) {
            kc = Z.rankFrom(nodeAt[std::min(f1, f2)], kAt[std::min(f1, f2)], childbuf);
          } else {
            int mf1 = mirSq_[f1], mf2 = mirSq_[f2];
            mirrorChild[mf1] = 0; mirrorChild[mf2] = (u8)(ns + 5);
            kc = compareReflection(childbuf) <= 0
               ? Z.rankFrom(nodeAt[std::min(f1, f2)], kAt[std::min(f1, f2)], childbuf)
               : Z.rankFrom(mirrorNodeAt[std::min(mf1, mf2)],
                            mirrorKAt[std::min(mf1, mf2)], mirrorChild);
            mirrorChild[mf1] = mirrorBase[mf1]; mirrorChild[mf2] = mirrorBase[mf2];
          }
          assert(kc == full && kc != UINT64_MAX);
          childbuf[f1] = fpos[f1]; childbuf[f2] = fpos[f2];
          ++checked;
        }
      }
      for (int t3 = 0; t3 < HAND_TYPES; ++t3) {
        if (!hb[t3]) continue;
        for (int q = 0; q < S; ++q) {
          if (arr[q]) continue;
          int f1 = flipSq_[q];
          childbuf[f1] = (u8)(2 + t3 + 5);
          memcpy(arr2, childbuf, ARR);
          u64 full = Z.rankCanonical(arr2, scratch);
          u64 kc;
          if (!reflectionReduced) kc = Z.rankFrom(nodeAt[f1], kAt[f1], childbuf);
          else {
            int mf1 = mirSq_[f1]; mirrorChild[mf1] = (u8)(2 + t3 + 5);
            kc = compareReflection(childbuf) <= 0
               ? Z.rankFrom(nodeAt[f1], kAt[f1], childbuf)
               : Z.rankFrom(mirrorNodeAt[mf1], mirrorKAt[mf1], mirrorChild);
            mirrorChild[mf1] = mirrorBase[mf1];
          }
          assert(kc == full && kc != UINT64_MAX);
          childbuf[f1] = fpos[f1];
          ++checked;
        }
      }
    }
    printf("[selftest] %s child-rank OK (%d children)\n",
           reflectionReduced ? "canonical" : "cached", checked);
    selftestRetro(Z, rng);
    printf("[selftest] ALL OK\n");
  } else if (cmd == "solve" || cmd == "solve-forward") {
    interleaveMemory();
    Solver Sv(reflectionReduced);
    Sv.threads = threads;
    printf("[solve] %dx3 dobutsu shogi, %d threads, table %s%s\n", R, threads,
           tablePath.c_str(), cmd == "solve" ? "" : " (forward iteration)");
    Sv.setup(tablePath);
    double t0 = now_s();
    if (cmd == "solve") {
      RetroSolver retro(Sv);
      retro.setup();
      retro.solve(ckptMinutes);
    } else {
      Sv.solve(ckpt);
    }
    printf("[solve] total time %.1fs\n", now_s() - t0);
  } else if (cmd == "reach") {
    interleaveMemory();
    Reachability reach(threads, tablePath, reflectionReduced, ckptMinutes);
    reach.run();
  } else if (cmd == "reach-audit") {
    auditReachabilityFile(tablePath, threads, reflectionReduced);
  } else if (cmd == "info") {
    Prober P(reflectionReduced);
    P.open_(tablePath);
    printf("Table: %s\n", tablePath.c_str());
    printf("  format version: %u\n", P.hdr.version);
    printf("  indexing: %s\n", tableIsReflectionReduced(P.hdr)
           ? "left/right-reflection orbits" : "full oriented ranks");
    printf("  board/universe: %ux%u / %s positions\n", P.hdr.L, P.hdr.W,
           commas(P.hdr.N).c_str());
    printf("  WDL: %s, passes: %u\n", P.hdr.done ? "complete" : "incomplete",
           P.hdr.passesDone);
    if (P.hasReachability())
      printf("  K plane: %s reachable positions (%s words)\n",
             commas(P.hdr.reachableCount).c_str(), commas(P.hdr.reachWords).c_str());
    else
      printf("  K plane: not present\n");
  } else if (cmd == "verify-rust") {
    if (R != 4 && R != 5)
      die("no independent reference dump exists for %dx%d — use audit/bfcheck", R, C);
    Solver Sv(reflectionReduced);
    Sv.setup(tablePath);
    if (!Sv.T.load(tablePath, Sv.Z.total, reflectionReduced) || !Sv.T.hdr.done)
      die("solve first");
    verifyRust(Sv.Z, Sv.T,
               rest.empty() ? dumpPath.c_str() : rest[0].c_str(),
               tablePath.c_str());
  } else if (cmd == "compare-full") {
    if (!reflectionReduced) die("compare-full requires --symmetry lr");
    Solver Sv(true);
    Sv.setup(tablePath);
    if (!Sv.T.load(tablePath, Sv.Z.total, true) || !Sv.T.hdr.done) die("solve first");
    char fullbuf[128]; snprintf(fullbuf, sizeof fullbuf, "wdl_%dx%d.bin", R, C);
    compareFullTable(Sv, rest.empty() ? fullbuf : rest[0], threads);
  } else if (cmd == "audit") {
    Solver Sv(reflectionReduced);
    Sv.setup(tablePath);
    if (!Sv.T.load(tablePath, Sv.Z.total, reflectionReduced) || !Sv.T.hdr.done)
      die("solve first");
    audit(Sv, threads);
  } else if (cmd == "bfcheck") {
    Prober P(reflectionReduced);
    P.open_(tablePath);
    int samples = rest.size() > 0 ? atoi(rest[0].c_str()) : 20000;
    int depth = rest.size() > 1 ? atoi(rest[1].c_str()) : 9;
    if (samples < 0 || depth < 0) die("bfcheck samples and depth must be nonnegative");
    bf::spotCheck(P.Z, P, samples, depth);
  } else if (cmd == "analyze" || cmd == "probe") {
    Prober P(reflectionReduced);
    P.open_(tablePath);
    bool done = P.hdr.done;
    if (!done) printf("note: table incomplete (passes done: %u)\n", P.hdr.passesDone);
    u8 arr[MAXARR];
    bool whiteToMove = false;
    if (cmd == "analyze") {
      Solver::initialPosition(arr);
      printf("Initial position (Sente to move):\n");
    } else {
      if ((int)rest.size() != 1 + S + 2)
        die("probe needs side + %d cells + 2 hands, got %zu args", S, rest.size());
      whiteToMove = (rest[0] == "w" || rest[0] == "W");
      memset(arr, 0, ARR);
      for (int q = 0; q < S; ++q) {
        const std::string& t = rest[1 + q];
        if (t.size() != 1) die("bad cell '%s'", t.c_str());
        const char* p = strchr(PIECE_CH, t[0]);
        int s = (t[0] == '.' || t[0] == '-') ? 0 : p ? (int)(p - PIECE_CH) : -1;
        if (s < 0) die("bad cell '%s'", t.c_str());
        arr[q] = (u8)s;
      }
      int hbP[HAND_TYPES], hwP[HAND_TYPES];
      if (!parseHand(rest[1 + S], hbP) || !parseHand(rest[2 + S], hwP)) die("bad hand");
      arr[S + 0] = hbP[0] >= 1; arr[S + 1] = hbP[0] >= 2;
      arr[S + 2] = hbP[1] >= 1; arr[S + 3] = hbP[1] >= 2;
      arr[S + 4] = hbP[2] >= 1; arr[S + 5] = hbP[2] >= 2;
      int hb[HAND_TYPES], hw[HAND_TYPES];
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      for (int t = 0; t < HAND_TYPES; ++t)
        if (hw[t] != hwP[t])
          die("gote hand inconsistent with piece counts (expect %d %d %d)", hw[0], hw[1], hw[2]);
    }
    u8 canon[MAXARR];
    if (whiteToMove) {
      int hb[HAND_TYPES], hw[HAND_TYPES];
      handCounts(arr, hb);
      whiteHand(arr, hb, hw);
      buildFlip(arr, hw, canon);
    } else memcpy(canon, arr, ARR);
    printPos(arr);
    u32 v = P.value(canon);
    printf("Side to move (%s): %s\n", whiteToMove ? "Gote/White" : "Sente/Black", wdlName(v, done));
    printf("Reachability: %s\n", P.hasReachability()
           ? (P.reachable(canon) ? "REACHABLE" : "UNREACHABLE")
           : "not present (run 'reach')");
    printf("Moves (result for the side moving)%s:\n",
           whiteToMove ? " [shown in the flipped, color-swapped frame]" : "");
    analyzeMoves(P, canon);
  } else {
    die("unknown command '%s'", cmd.c_str());
  }
  return 0;
}
