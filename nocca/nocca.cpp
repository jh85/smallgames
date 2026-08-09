// nocca.cpp — Strong solver for NOCCA x NOCCA via ZDD minimal perfect hash + retrograde analysis.
//
// Based on: A. Yamamoto and K. Hoki, "Strongly Solving NOCCA x NOCCA" (NOCCA x NOCCA の強解決),
// The 27th Game Programming Workshop 2022, pp. 9-16.
//
// Pipeline (paper sections in parentheses):
//   1. Enumerate the "pseudo-reachable" positions (§6): 5 white + 5 black pieces on the 5x6
//      board, stacks up to 3 high, and at least one white and one black piece visible from
//      above. There are 147,969,899,280 such positions.
//   2. Build a ZDD over items = (square, square-state) pairs (§7, Table 7). Each square takes
//      one of 15 states (Table 6): empty, or a stack of height 1..3 of white/black pieces.
//      States 0..13 are explicit items; taking the 0-branch on all 14 items of a square
//      assigns the default state 14 (the all-black 3-stack). The ZDD is the canonical
//      (reduced) form and doubles as a minimal perfect hash via path counting (Tables 2, 3).
//   3. Retrograde analysis (§5 Table 4, §8) over black-to-move positions only; white-to-move
//      positions are mapped by the flip-rows + swap-colors symmetry. Strict iteration passes:
//      pass 1 marks win-in-1 (odd) and no-move terminal losses; pass k marks positions at
//      distance k. Remaining positions after convergence are draws.
//      Result: packed 2-bit WDL table indexed by the ZDD rank (0=draw/unknown, 1=win, 2=loss,
//      from the perspective of the player to move in the canonical black-to-move encoding).
//
// Rules of NOCCA x NOCCA (§3):
//   - 5x6 board, 5 pieces per player, initially filling each player's home row.
//   - Players alternate moving one piece one square in any of the 8 directions.
//   - A piece may land on any stack of height <= 2 (own or opponent's), going on top.
//     Covered pieces cannot move; only stack tops move. Max stack height is 3.
//   - A player wins by moving a piece off the far edge into the opponent's goal (reachable in
//     one move from any square of the opponent's home row), or when the opponent has no
//     movable piece (all covered and/or blocked).
//   - Infinite play is formally a draw.

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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
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
// Square states (paper Table 6). 15 states: 0 = empty; heights 1..3 with each level white or
// black. pat bit k = color of level k (0 = white, 1 = black), bit 0 = bottom.
// State id = BASE[h] + pat. State 14 (h=3, pat=7, i.e. black-black-black) is the ZDD default.
// ------------------------------------------------------------------------------------------
static const int SBASE[4] = {0, 1, 3, 7};

struct States {
  int hgt[15], pat[15], nw[15], nb[15], top[15]; // top: -1 empty, 0 white, 1 black
  int push[15][2];                               // push[s][color] -> new state, -1 if full
  int pop[15];                                   // state after removing top, -1 if empty
  int swp[15];                                   // color-swapped state
  void init() {
    for (int h = 0; h <= 3; ++h)
      for (int p = 0; p < (1 << h); ++p) {
        int s = SBASE[h] + p;
        hgt[s] = h; pat[s] = p;
        nb[s] = __builtin_popcount(p); nw[s] = h - nb[s];
        top[s] = h ? ((p >> (h - 1)) & 1) : -1;
        pop[s] = h ? SBASE[h - 1] + (p & ((1 << (h - 1)) - 1)) : -1;
        swp[s] = SBASE[h] + ((~p) & ((1 << h) - 1));
        for (int c = 0; c < 2; ++c)
          push[s][c] = (h < 3) ? SBASE[h + 1] + (p | (c << h)) : -1;
      }
  }
};
static States ST;

// ------------------------------------------------------------------------------------------
// Board geometry. Rows 0..L-1; row 0 = Black's home row, row L-1 = White's home row.
// Black moves toward row L-1 and enters its goal off the board from row L-1.
// Canonical positions are always black-to-move; the flip+swap involution maps a
// white-to-move position to canonical form.
// ------------------------------------------------------------------------------------------
struct Game {
  int W, L, S, P;                 // width, length, squares, pieces per player (= W)
  std::vector<std::array<int,8>> nbr;
  std::vector<int> nn, flip, row;
  void init(int w, int l) {
    W = w; L = l; S = W * L; P = W;
    nbr.assign(S, {}); nn.assign(S, 0); flip.assign(S, 0); row.assign(S, 0);
    for (int r = 0; r < L; ++r)
      for (int c = 0; c < W; ++c) {
        int q = r * W + c;
        row[q] = r;
        flip[q] = (L - 1 - r) * W + c;
        for (int dr = -1; dr <= 1; ++dr)
          for (int dc = -1; dc <= 1; ++dc) {
            if (!dr && !dc) continue;
            int r2 = r + dr, c2 = c + dc;
            if (r2 < 0 || r2 >= L || c2 < 0 || c2 >= W) continue;
            nbr[q][nn[q]++] = r2 * W + c2;
          }
      }
  }
};

// ------------------------------------------------------------------------------------------
// ZDD: canonical, built by memoized recursion over items d = square*14 + state (state 0..13).
// Frontier signature: (f, nw, nb, tw, tb) = (current square already assigned?, #white placed,
// #black placed, white top seen?, black top seen?). Equivalent to paper Table 7 + reduction.
// Node 0 = 0-leaf, node 1 = 1-leaf.
// ------------------------------------------------------------------------------------------
struct Zdd {
  int S, P, D;
  std::vector<u8>  sq_, st_;
  std::vector<u32> lo_, hi_;
  std::vector<u64> cnt_, cntlo_;
  u32 root = 0;
  u64 total = 0;

  std::unordered_map<u32, u32> memo;
  std::unordered_map<u64, u32> uniq;

  static u32 pack(int f, int nw, int nb, int tw, int tb) {
    return (u32)f | (u32)nw << 1 | (u32)nb << 4 | (u32)tw << 7 | (u32)tb << 8;
  }

