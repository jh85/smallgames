// dobutsu.cpp — Strong solver for 4x3 through 8x3 Dobutsu Shogi (animal shogi) via a
// ZDD minimal perfect hash + retrograde analysis, following the architecture of NOCCA x NOCCA
// solver (Yamamoto & Hoki, GPW 2022) with a hand-piece extension.
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
#include <chrono>
#include <random>
#include <algorithm>
#include <limits>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
// constants. Attack masks and the independent u128 checker both safely cover an 8x3 board.
static constexpr int C = 3;
static constexpr int MIN_R = 4, MAX_R = 8;
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
    for (int q = 0; q < MAXS; ++q) MOV[k][q].clear();
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
    if (nwords > std::numeric_limits<size_t>::max() / sizeof(u64))
      die("table is too large for this platform");
    size_t bytes = nwords * sizeof(u64);
    words = (u64*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (words == MAP_FAILED) die("mmap failed");
    madvise(words, bytes, MADV_HUGEPAGE);
  }
  inline u32 get(u64 i) const {
    u64 w = __atomic_load_n(&words[i / TABLE_POSITIONS_PER_WORD], __ATOMIC_RELAXED);
    int shift = (int)(i % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
    return (u32)((w >> shift) & 3);
  }
  void save(const std::string& path) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open %s failed", tmp.c_str());
    u8 hb[HDR_BYTES]; memset(hb, 0, sizeof hb);
    memcpy(hb, &hdr, sizeof hdr);
    if (write(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header write failed");
    size_t bytes = nwords * sizeof(u64), off = 0;
    while (off < bytes) {
      ssize_t r = write(fd, (u8*)words + off, std::min((size_t)1 << 30, bytes - off));
      if (r <= 0) die("table write failed");
      off += (size_t)r;
    }
    if (fsync(fd)) die("fsync failed");
    close(fd);
    if (rename(tmp.c_str(), path.c_str())) die("rename failed");
  }
  bool load(const std::string& path, u64 n, bool reduced = false) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    u8 hb[HDR_BYTES];
    if (read(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header read failed");
    memcpy(&hdr, hb, sizeof hdr);
    if (!validTableHeader(hdr, n, nwords, reduced))
      die("table mismatch");
    size_t bytes = nwords * sizeof(u64), off = 0;
    while (off < bytes) {
      ssize_t r = read(fd, (u8*)words + off, std::min((size_t)1 << 30, bytes - off));
      if (r <= 0) die("table read failed");
      off += (size_t)r;
    }
    close(fd);
    return true;
  }
};

// ------------------------------------------------------------------------------------------
// Solver
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
// Forward reachability. The finished K plane is one bit per ZDD rank and is appended to a
// completed version-1 WDL table. During traversal a resumable two-bit work map is used:
// 0=unseen, 1=committed reachable, 2=pending in the next frontier. A small phase journal
// makes a crash during frontier construction or pending->committed conversion recoverable.
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

struct PackedPos {
  u64 lo, hi;
};
static_assert(sizeof(PackedPos) == 16);

static PackedPos packPosition(const u8* arr) {
  u128 p = 0;
  for (int i = 0; i < ARR; ++i) p |= (u128)arr[i] << (4 * i);
  return {(u64)p, (u64)(p >> 64)};
}

static void unpackPosition(const PackedPos& p, u8* arr) {
  u128 v = (u128)p.lo | (u128)p.hi << 64;
  for (int i = 0; i < ARR; ++i) arr[i] = (u8)((v >> (4 * i)) & 0xf);
}

static constexpr size_t REACH_WORK_HDR_BYTES = 4096;
static constexpr u32 REACH_WORK_VERSION = 1;
enum ReachPhase : u32 { REACH_READY = 0, REACH_EXPANDING = 1, REACH_COMMITTING = 2 };

struct ReachWorkHdr {
  char magic[8];                    // "DOBURCHW"
  u32 version, W, L, phase;
  u32 frontierSlot, nextSlot, level, reserved;
  u64 N, stateWords;
  u64 frontierCount, nextCount, reachableCount, commitTotal;
};
static_assert(sizeof(ReachWorkHdr) <= REACH_WORK_HDR_BYTES);

class Reachability {
  Zdd Z;
  bool reflectionReduced;
  int threads;
  std::string tablePath, workPath, frontierPath[2], frontierTmpPath;
  int workFd = -1;
  u8* workMap = nullptr;
  size_t workMapBytes = 0, stateBytes = 0;
  ReachWorkHdr* wh = nullptr;
  u64* states = nullptr;

  static constexpr u64 FRONTIER_CHUNK = 1ull << 12;
  static constexpr size_t WRITER_RECORDS = 1ull << 13;

  u64 expectedReachableCount() const {
    // This solver's ZDD ranks both left/right orientations separately. The published
    // 246,803,167 count is after reflect_lr canonicalization; the corresponding full-rank
    // 4x3 set has 493,573,042 positions (33,292 of the published orbits are fixed mirrors).
    // The 5x3 reflection-orbit count comes from the independently validated Rust traversal.
    if (R == 4) return reflectionReduced ? 246803167ull : 493573042ull;
    if (R == 5 && reflectionReduced) return 3359910526ull;
    return 0;
  }

  void makePaths() {
    char b[128];
    const char* suffix = reflectionReduced ? "_lr" : "";
    snprintf(b, sizeof b, "reach_work_%dx%d%s.bin", R, C, suffix); workPath = b;
    snprintf(b, sizeof b, "reach_frontier_%dx%d%s_0.bin", R, C, suffix); frontierPath[0] = b;
    snprintf(b, sizeof b, "reach_frontier_%dx%d%s_1.bin", R, C, suffix); frontierPath[1] = b;
    snprintf(b, sizeof b, "reach_frontier_%dx%d%s.tmp", R, C, suffix); frontierTmpPath = b;
  }

  static off_t checkedFileBytes(u64 records, size_t recordBytes, const char* what) {
    u128 n = (u128)records * recordBytes;
    if (n > (u128)std::numeric_limits<off_t>::max()) die("%s is too large", what);
    return (off_t)n;
  }

  void persistHeader() {
    if (msync(workMap, REACH_WORK_HDR_BYTES, MS_SYNC)) die("reach header msync failed");
  }

  void persistStates() {
    if (msync((u8*)states, stateBytes, MS_SYNC)) die("reach state msync failed");
  }

  static void writeInitialFrontier(const std::string& path, const PackedPos& p) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("cannot create %s", path.c_str());
    pwriteAllAt(fd, &p, sizeof p, 0, "initial frontier");
    if (fsync(fd)) die("initial frontier fsync failed");
    close(fd);
  }

  void validateFrontier(int slot, u64 count) const {
    struct stat st{};
    if (stat(frontierPath[slot].c_str(), &st))
      die("missing reach frontier %s", frontierPath[slot].c_str());
    off_t expected = checkedFileBytes(count, sizeof(PackedPos), "reach frontier");
    if (st.st_size != expected)
      die("reach frontier size mismatch: %s", frontierPath[slot].c_str());
  }

  // Convert all pending states (10b) either to committed (01b) or back to unseen (00b).
  u64 sweepPending(bool commit) {
    std::atomic<u64> count{0}, bad{0};
    std::vector<std::thread> pool;
    u64 per = (wh->stateWords + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
      u64 begin = (u64)t * per, end = std::min(wh->stateWords, begin + per), local = 0;
      for (u64 i = begin; i < end; ++i) {
        u64 w = states[i];
        u64 low = w & TABLE_LOW_BIT_MASK;
        u64 high = (w >> 1) & TABLE_LOW_BIT_MASK;
        if (high & low) bad.fetch_add(__builtin_popcountll(high & low),
                                      std::memory_order_relaxed);
        u64 pending = high & ~low;
        if (!pending) continue;
        local += __builtin_popcountll(pending);
        states[i] = commit ? ((w & ~(pending << 1)) | pending)
                           : (w & ~(pending << 1));
      }
      count.fetch_add(local, std::memory_order_relaxed);
    });
    for (auto& t : pool) t.join();
    if (bad.load()) die("reach work map contains invalid state 3");
    return count.load();
  }

  bool markPending(u64 rank) {
    u64* word = &states[rank / TABLE_POSITIONS_PER_WORD];
    int shift = (int)(rank % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
    u64 fieldMask = 3ull << shift, pending = 2ull << shift;
    u64 old = __atomic_load_n(word, __ATOMIC_RELAXED);
    for (;;) {
      if (old & fieldMask) return false;
      u64 desired = old | pending;
      if (__atomic_compare_exchange_n(word, &old, desired, false,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return true;
    }
  }

  struct FrontierWriter {
    int fd;
    std::atomic<u64>& records;
    std::vector<PackedPos> buf;

    FrontierWriter(int f, std::atomic<u64>& n) : fd(f), records(n) {
      buf.reserve(WRITER_RECORDS);
    }
    void add(const u8* arr) {
      buf.push_back(packPosition(arr));
      if (buf.size() == WRITER_RECORDS) flush();
    }
    void flush() {
      if (buf.empty()) return;
      u64 base = records.fetch_add(buf.size(), std::memory_order_relaxed);
      off_t off = checkedFileBytes(base, sizeof(PackedPos), "reach frontier offset");
      pwriteAllAt(fd, buf.data(), buf.size() * sizeof(PackedPos), off, "reach frontier");
      buf.clear();
    }
  };

  void expandPosition(const u8* arr, FrontierWriter& out) {
    if (gameAlreadyOver(arr)) return;
    int hb[HAND_TYPES], hw[HAND_TYPES];
    handCounts(arr, hb);
    whiteHand(arr, hb, hw);
    u8 fpos[MAXARR], child[MAXARR], mirrorBase[MAXARR], mirrorChild[MAXARR];
    u32 nodeAt[MAXARR + 1], mirrorNodeAt[MAXARR + 1];
    u64 kAt[MAXARR + 1], mirrorKAt[MAXARR + 1];
    buildFlip(arr, hw, fpos);
    Z.buildPrefix(fpos, nodeAt, kAt);
    memcpy(child, fpos, ARR);
    if (reflectionReduced) {
      mirrorPosition(fpos, mirrorBase);
      Z.buildPrefix(mirrorBase, mirrorNodeAt, mirrorKAt);
      memcpy(mirrorChild, mirrorBase, ARR);
    }

    for (int q = 0; q < S; ++q) {
      int s = arr[q];
      if (s < 1 || s > 5) continue;
      int f1 = flipSq_[q];
      for (u8 t : MOV[s][q]) {
        int tt = arr[t];
        if (tt >= 1 && tt <= 6) continue;  // own piece or terminal lion capture
        int ns = (s == 4 && rowOf_[t] == 0) ? 5 : s;
        int f2 = flipSq_[t];
        child[f1] = 0; child[f2] = (u8)(ns + 5);
        int first = std::min(f1, f2);
        const u8* indexed = child;
        u64 rank;
        int mf1 = 0, mf2 = 0;
        if (!reflectionReduced) {
          rank = Z.rankFrom(nodeAt[first], kAt[first], child);
        } else {
          mf1 = mirSq_[f1]; mf2 = mirSq_[f2];
          mirrorChild[mf1] = 0; mirrorChild[mf2] = (u8)(ns + 5);
          if (compareReflection(child) <= 0)
            rank = Z.rankFrom(nodeAt[first], kAt[first], child);
          else {
            indexed = mirrorChild;
            int mirrorFirst = std::min(mf1, mf2);
            rank = Z.rankFrom(mirrorNodeAt[mirrorFirst], mirrorKAt[mirrorFirst],
                              mirrorChild);
          }
        }
        if (rank == UINT64_MAX) die("reach child rank failed");
        if (markPending(rank)) out.add(indexed);
        child[f1] = fpos[f1]; child[f2] = fpos[f2];
        if (reflectionReduced) {
          mirrorChild[mf1] = mirrorBase[mf1]; mirrorChild[mf2] = mirrorBase[mf2];
        }
      }
    }
    for (int type = 0; type < HAND_TYPES; ++type) {
      if (!hb[type]) continue;
      u8 dropped = (u8)(2 + type + 5);
      for (int q = 0; q < S; ++q) {
        if (arr[q]) continue;
        int f1 = flipSq_[q];
        child[f1] = dropped;
        const u8* indexed = child;
        u64 rank;
        int mf1 = 0;
        if (!reflectionReduced) {
          rank = Z.rankFrom(nodeAt[f1], kAt[f1], child);
        } else {
          mf1 = mirSq_[f1]; mirrorChild[mf1] = dropped;
          if (compareReflection(child) <= 0)
            rank = Z.rankFrom(nodeAt[f1], kAt[f1], child);
          else {
            indexed = mirrorChild;
            rank = Z.rankFrom(mirrorNodeAt[mf1], mirrorKAt[mf1], mirrorChild);
          }
        }
        if (rank == UINT64_MAX) die("reach drop rank failed");
        if (markPending(rank)) out.add(indexed);
        child[f1] = fpos[f1];
        if (reflectionReduced) mirrorChild[mf1] = mirrorBase[mf1];
      }
    }
  }

  void recoverIfNeeded() {
    if (wh->phase == REACH_EXPANDING) {
      printf("[reach] recovering interrupted expansion at level %u\n", wh->level);
      u64 cleared = sweepPending(false);
      persistStates();
      unlink(frontierTmpPath.c_str());
      unlink(frontierPath[wh->nextSlot].c_str());
      wh->phase = REACH_READY; wh->nextCount = 0; wh->commitTotal = 0;
      persistHeader();
      printf("[reach] rolled back %s pending positions\n", commas(cleared).c_str());
    } else if (wh->phase == REACH_COMMITTING) {
      printf("[reach] finishing interrupted commit at level %u\n", wh->level + 1);
      finishCommit(true);
    } else if (wh->phase != REACH_READY) {
      die("unknown reach work phase %u", wh->phase);
    }
  }

  void finishCommit(bool recovering = false) {
    validateFrontier(wh->nextSlot, wh->nextCount);
    u64 converted = sweepPending(true);
    if ((!recovering && converted != wh->nextCount) || converted > wh->nextCount)
      die("reach commit mismatch: frontier=%" PRIu64 " pending=%" PRIu64,
          wh->nextCount, converted);
    persistStates();
    wh->reachableCount = wh->commitTotal;
    wh->frontierSlot = wh->nextSlot;
    wh->frontierCount = wh->nextCount;
    wh->nextCount = 0;
    wh->commitTotal = 0;
    ++wh->level;
    wh->phase = REACH_READY;
    persistHeader();
  }

  void expandOneLevel() {
    validateFrontier(wh->frontierSlot, wh->frontierCount);
    off_t currentBytes = checkedFileBytes(wh->frontierCount, sizeof(PackedPos),
                                          "reach frontier");
    int currentFd = open(frontierPath[wh->frontierSlot].c_str(), O_RDONLY);
    if (currentFd < 0) die("cannot open current reach frontier");
    const PackedPos* current = nullptr;
    if (currentBytes) {
      void* p = mmap(nullptr, (size_t)currentBytes, PROT_READ, MAP_SHARED, currentFd, 0);
      if (p == MAP_FAILED) die("reach frontier mmap failed");
      current = (const PackedPos*)p;
      madvise(p, (size_t)currentBytes, MADV_SEQUENTIAL);
    }

    int nextFd = open(frontierTmpPath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (nextFd < 0) die("cannot create next reach frontier");
    wh->nextSlot = 1u - wh->frontierSlot;
    wh->nextCount = 0;
    wh->phase = REACH_EXPANDING;
    persistHeader();

    double started = now_s();
    std::atomic<u64> cursor{0}, nextRecords{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
      (void)t;
      FrontierWriter writer(nextFd, nextRecords);
      u8 arr[MAXARR];
      for (;;) {
        u64 begin = cursor.fetch_add(FRONTIER_CHUNK, std::memory_order_relaxed);
        if (begin >= wh->frontierCount) break;
        u64 end = std::min(wh->frontierCount, begin + FRONTIER_CHUNK);
        for (u64 i = begin; i < end; ++i) {
          unpackPosition(current[i], arr);
          expandPosition(arr, writer);
        }
      }
      writer.flush();
    });
    for (auto& t : pool) t.join();
    u64 nextCount = nextRecords.load();
    off_t nextBytes = checkedFileBytes(nextCount, sizeof(PackedPos), "next reach frontier");
    if (ftruncate(nextFd, nextBytes)) die("next reach frontier truncate failed");
    if (fsync(nextFd)) die("next reach frontier fsync failed");
    close(nextFd);
    if (currentBytes) munmap((void*)current, (size_t)currentBytes);
    close(currentFd);
    if (rename(frontierTmpPath.c_str(), frontierPath[wh->nextSlot].c_str()))
      die("next reach frontier rename failed");
    syncCurrentDirectory();

    wh->nextCount = nextCount;
    wh->commitTotal = wh->reachableCount + nextCount;
    wh->phase = REACH_COMMITTING;
    persistHeader();
    u64 previousTotal = wh->reachableCount;
    finishCommit();
    printf("[reach level %3u] +%-13s = %-15s %7.1fs\n", wh->level,
           commas(nextCount).c_str(), commas(previousTotal + nextCount).c_str(),
           now_s() - started);
    fflush(stdout);
  }

  static u32 compressEvenBits(u64 x) {
    x &= TABLE_LOW_BIT_MASK;
    x = (x | (x >> 1)) & 0x3333333333333333ull;
    x = (x | (x >> 2)) & 0x0f0f0f0f0f0f0f0full;
    x = (x | (x >> 4)) & 0x00ff00ff00ff00ffull;
    x = (x | (x >> 8)) & 0x0000ffff0000ffffull;
    x = (x | (x >> 16)) & 0x00000000ffffffffull;
    return (u32)x;
  }

  void finalizeTable() {
    u64 expected = expectedReachableCount();
    if (expected && wh->reachableCount != expected)
      die("reachable count mismatch: got %s expected %s; table was not modified",
          commas(wh->reachableCount).c_str(), commas(expected).c_str());

    int fd = open(tablePath.c_str(), O_RDWR);
    if (fd < 0) die("cannot open completed WDL table %s", tablePath.c_str());
    u8 headerBytes[HDR_BYTES];
    preadAllAt(fd, headerBytes, HDR_BYTES, 0, "WDL header");
    Hdr h{}; memcpy(&h, headerBytes, sizeof h);
    if (!validTableHeader(h, Z.total, wh->stateWords, reflectionReduced) || !h.done)
      die("reachability requires a completed matching WDL table");
    if (tableHasReachability(h)) {
      if (h.reachableCount != wh->reachableCount) die("existing K-plane count mismatch");
      close(fd);
      printf("[reach] WDL table already contains a validated K plane\n");
      return;
    }

    u64 kwords = reachWordCount(Z.total);
    off_t reachOffset = (off_t)HDR_BYTES
                      + checkedFileBytes(wh->stateWords, sizeof(u64), "WDL plane");
    off_t finalBytes = reachOffset + checkedFileBytes(kwords, sizeof(u64), "K plane");
    struct stat st{};
    if (fstat(fd, &st)) die("WDL table stat failed");
    if (st.st_size < reachOffset) die("WDL table is truncated");
    if (st.st_size != reachOffset)
      printf("[reach] replacing an incomplete trailing K plane\n");
    if (ftruncate(fd, finalBytes)) die("WDL+K table truncate failed");

    std::atomic<u64> outputCursor{0}, setBits{0};
    std::vector<std::thread> pool;
    static constexpr u64 OUTPUT_WORDS = 1ull << 17;
    for (int t = 0; t < threads; ++t) pool.emplace_back([&, t] {
      (void)t;
      std::vector<u64> out;
      u64 localCount = 0;
      for (;;) {
        u64 begin = outputCursor.fetch_add(OUTPUT_WORDS, std::memory_order_relaxed);
        if (begin >= kwords) break;
        u64 end = std::min(kwords, begin + OUTPUT_WORDS);
        out.resize((size_t)(end - begin));
        for (u64 j = begin; j < end; ++j) {
          u64 a = 2 * j < wh->stateWords ? states[2 * j] : 0;
          u64 b = 2 * j + 1 < wh->stateWords ? states[2 * j + 1] : 0;
          // At a completed checkpoint all reachable fields are 01b; accepting either bit
          // here also makes finalization idempotent after a recovered commit.
          u32 lo = compressEvenBits(a | (a >> 1));
          u32 hi = compressEvenBits(b | (b >> 1));
          u64 bits = (u64)lo | (u64)hi << 32;
          out[(size_t)(j - begin)] = bits;
          localCount += __builtin_popcountll(bits);
        }
        pwriteAllAt(fd, out.data(), out.size() * sizeof(u64),
                    reachOffset + (off_t)(begin * sizeof(u64)), "K plane");
      }
      setBits.fetch_add(localCount, std::memory_order_relaxed);
    });
    for (auto& t : pool) t.join();
    if (setBits.load() != wh->reachableCount)
      die("K-plane bit count mismatch after writing");
    if (fsync(fd)) die("K-plane fsync failed");

    h.version = reflectionReduced ? TABLE_FORMAT_V3 : TABLE_FORMAT_V2;
    h.P = TABLE_FLAG_REACHABILITY
        | (reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0);
    h.reachWords = kwords;
    h.reachableCount = wh->reachableCount;
    h.reachDone = 1;
    memcpy(headerBytes, &h, sizeof h);
    pwriteAllAt(fd, headerBytes, HDR_BYTES, 0, "WDL+K header");
    if (fsync(fd)) die("WDL+K header fsync failed");
    close(fd);
    printf("[reach] appended K plane: %s reachable bits, table format version %u\n",
           commas(wh->reachableCount).c_str(), h.version);
  }

  bool completedTableAlreadyHasReach() {
    int fd = open(tablePath.c_str(), O_RDONLY);
    if (fd < 0) die("cannot open %s — run 'solve' first", tablePath.c_str());
    u8 hb[HDR_BYTES]; preadAllAt(fd, hb, sizeof hb, 0, "WDL header");
    close(fd);
    Hdr h{}; memcpy(&h, hb, sizeof h);
    if (!validTableHeader(h, Z.total, Z.total / TABLE_POSITIONS_PER_WORD
                                      + (Z.total % TABLE_POSITIONS_PER_WORD != 0),
                          reflectionReduced) || !h.done)
      die("reachability requires a completed matching WDL table");
    if (!tableHasReachability(h)) return false;
    printf("[reach] table already has K plane: %s reachable of %s positions\n",
           commas(h.reachableCount).c_str(), commas(h.N).c_str());
    return true;
  }

  void openOrCreateWork() {
    u64 stateWords = Z.total / TABLE_POSITIONS_PER_WORD
                   + (Z.total % TABLE_POSITIONS_PER_WORD != 0);
    stateBytes = (size_t)checkedFileBytes(stateWords, sizeof(u64), "reach work map");
    u128 totalMap = (u128)REACH_WORK_HDR_BYTES + stateBytes;
    if (totalMap > std::numeric_limits<size_t>::max()) die("reach work map is too large");
    workMapBytes = (size_t)totalMap;
    workFd = open(workPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (workFd < 0) die("cannot open %s", workPath.c_str());
    struct stat st{}; if (fstat(workFd, &st)) die("reach work stat failed");
    bool fresh = st.st_size == 0;
    if (fresh && ftruncate(workFd, (off_t)workMapBytes)) die("reach work truncate failed");
    if (!fresh && (u64)st.st_size != workMapBytes) die("reach work file size mismatch");
    void* p = mmap(nullptr, workMapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, workFd, 0);
    if (p == MAP_FAILED) die("reach work mmap failed");
    workMap = (u8*)p; wh = (ReachWorkHdr*)p;
    states = (u64*)(workMap + REACH_WORK_HDR_BYTES);
    madvise(states, stateBytes, MADV_HUGEPAGE);

    if (fresh) {
      memset(wh, 0, sizeof *wh);
      memcpy(wh->magic, "DOBURCHW", 8);
      wh->version = REACH_WORK_VERSION; wh->W = C; wh->L = R;
      wh->reserved = reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0;
      wh->phase = REACH_READY; wh->frontierSlot = 0;
      wh->N = Z.total; wh->stateWords = stateWords;
      u8 initial[MAXARR], scratch[MAXARR]; Solver::initialPosition(initial);
      const u8* indexed = reflectionRepresentative(initial, scratch, reflectionReduced);
      u64 rank = Z.rank(indexed);
      if (rank == UINT64_MAX) die("initial position rank failed");
      int shift = (int)(rank % TABLE_POSITIONS_PER_WORD) * TABLE_BITS_PER_POSITION;
      states[rank / TABLE_POSITIONS_PER_WORD] |= 1ull << shift;
      wh->frontierCount = 1; wh->reachableCount = 1;
      writeInitialFrontier(frontierPath[0], packPosition(indexed));
      unlink(frontierPath[1].c_str()); unlink(frontierTmpPath.c_str());
      persistStates(); persistHeader(); syncCurrentDirectory();
      printf("[reach] initialized resumable work map %s\n", workPath.c_str());
    } else {
      if (memcmp(wh->magic, "DOBURCHW", 8) || wh->version != REACH_WORK_VERSION
          || wh->W != (u32)C || wh->L != (u32)R || wh->N != Z.total
          || wh->stateWords != stateWords || wh->frontierSlot > 1 || wh->nextSlot > 1
          || wh->reserved != (reflectionReduced ? TABLE_FLAG_REFLECTION_REDUCED : 0))
        die("reach work file does not match this board/code");
      printf("[reach] resuming after level %u: %s reached, frontier %s\n", wh->level,
             commas(wh->reachableCount).c_str(), commas(wh->frontierCount).c_str());
      recoverIfNeeded();
    }
  }

  void closeWork() {
    if (workMap) { munmap(workMap, workMapBytes); workMap = nullptr; }
    if (workFd >= 0) { close(workFd); workFd = -1; }
  }

  void cleanupWorkFiles() {
    unlink(workPath.c_str()); unlink(frontierPath[0].c_str());
    unlink(frontierPath[1].c_str()); unlink(frontierTmpPath.c_str());
    syncCurrentDirectory();
  }

 public:
  Reachability(int nthreads, const std::string& path, bool reduced)
      : Z(reduced), reflectionReduced(reduced), threads(nthreads), tablePath(path) {
    makePaths();
  }
  ~Reachability() { closeWork(); }

  void run() {
    double started = now_s();
    Z.build();
    u64 dp = Zdd::dpCount();
    u64 expected = reflectionReduced
                 ? (dp + Zdd::dpMirrorFixedCount()) / 2 : dp;
    if (Z.total != expected) die("ZDD count crosscheck failed");
    printf("[reach] %dx%d, %d threads, %s universe %s positions\n", R, C, threads,
           reflectionReduced ? "reflection-orbit" : "full-rank",
           commas(Z.total).c_str());
    if (completedTableAlreadyHasReach()) { cleanupWorkFiles(); return; }
    openOrCreateWork();
    while (wh->frontierCount) expandOneLevel();
    printf("[reach] traversal complete: %s reachable positions\n",
           commas(wh->reachableCount).c_str());
    finalizeTable();
    closeWork();
    cleanupWorkFiles();
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
      "                       [--threads N] [--ckpt K] [arguments]\n"
      "       COMMAND: selftest | solve | reach | reach-audit | info |\n"
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
  bool reflectionReduced = false;
  std::string explicitTable;
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--ckpt" && i + 1 < argc) ckpt = atoi(argv[++i]);
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
      unpackPosition(packPosition(arr), arr2);
      assert(memcmp(arr, arr2, ARR) == 0);
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
    printf("[selftest] ALL OK\n");
  } else if (cmd == "solve") {
    Solver Sv(reflectionReduced);
    Sv.threads = threads;
    printf("[solve] %dx3 dobutsu shogi, %d threads, table %s\n", R, threads, tablePath.c_str());
    Sv.setup(tablePath);
    double t0 = now_s();
    Sv.solve(ckpt);
    printf("[solve] total time %.1fs\n", now_s() - t0);
  } else if (cmd == "reach") {
    Reachability reach(threads, tablePath, reflectionReduced);
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