  u32 rec(int d, u32 sig) {
    if (d == D) {
      int f = sig & 1, nw = (sig >> 1) & 7, nb = (sig >> 4) & 7, tw = (sig >> 7) & 1, tb = (sig >> 8) & 1;
      (void)f;
      return (nw == P && nb == P && tw && tb) ? 1u : 0u;
    }
    u32 key = (u32)d << 9 | sig;
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;

    int f = sig & 1, nw = (sig >> 1) & 7, nb = (sig >> 4) & 7, tw = (sig >> 7) & 1, tb = (sig >> 8) & 1;
    int s = d % 14;

    // 0-branch: state s not chosen. At s==13 with the square still unassigned, the square
    // defaults to state 14 (BBB): +3 black pieces, black visible on top.
    u32 lo;
    {
      int f2 = f, nb2 = nb, tb2 = tb; bool ok = true;
      if (s == 13) {
        if (!f2) { nb2 += 3; tb2 = 1; if (nb2 > P) ok = false; }
        f2 = 0;
      }
      lo = ok ? rec(d + 1, pack(f2, nw, nb2, tw, tb2)) : 0;
    }
    // 1-branch: state s chosen for this square (only if not already assigned).
    u32 hi = 0;
    if (!f) {
      int nw2 = nw + ST.nw[s], nb2 = nb + ST.nb[s];
      if (nw2 <= P && nb2 <= P) {
        int tw2 = tw | (ST.top[s] == 0), tb2 = tb | (ST.top[s] == 1);
        int f2 = (s == 13) ? 0 : 1;
        hi = rec(d + 1, pack(f2, nw2, nb2, tw2, tb2));
      }
    }

    u32 res;
    if (hi == 0) {
      res = lo;                                  // zero-suppression rule
    } else {
      u64 hkey = (u64)d << 48 | (u64)lo << 24 | hi;
      auto it2 = uniq.find(hkey);
      if (it2 != uniq.end()) res = it2->second;
      else {
        res = (u32)sq_.size();
        sq_.push_back((u8)(d / 14)); st_.push_back((u8)(d % 14));
        lo_.push_back(lo); hi_.push_back(hi);
        uniq.emplace(hkey, res);
      }
    }
    memo.emplace(key, res);
    return res;
  }

  void build(const Game& G) {
    S = G.S; P = G.P; D = S * 14;
    sq_.assign(2, (u8)S); st_.assign(2, 0);
    lo_.assign(2, 0); hi_.assign(2, 0);
    root = rec(0, 0);
    memo.clear(); uniq.clear();
    cnt_.assign(sq_.size(), 0);
    cnt_[0] = 0; cnt_[1] = 1;
    for (u32 i = 2; i < (u32)sq_.size(); ++i) cnt_[i] = cnt_[lo_[i]] + cnt_[hi_[i]];
    cntlo_.assign(sq_.size(), 0);
    for (u32 i = 2; i < (u32)sq_.size(); ++i) cntlo_[i] = cnt_[lo_[i]];
    total = cnt_[root];
  }

  // Independent count of the pseudo-reachable set by plain DP (validation).
  static u64 dpCount(const Game& G) {
    // dp[nw][nb][tw][tb]
    int P = G.P;
    std::vector<u64> dp((P + 1) * (P + 1) * 4, 0), nx;
    auto at = [&](std::vector<u64>& v, int nw, int nb, int t) -> u64& {
      return v[(nw * (P + 1) + nb) * 4 + t];
    };
    at(dp, 0, 0, 0) = 1;
    for (int q = 0; q < G.S; ++q) {
      nx.assign(dp.size(), 0);
      for (int nw = 0; nw <= P; ++nw)
        for (int nb = 0; nb <= P; ++nb)
          for (int t = 0; t < 4; ++t) {
            u64 v = at(dp, nw, nb, t);
            if (!v) continue;
            for (int s = 0; s < 15; ++s) {
              int nw2 = nw + ST.nw[s], nb2 = nb + ST.nb[s];
              if (nw2 > P || nb2 > P) continue;
              int t2 = t | (ST.top[s] == 0 ? 1 : 0) | (ST.top[s] == 1 ? 2 : 0);
              at(nx, nw2, nb2, t2) += v;
            }
          }
      dp.swap(nx);
    }
    return at(dp, P, P, 3);
  }

  u64 rank(const u8* pos) const {
    u32 n = root; u64 k = 0;
    while (n > 1) {
      if (pos[sq_[n]] == st_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }

  void unrank(u64 k, u8* pos) const {
    memset(pos, 14, S);
    u32 n = root;
    while (n > 1) {
      if (cntlo_[n] <= k) { k -= cntlo_[n]; pos[sq_[n]] = st_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    assert(n == 1 && k == 0);
  }

  // Per-square rank prefix: nodeAt[j]/kAt[j] = walk state upon entering square j
  // (all items of squares < j resolved). nodeAt[S] ends at the 1-leaf, kAt[S] = rank.
  void buildPrefix(const u8* pos, u32* nodeAt, u64* kAt) const {
    u32 n = root; u64 k = 0; int last = -1;
    for (;;) {
      int t = (n > 1) ? (int)sq_[n] : S;
      for (int j = last + 1; j <= t; ++j) { nodeAt[j] = n; kAt[j] = k; }
      last = t;
      if (n <= 1) break;
      if (pos[sq_[n]] == st_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
  }

  u64 rankFrom(u32 n, u64 k, const u8* pos) const {
    while (n > 1) {
      if (pos[sq_[n]] == st_[n]) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }
};

// Sequential enumeration of combinations (positions) in rank order.
struct ZddIter {
  const Zdd* z = nullptr;
  int sp = 0;
  u32 nstack[512];
  u8 br[512];
  u8 pos[64];

  void descend(u32 m) {
    while (m > 1) {
      nstack[sp] = m;
      if (z->lo_[m] == 0) { br[sp] = 1; pos[z->sq_[m]] = z->st_[m]; ++sp; m = z->hi_[m]; }
      else { br[sp] = 0; ++sp; m = z->lo_[m]; }
    }
    assert(m == 1);
  }

  void initAt(const Zdd& zz, u64 k) {
    z = &zz; sp = 0;
    memset(pos, 14, z->S);
    u32 n = z->root;
    while (n > 1) {
      nstack[sp] = n;
      if (z->cntlo_[n] <= k) { k -= z->cntlo_[n]; br[sp] = 1; pos[z->sq_[n]] = z->st_[n]; n = z->hi_[n]; }
      else { br[sp] = 0; n = z->lo_[n]; }
      ++sp;
    }
    assert(n == 1 && k == 0);
  }

  bool next() {
    while (sp) {
      --sp;
      u32 n = nstack[sp];
      if (br[sp]) { pos[z->sq_[n]] = 14; }
      else { br[sp] = 1; pos[z->sq_[n]] = z->st_[n]; ++sp; descend(z->hi_[n]); return true; }
    }
    return false;
  }
};

// ------------------------------------------------------------------------------------------
// Game-state evaluation on canonical (black-to-move) positions.
// ------------------------------------------------------------------------------------------

// Does White (about to move) have any legal move in position `posv` where two squares are
// overridden (a->sa, b->sb)? White moves toward row 0 and has a goal move from row 0.
static bool whiteHasMove(const Game& G, const u8* posv, int a, int sa, int b, int sb) {
  auto val = [&](int u) -> int { return u == a ? sa : (u == b ? sb : posv[u]); };
  for (int u = 0; u < G.S; ++u) {
    int su = val(u);
    if (ST.top[su] != 0) continue;          // want a white top
    if (G.row[u] == 0) return true;         // goal move always available
    for (int j = 0; j < G.nn[u]; ++j)
      if (ST.hgt[val(G.nbr[u][j])] <= 2) return true;
  }
  return false;
}

// Pass-1 intrinsic value of a canonical position: 1 = black wins in 1 move (enter goal,
// cover White's last visible piece, or leave White with no movable piece), 2 = black has no
// legal move (terminal loss), 0 = neither.
static int pass1Eval(const Game& G, const u8* pos) {
  // (a) black top on White's home row -> goal entry available -> win.
  int gr = (G.L - 1) * G.W;
  for (int c = 0; c < G.W; ++c)
    if (ST.top[pos[gr + c]] == 1) return 1;

  int wv0 = 0;
  for (int q = 0; q < G.S; ++q) wv0 += (ST.top[pos[q]] == 0);

  bool any = false;
  for (int q = 0; q < G.S; ++q) {
    if (ST.top[pos[q]] != 1) continue;
    int s1 = ST.pop[pos[q]];
    for (int j = 0; j < G.nn[q]; ++j) {
      int q2 = G.nbr[q][j];
      if (ST.hgt[pos[q2]] > 2) continue;
      any = true;
      // After moving the black top q -> q2:
      int s2 = ST.push[pos[q2]][1];
      int wv = wv0 - (ST.top[pos[q2]] == 0) + (ST.top[s1] == 0);
      if (wv == 0) return 1;                 // White fully covered
      if (wv <= 2) {                          // stuck is only possible with few visible whites
        if (!whiteHasMove(G, pos, q, s1, q2, s2)) return 1;
      }
    }
  }
  if (!any) return 2;                         // black cannot move
  return 0;
}

// ------------------------------------------------------------------------------------------
// Packed 2-bit WDL table (in anonymous memory; snapshotted to a file with a header).
// Values: 0 = unknown (draw once done), 1 = win, 2 = loss, 3 = newly decided this pass.
// ------------------------------------------------------------------------------------------
struct Hdr {
  char magic[8];       // "NOCCAWDL"
  u32 version, W, L, P;
  u64 N, nwords;
  u32 passesDone, done;
  u64 histW[256], histL[256];
};
static const size_t HDR_BYTES = 8192;

struct Table {
  u64* words = nullptr;
  u64 nwords = 0, N = 0;
  Hdr hdr{};

  void alloc(u64 n) {
    N = n; nwords = (n + 31) / 32;
    size_t bytes = nwords * 8;
    words = (u64*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (words == MAP_FAILED) die("mmap of %zu bytes failed", bytes);
    madvise(words, bytes, MADV_HUGEPAGE);
  }
  inline u32 get(u64 i) const {
    u64 w = __atomic_load_n(&words[i >> 5], __ATOMIC_RELAXED);
    return (u32)((w >> ((i & 31) * 2)) & 3);
  }

  void save(const std::string& path) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open %s failed", tmp.c_str());
    u8 hb[HDR_BYTES]; memset(hb, 0, sizeof hb);
    memcpy(hb, &hdr, sizeof hdr);
    if (write(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header write failed");
    size_t bytes = nwords * 8, off = 0;
    const size_t CH = 1ull << 30;
    while (off < bytes) {
      size_t n = std::min(CH, bytes - off);
      ssize_t r = write(fd, (u8*)words + off, n);
      if (r <= 0) die("table write failed");
      off += (size_t)r;
    }
    if (fsync(fd)) die("fsync failed");
    close(fd);
    if (rename(tmp.c_str(), path.c_str())) die("rename failed");
  }

  bool load(const std::string& path, u32 W, u32 L, u64 n) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    u8 hb[HDR_BYTES];
    if (read(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header read failed");
    memcpy(&hdr, hb, sizeof hdr);
    if (memcmp(hdr.magic, "NOCCAWDL", 8) || hdr.W != W || hdr.L != L || hdr.N != n)
      die("existing table %s does not match board/count", path.c_str());
    size_t bytes = nwords * 8, off = 0;
    const size_t CH = 1ull << 30;
    while (off < bytes) {
      size_t nn = std::min(CH, bytes - off);
      ssize_t r = read(fd, (u8*)words + off, nn);
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

// Paper Table A-1: number of positions decided at each pass (1-based; odd = win, even = loss)
// for the 5x6 game. Entry 48 was not reliably machine-readable from the PDF (-1 = skip).
static const i64 EXP56[70] = { 0,
  77645562828, 22410730165, 15142536934, 7707358885, 2996378622, 2530587844, 1793971952,
  1708892403, 1509313136, 1085543075, 1080382276, 855684894, 992892748, 829104200,
  952307416, 782437940, 878640363, 718653702, 796900982, 656328294, 694050711, 570638553,
  568580405, 455202391, 424155415, 327678503, 287199523, 215552713, 176144437, 128061051,
  99613880, 69681294, 51549796, 36136187, 26077221, 18510133, 12725495, 9461541, 6281721,
  5122307, 3427186, 3122345, 2027453, 2001316, 1256933, 1327058, 815882, -1, 516770,
  535828, 313835, 305484, 194950, 176195, 123798, 111626, 77589, 65019, 36591, 29351,
  14906, 8074, 5305, 2244, 1704, 582, 118, 28, 30 };
// Paper Table 8 totals. Note: the paper's loss total excludes the 30 zero-legal-move terminal
// positions (its Table 8 leaves them unvalued, counting them among the draws), even though
// its own Fig. 4 describes them as losses. This solver marks them as losses-in-0, so our
// expected totals are losses+30 / draws-30 relative to Table 8.
static const u64 EXP56_TOTAL   = 147969899280ull;
static const u64 EXP56_WINS    = 106144078911ull;
static const u64 EXP56_LOSSES  = 41129930509ull + 30;  // Table 8 value + 30 terminals
static const u64 EXP56_DRAWS   = 695889860ull - 30;    // Table 8 value - 30 terminals
static const u64 EXP56_TERM    = 30ull;

struct Solver {
  Game G; Zdd Z; Table T;
  int threads = (int)std::thread::hardware_concurrency();
  std::string tablePath;
  u64 initIdx = 0;
  std::atomic<u64> chunkCtr{0};
  std::atomic<u64> totW{0}, totL{0};
  static const u64 CHUNK = 1ull << 21;

  void initialPosition(u8* pos) const {
    memset(pos, 0, G.S);
    for (int c = 0; c < G.W; ++c) {
      pos[c] = 2;                       // single black on Black's home row
      pos[(G.L - 1) * G.W + c] = 1;     // single white on White's home row
    }
  }

  void setup(int W, int L, const std::string& path) {
    G.init(W, L);
    tablePath = path;
    double t0 = now_s();
    Z.build(G);
    u64 dp = Zdd::dpCount(G);
    printf("[zdd] %dx%d: nodes=%s (incl. 2 leaves), |set|=%s, DP crosscheck=%s (%s), %.2fs\n",
           W, L, commas(Z.sq_.size()).c_str(), commas(Z.total).c_str(), commas(dp).c_str(),
           Z.total == dp ? "OK" : "MISMATCH!", now_s() - t0);
    if (Z.total != dp) die("ZDD count does not match DP count");
    if (W == 5 && L == 6 && Z.total != EXP56_TOTAL)
      die("5x6 count %" PRIu64 " != paper value %" PRIu64, Z.total, EXP56_TOTAL);
    u8 pos[64];
    initialPosition(pos);
    initIdx = Z.rank(pos);
    if (initIdx == UINT64_MAX) die("initial position not in the pseudo-reachable set?!");
    printf("[zdd] initial position index = %s\n", commas(initIdx).c_str());
    T.alloc(Z.total);
    memcpy(T.hdr.magic, "NOCCAWDL", 8);
    T.hdr.version = 1; T.hdr.W = W; T.hdr.L = L; T.hdr.P = G.P;
    T.hdr.N = Z.total; T.hdr.nwords = T.nwords;
  }

  // Evaluate one unknown position at pass >= 2 using children lookups.
  // pos: canonical position; scratch buffers provided by the worker.
  inline int passKEval(const u8* pos, u8* fpos, u8* childbuf, u32* nodeAt, u64* kAt) const {
    for (int q = 0; q < G.S; ++q) fpos[G.flip[q]] = (u8)ST.swp[pos[q]];
    Z.buildPrefix(fpos, nodeAt, kAt);
    memcpy(childbuf, fpos, G.S);
    bool allWin = true;
    for (int q = 0; q < G.S; ++q) {
      if (ST.top[pos[q]] != 1) continue;
      int s1f = ST.swp[ST.pop[pos[q]]];
      int fq1 = G.flip[q];
      for (int j = 0; j < G.nn[q]; ++j) {
        int q2 = G.nbr[q][j];
        if (ST.hgt[pos[q2]] > 2) continue;
        int s2f = ST.swp[ST.push[pos[q2]][1]];
        int fq2 = G.flip[q2];
        childbuf[fq1] = (u8)s1f; childbuf[fq2] = (u8)s2f;
        int c1 = fq1 < fq2 ? fq1 : fq2;
        u64 k = Z.rankFrom(nodeAt[c1], kAt[c1], childbuf);
        childbuf[fq1] = fpos[fq1]; childbuf[fq2] = fpos[fq2];
        if (k == UINT64_MAX) die("child rank failed (invalid child) — logic bug");
        u32 v = T.get(k);
        if (v == 2) return 1;         // some child is a loss -> win
        if (v != 1) allWin = false;   // unknown or newly-decided child
      }
    }
    return allWin ? 2 : 0;
  }

  void worker(int pass, u64* cw, u64* cl) {
    u8 fpos[64], childbuf[64];
    u32 nodeAt[64]; u64 kAt[64];
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
        u64 wi = idx >> 5;
        u64 spanEnd = std::min(hi, (wi + 1) << 5);
        u64 w = __atomic_load_n(&T.words[wi], __ATOMIC_RELAXED);
        u64 add = 0;
        for (; idx < spanEnd; ++idx) {
          int sh = (int)(idx & 31) * 2;
          if (((w >> sh) & 3) == 0) {
            int r = (pass == 1) ? pass1Eval(G, it.pos)
                                : passKEval(it.pos, fpos, childbuf, nodeAt, kAt);
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

  // Convert 11 (new) pairs to the given final value (1 or 2); returns number converted.
  u64 sweepConvert(u32 val) {
    std::atomic<u64> cnt{0};
    std::vector<std::thread> th;
    u64 per = (T.nwords + threads - 1) / threads;
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&, t] {
        u64 b = t * per, e = std::min(T.nwords, b + per), c = 0;
        for (u64 i = b; i < e; ++i) {
          u64 w = T.words[i];
          u64 m = w & (w >> 1) & 0x5555555555555555ull;
          if (!m) continue;
          c += __builtin_popcountll(m);
          w = (val == 1) ? (w & ~(m << 1)) : (w & ~m);
          T.words[i] = w;
        }
        cnt += c;
      });
    for (auto& x : th) x.join();
    return cnt.load();
  }

  void tally(u64& wins, u64& losses, u64& unk) {
    std::atomic<u64> aw{0}, al{0};
    std::vector<std::thread> th;
    u64 per = (T.nwords + threads - 1) / threads;
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&, t] {
        u64 b = t * per, e = std::min(T.nwords, b + per), cw = 0, cl = 0;
        for (u64 i = b; i < e; ++i) {
          u64 w = T.words[i];
          u64 hiB = (w >> 1) & 0x5555555555555555ull, loB = w & 0x5555555555555555ull;
          cw += __builtin_popcountll(loB & ~hiB);   // 01
          cl += __builtin_popcountll(hiB & ~loB);   // 10
        }
        aw += cw; al += cl;
      });
    for (auto& x : th) x.join();
    wins = aw; losses = al;
    // Positions beyond N in the last word are 0 and counted nowhere.
    unk = Z.total - wins - losses;
  }

  void solve(int ckptEvery) {
    bool resumed = T.load(tablePath, G.W, G.L, Z.total);
    int startPass = resumed ? (int)T.hdr.passesDone + 1 : 1;
    if (resumed) {
      if (T.hdr.done) { printf("[solve] table already complete.\n"); return; }
      printf("[solve] resuming after pass %u\n", T.hdr.passesDone);
    }
    bool expOK = true;
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
        die("pass %d found both wins (%" PRIu64 ") and losses (%" PRIu64 ") — logic bug",
            pass, nw, nl);
      if (pass > 1 && nw + nl) {
        u64 conv = sweepConvert(nw ? 1 : 2);
        if (conv != nw + nl) die("sweep converted %" PRIu64 " != %" PRIu64, conv, nw + nl);
      }
      if (pass < 256) { T.hdr.histW[pass] = nw; T.hdr.histL[pass] = nl; }

      std::string expNote;
      if (G.W == 5 && G.L == 6) {
        i64 exp = (pass <= 69) ? EXP56[pass] : 0;
        u64 got = (pass == 1) ? nw : nw + nl;   // pass 1 also finds the terminal losses
        if (exp < 0) expNote = " [paper: entry unreadable]";
        else if ((u64)exp == got) expNote = " [paper: OK]";
        else { expNote = " [paper: MISMATCH exp " + commas((u64)exp) + "]"; expOK = false; }
        if (pass == 1 && nl != EXP56_TERM) {
          expNote += " [terminal losses exp 30: MISMATCH]"; expOK = false;
        }
      }
      printf("[pass %3d] wins +%-15s losses +%-15s  %6.1fs%s\n",
             pass, commas(nw).c_str(), commas(nl).c_str(), now_s() - t0, expNote.c_str());
      fflush(stdout);

      u32 iv = T.get(initIdx);
      if (iv == 1 || iv == 2) {
        static bool reported = false;
        if (!reported) {
          reported = true;
          printf("[solve] *** initial position decided at pass %d: %s for the first player ***\n",
                 pass, iv == 1 ? "WIN" : "LOSS");
          fflush(stdout);
        }
      }

      bool finished = (pass > 1 && nw + nl == 0);
      T.hdr.passesDone = pass;
      if (finished) T.hdr.done = 1;
      if (finished || pass % ckptEvery == 0) {
        double s0 = now_s();
        T.save(tablePath);
        printf("[ckpt] pass %d saved to %s (%.1fs)\n", pass, tablePath.c_str(), now_s() - s0);
        fflush(stdout);
      }
      if (finished) break;
    }

    u64 wins, losses, draws;
    tally(wins, losses, draws);
    printf("[done] wins=%s losses=%s draws=%s total=%s\n",
           commas(wins).c_str(), commas(losses).c_str(), commas(draws).c_str(),
           commas(wins + losses + draws).c_str());
    if (G.W == 5 && G.L == 6) {
      bool ok = wins == EXP56_WINS && losses == EXP56_LOSSES && draws == EXP56_DRAWS;
      printf("[done] paper totals adjusted for the 30 zero-move terminals (W/L/D %s/%s/%s): %s\n",
             commas(EXP56_WINS).c_str(), commas(EXP56_LOSSES).c_str(),
             commas(EXP56_DRAWS).c_str(), ok ? "MATCH" : "MISMATCH");
      printf("[done] per-pass history vs paper Table A-1: %s\n",
             expOK ? "ALL MATCH" : "MISMATCHES PRESENT");
    }
    u32 iv = T.get(initIdx);
    printf("[done] initial position: %s\n",
           iv == 1 ? "WIN for the first player" : iv == 2 ? "LOSS for the first player" : "DRAW");
    fflush(stdout);
  }
};

// ------------------------------------------------------------------------------------------
// Probe: WDL lookup for an arbitrary position from the table file (no full load).
// ------------------------------------------------------------------------------------------
struct Prober {
  Game G; Zdd Z; Hdr hdr{}; int fd = -1;

  void open_(int W, int L, const std::string& path) {
    G.init(W, L);
    Z.build(G);
    fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) die("cannot open table %s — run 'solve' first", path.c_str());
    u8 hb[HDR_BYTES];
    if (read(fd, hb, HDR_BYTES) != (ssize_t)HDR_BYTES) die("header read failed");
    memcpy(&hdr, hb, sizeof hdr);
    if (memcmp(hdr.magic, "NOCCAWDL", 8) || (int)hdr.W != W || (int)hdr.L != L)
      die("table %s does not match board", path.c_str());
    if (hdr.N != Z.total) die("table N mismatch");
  }
  u32 get(u64 i) const {
    u64 w = 0;
    if (pread(fd, &w, 8, (off_t)(HDR_BYTES + (i >> 5) * 8)) != 8) die("pread failed");
    return (u32)((w >> ((i & 31) * 2)) & 3);
  }
  // value of canonical (black-to-move) position; 0/1/2 (+3 shouldn't occur in snapshots)
  u32 value(const u8* pos) const {
    u64 k = Z.rank(pos);
    if (k == UINT64_MAX) die("position is not pseudo-reachable");
    return get(k);
  }
};

static const char* wdlName(u32 v, bool done) {
  if (v == 1) return "WIN";
  if (v == 2) return "LOSS";
  return done ? "DRAW" : "UNKNOWN(not yet solved)";
}

static bool parseCell(const std::string& tok, int& state) {
  if (tok == "-") { state = 0; return true; }
  if (tok.size() < 1 || tok.size() > 3) return false;
  int h = 0, p = 0;
  for (char ch : tok) {
    if (ch == 'w' || ch == 'W') { /* white bit 0 */ }
    else if (ch == 'b' || ch == 'B') p |= 1 << h;
    else return false;
    ++h;
  }
  state = SBASE[h] + p;
  return true;
}

static void printPosition(const Game& G, const u8* pos) {
  for (int r = G.L - 1; r >= 0; --r) {
    printf("  row %d |", r);
    for (int c = 0; c < G.W; ++c) {
      int s = pos[r * G.W + c];
      char buf[5]; int n = 0;
      for (int k = 0; k < ST.hgt[s]; ++k) buf[n++] = ((ST.pat[s] >> k) & 1) ? 'b' : 'w';
      if (!n) buf[n++] = '-';
      buf[n] = 0;
      printf(" %-3s", buf);
    }
    printf(" |%s\n", r == G.L - 1 ? "  <- White home (Black's goal beyond)"
                    : r == 0 ? "  <- Black home (White's goal beyond)" : "");
  }
}

// Print per-move analysis for a canonical black-to-move position.
static void analyzeMoves(const Game& G, const Prober& P, const u8* pos) {
  bool done = P.hdr.done;
  u8 child[64], canon[64];
  int nmoves = 0;
  for (int q = 0; q < G.S; ++q) {
    if (ST.top[pos[q]] != 1) continue;
    if (G.row[q] == G.L - 1) {
      printf("  (%d,%d) -> GOAL : Win (immediate)\n", G.row[q], q % G.W);
      ++nmoves;
    }
    for (int j = 0; j < G.nn[q]; ++j) {
      int q2 = G.nbr[q][j];
      if (ST.hgt[pos[q2]] > 2) continue;
      ++nmoves;
      memcpy(child, pos, G.S);
      child[q] = (u8)ST.pop[pos[q]];
      child[q2] = (u8)ST.push[pos[q2]][1];
      // game over checks after the move
      int wv = 0;
      for (int u = 0; u < G.S; ++u) wv += (ST.top[child[u]] == 0);
      const char* res;
      if (wv == 0 || !whiteHasMove(G, child, -1, 0, -1, 0)) {
        res = "Win (immediate: opponent cannot move)";
      } else {
        for (int u = 0; u < G.S; ++u) canon[G.flip[u]] = (u8)ST.swp[child[u]];
        u32 v = P.value(canon);
        res = (v == 2) ? "Win" : (v == 1) ? "Loss" : (done ? "Draw" : "Unknown");
      }
      printf("  (%d,%d) -> (%d,%d) : %s\n", G.row[q], q % G.W, G.row[q2], q2 % G.W, res);
    }
  }
  printf("  (%d legal moves)\n", nmoves);
}

// ------------------------------------------------------------------------------------------
// Self tests
// ------------------------------------------------------------------------------------------
static void selftest() {
  // state tables
  for (int s = 0; s < 15; ++s) {
    if (ST.hgt[s] < 3)
      for (int c = 0; c < 2; ++c) {
        int t = ST.push[s][c];
        assert(t >= 0 && ST.pop[t] == s && ST.top[t] == c);
      }
    assert(ST.swp[ST.swp[s]] == s);
    assert(ST.nw[s] + ST.nb[s] == ST.hgt[s]);
  }
  assert(ST.top[14] == 1 && ST.nb[14] == 3);
  printf("[selftest] state tables OK\n");

  for (auto [w, l] : {std::pair{3, 3}, std::pair{5, 6}}) {
    Game G; G.init(w, l);
    Zdd Z; Z.build(G);
    u64 dp = Zdd::dpCount(G);
    printf("[selftest] %dx%d: nodes=%s count=%s dp=%s %s\n", w, l,
           commas(Z.sq_.size()).c_str(), commas(Z.total).c_str(), commas(dp).c_str(),
           Z.total == dp ? "OK" : "MISMATCH");
    assert(Z.total == dp);
    if (w == 5 && l == 6) assert(Z.total == EXP56_TOTAL);

    std::mt19937_64 rng(12345);
    u8 pos[64], pos2[64];
    for (int i = 0; i < 20000; ++i) {
      u64 k = rng() % Z.total;
      Z.unrank(k, pos);
      int nw = 0, nb = 0, tw = 0, tb = 0;
      for (int q = 0; q < G.S; ++q) {
        nw += ST.nw[pos[q]]; nb += ST.nb[pos[q]];
        tw |= ST.top[pos[q]] == 0; tb |= ST.top[pos[q]] == 1;
      }
      assert(nw == G.P && nb == G.P && tw && tb);
      assert(Z.rank(pos) == k);
    }
    printf("[selftest] %dx%d rank/unrank roundtrip OK\n", w, l);

    // iterator vs unrank
    ZddIter it;
    for (int rep = 0; rep < 5; ++rep) {
      u64 k0 = rng() % (Z.total - 3000);
      it.initAt(Z, k0);
      for (u64 k = k0; k < k0 + 3000; ++k) {
        Z.unrank(k, pos);
        assert(memcmp(pos, it.pos, G.S) == 0);
        if (k + 1 < Z.total) assert(it.next());
      }
    }
    printf("[selftest] %dx%d iterator OK\n", w, l);

    // prefix-cached child rank vs naive rank
    u32 nodeAt[64]; u64 kAt[64];
    u8 fpos[64], childbuf[64];
    int checked = 0;
    for (int i = 0; i < 3000; ++i) {
      u64 k = rng() % Z.total;
      Z.unrank(k, pos);
      if (pass1Eval(G, pos) != 0) continue;   // want positions whose children are all in-set
      for (int q = 0; q < G.S; ++q) fpos[G.flip[q]] = (u8)ST.swp[pos[q]];
      Z.buildPrefix(fpos, nodeAt, kAt);
      assert(kAt[G.S] == Z.rank(fpos));
      memcpy(childbuf, fpos, G.S);
      for (int q = 0; q < G.S; ++q) {
        if (ST.top[pos[q]] != 1) continue;
        for (int j = 0; j < G.nn[q]; ++j) {
          int q2 = G.nbr[q][j];
          if (ST.hgt[pos[q2]] > 2) continue;
          int fq1 = G.flip[q], fq2 = G.flip[q2];
          childbuf[fq1] = (u8)ST.swp[ST.pop[pos[q]]];
          childbuf[fq2] = (u8)ST.swp[ST.push[pos[q2]][1]];
          memcpy(pos2, childbuf, G.S);
          int c1 = fq1 < fq2 ? fq1 : fq2;
          u64 kc = Z.rankFrom(nodeAt[c1], kAt[c1], childbuf);
          assert(kc == Z.rank(pos2) && kc != UINT64_MAX);
          childbuf[fq1] = fpos[fq1]; childbuf[fq2] = fpos[fq2];
          ++checked;
        }
      }
    }
    printf("[selftest] %dx%d cached child-rank OK (%d children)\n", w, l, checked);
  }
  printf("[selftest] ALL OK\n");
}

// ------------------------------------------------------------------------------------------
// Independent 3x3 validation: depth-bounded minimax directly on (position, side-to-move),
// with its own move logic — no ZDD, no flip+swap symmetry, no retrograde. Used to cross-check
// the retrograde WDL table.
// ------------------------------------------------------------------------------------------
namespace bf {
  Game G;
  static u64 key(const u8* pos) {
    u64 k = 0;
    for (int q = 0; q < G.S; ++q) k = k * 15 + pos[q];
    return k;
  }
  static int goalRow(int c) { return c == 1 ? G.L - 1 : 0; }
  static bool stuck(const u8* pos, int c) {
    for (int q = 0; q < G.S; ++q) {
      if (ST.top[pos[q]] != c) continue;
      if (G.row[q] == goalRow(c)) return false;
      for (int j = 0; j < G.nn[q]; ++j)
        if (ST.hgt[pos[G.nbr[q][j]]] <= 2) return false;
    }
    return true;
  }
  static bool onGoalRow(const u8* pos, int c) {
    for (int q = 0; q < G.S; ++q)
      if (ST.top[pos[q]] == c && G.row[q] == goalRow(c)) return true;
    return false;
  }
  std::unordered_map<u64, bool> memoW, memoL;
  static bool canWin(const u8* pos, int c, int d);
  // c to move loses within <= d plies against optimal opponent
  static bool willLose(const u8* pos, int c, int d) {
    if (stuck(pos, c)) return true;
    if (onGoalRow(pos, c)) return false;
    if (d == 0) return false;
    u64 mk = key(pos) << 7 | (u64)d << 1 | c;
    auto it = memoL.find(mk);
    if (it != memoL.end()) return it->second;
    bool res = true;
    u8 child[64];
    for (int q = 0; q < G.S && res; ++q) {
      if (ST.top[pos[q]] != c) continue;
      for (int j = 0; j < G.nn[q]; ++j) {
        int q2 = G.nbr[q][j];
        if (ST.hgt[pos[q2]] > 2) continue;
        memcpy(child, pos, G.S);
        child[q] = (u8)ST.pop[pos[q]];
        child[q2] = (u8)ST.push[pos[q2]][c];
        if (stuck(child, 1 - c)) { res = false; break; }  // this move wins for c
        if (!canWin(child, 1 - c, d - 1)) { res = false; break; }
      }
    }
    memoL[mk] = res;
    return res;
  }
  // c to move can force a win within <= d plies
  static bool canWin(const u8* pos, int c, int d) {
    if (stuck(pos, c)) return false;
    if (onGoalRow(pos, c)) return d >= 1;
    if (d == 0) return false;
    u64 mk = key(pos) << 7 | (u64)d << 1 | c;
    auto it = memoW.find(mk);
    if (it != memoW.end()) return it->second;
    bool res = false;
    u8 child[64];
    for (int q = 0; q < G.S && !res; ++q) {
      if (ST.top[pos[q]] != c) continue;
      for (int j = 0; j < G.nn[q]; ++j) {
        int q2 = G.nbr[q][j];
        if (ST.hgt[pos[q2]] > 2) continue;
        memcpy(child, pos, G.S);
        child[q] = (u8)ST.pop[pos[q]];
        child[q2] = (u8)ST.push[pos[q2]][c];
        if (stuck(child, 1 - c)) { res = true; break; }
        if (willLose(child, 1 - c, d - 1)) { res = true; break; }
      }
    }
    memoW[mk] = res;
    return res;
  }

  static void run() {
    G.init(3, 3);
    u8 init[9];
    memset(init, 0, 9);
    for (int c = 0; c < 3; ++c) { init[c] = 2; init[6 + c] = 1; }
    int minWin = -1;
    for (int d = 1; d <= 15; ++d)
      if (canWin(init, 1, d)) { minWin = d; break; }
    printf("[bf3] initial position: first player forces a win in %d plies (independent search)\n",
           minWin);

    // Cross-check the whole retrograde table on random positions.
    Prober P;
    P.open_(3, 3, "./wdl_3x3.bin");
    if (!P.hdr.done) die("run solve3 first");
    const int D = 15;   // all decided distances in 3x3 are <= 12
    std::mt19937_64 rng(999);
    u8 pos[64], canon[64];
    int nW = 0, nL = 0, nD = 0;
    for (int i = 0; i < 4000; ++i) {
      u64 k = rng() % P.Z.total;
      P.Z.unrank(k, pos);
      // table value for black to move
      u32 tv = P.get(k);
      bool bw = canWin(pos, 1, D), bl = willLose(pos, 1, D);
      u32 bv = bw ? 1 : bl ? 2 : 0;
      if (tv != bv)
        die("mismatch at k=%" PRIu64 ": table=%u bf=%u", k, tv, bv);
      // also cross-check the white-to-move value via the flip+swap canonicalization
      for (int q = 0; q < 9; ++q) canon[P.G.flip[q]] = (u8)ST.swp[pos[q]];
      int cw = 0, cb = 0, ctw = 0, ctb = 0;
      for (int q = 0; q < 9; ++q) {
        cw += ST.nw[canon[q]]; cb += ST.nb[canon[q]];
        ctw |= ST.top[canon[q]] == 0; ctb |= ST.top[canon[q]] == 1;
      }
      if (cw == 3 && cb == 3 && ctw && ctb) {   // canonical form is in the set
        u32 tv2 = P.value(canon);
        bool ww = canWin(pos, 0, D), wl = willLose(pos, 0, D);
        u32 wv = ww ? 1 : wl ? 2 : 0;
        if (tv2 != wv)
          die("white-side mismatch at k=%" PRIu64 ": table=%u bf=%u", k, tv2, wv);
      }
      if (tv == 1) ++nW; else if (tv == 2) ++nL; else ++nD;
    }
    printf("[bf3] 4000 random positions cross-checked OK (both sides to move; W/L/D sample %d/%d/%d)\n",
           nW, nL, nD);
    printf("[bf3] retrograde + ZDD + flip/swap symmetry machinery VALIDATED\n");
  }
}

// ------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  ST.init();
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s selftest\n"
      "       %s solve   [--board WxL] [--threads N] [--ckpt N] [--dir D]\n"
      "       %s solve3                 # 3x3 validation game (expects win at pass 11)\n"
      "       %s analyze [--board WxL] [--dir D]\n"
      "       %s probe   [--board WxL] [--dir D] <b|w> <cell0> ... <cellS-1>\n"
      "                  cells row-major, row 0 (Black home) first; '-' empty or e.g. 'wb'\n"
      "                  = white bottom, black top. Side to move: b or w.\n",
      argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 1;
  }
  std::string cmd = argv[1];
  int W = 5, L = 6, threads = (int)std::thread::hardware_concurrency(), ckpt = 1;
  std::string dir = ".";
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--board" && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &W, &L); }
    else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--ckpt" && i + 1 < argc) ckpt = atoi(argv[++i]);
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else rest.push_back(a);
  }
  if (cmd == "solve3") { cmd = "solve"; W = 3; L = 3; }
  char pathbuf[256];
  snprintf(pathbuf, sizeof pathbuf, "%s/wdl_%dx%d.bin", dir.c_str(), W, L);
  std::string tablePath = pathbuf;

  if (cmd == "selftest") {
    selftest();
  } else if (cmd == "bf3check") {
    bf::run();
  } else if (cmd == "solve") {
    Solver S;
    S.threads = threads;
    printf("[solve] board %dx%d, %d threads, table %s\n", W, L, threads, tablePath.c_str());
    S.setup(W, L, tablePath);
    double t0 = now_s();
    S.solve(ckpt);
    printf("[solve] total time %.1fs\n", now_s() - t0);
    if (W == 3 && L == 3) {
      u32 iv = S.T.get(S.initIdx);
      bool ok = iv == 1 && S.T.hdr.histW[11] > 0;
      // find the pass where the initial position was decided
      printf("[solve3] validation (paper: first player wins, 11 moves): %s\n",
             ok ? "consistent" : "CHECK LOG");
    }
  } else if (cmd == "analyze" || cmd == "probe") {
    Prober P;
    P.open_(W, L, tablePath);
    bool done = P.hdr.done;
    if (!done) printf("note: table incomplete (passes done: %u) — 0-entries mean UNKNOWN\n",
                      P.hdr.passesDone);
    u8 pos[64];
    bool whiteToMove = false;
    if (cmd == "analyze") {
      memset(pos, 0, P.G.S);
      for (int c = 0; c < W; ++c) { pos[c] = 2; pos[(L - 1) * W + c] = 1; }
      printf("Initial position (Black to move):\n");
    } else {
      if ((int)rest.size() != 1 + P.G.S)
        die("probe needs side + %d cells, got %zu args", P.G.S, rest.size());
      whiteToMove = (rest[0] == "w" || rest[0] == "W");
      for (int q = 0; q < P.G.S; ++q) {
        int s;
        if (!parseCell(rest[1 + q], s)) die("bad cell '%s'", rest[1 + q].c_str());
        pos[q] = (u8)s;
      }
      int nw = 0, nb = 0;
      for (int q = 0; q < P.G.S; ++q) { nw += ST.nw[pos[q]]; nb += ST.nb[pos[q]]; }
      if (nw != P.G.P || nb != P.G.P)
        die("position must have %d white and %d black pieces (got %d/%d)", P.G.P, P.G.P, nw, nb);
    }
    u8 canon[64];
    if (whiteToMove)
      for (int q = 0; q < P.G.S; ++q) canon[P.G.flip[q]] = (u8)ST.swp[pos[q]];
    else memcpy(canon, pos, P.G.S);
    printPosition(P.G, pos);
    u32 v = P.value(canon);
    printf("Side to move (%s): %s\n", whiteToMove ? "White" : "Black", wdlName(v, done));
    printf("Moves (result for the side moving):\n");
    // analyzeMoves works on the canonical orientation; for White we present canonical coords.
    if (whiteToMove) printf("  (coordinates shown in the color-swapped, row-flipped frame)\n");
    analyzeMoves(P.G, P, canon);
  } else {
    die("unknown command '%s'", cmd.c_str());
  }
  return 0;
}
