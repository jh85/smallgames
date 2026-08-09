// c4.cpp — Strong solver for Connect Four (7x6 and smaller) via a per-ply ZDD minimal
// perfect hash + single-pass backward retrograde analysis, producing a packed 2-bit WDL
// table. Companion to the NOCCA x NOCCA and Dobutsu Shogi solvers in this repository.
//
// Position space ("pseudo-legal non-terminal", per ply k):
//   - W columns x H rows, gravity: each column is a stack of 0..H stones.
//   - k stones total, ceil(k/2) belong to X (first player; X moves on even plies).
//   - NO four-in-a-row anywhere (vertical, horizontal, diagonal).
// This is a superset of the reachable non-terminal positions (a final position without a
// 4-row can never have had a transient 4-row, since stones are never removed; the only
// unreachable members are those admitting no alternating-color placement order).
// Terminal positions (with a 4-row) and full-board draws need no table entries: a probe
// recognizes them directly. Children of pseudo positions are either immediate wins or
// pseudo positions at ply k+1, so a single backward sweep k = W*H-1 .. 0 solves the game
// with only two ply-slabs in memory (Connect Four is acyclic in the stone count).
//
// Encoding: a column with h stones and color pattern p (bit i = 1 if cell i, from the
// bottom, is X) has state id BASE[h] + p, BASE[h] = 2^h - 1; NS = 2^(H+1) - 1 states.
// States containing a vertical 4-run are globally excluded ("valid" states).
// The ZDD items are the SB = ceil(log2(NS+1)) bits of each column state, MSB first,
// column by column; a position is the set of 1-bits of its column states.
// The construction frontier entering column c is (window = states of columns c-3..c-1,
// r = stones still needed, rx = X stones still needed); all 43 ply-roots share one node
// pool because completions depend only on (window, r, rx).
//
// WDL semantics: 2-bit value for the SIDE TO MOVE at slab[ply]: 0 = draw, 1 = win,
// 2 = loss (same convention as the NOCCA / dobutsu tables).

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cinttypes>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <algorithm>
#include <array>
#include <unordered_map>
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
  std::string s = std::to_string(v), r; int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) { r += s[i]; if (++c % 3 == 0 && i) r += ','; }
  std::reverse(r.begin(), r.end()); return r;
}
static double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void* bigalloc(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) die("mmap %zu bytes failed", bytes);
  madvise(p, bytes, MADV_HUGEPAGE);
  return p;
}

// ------------------------------------------------------------------------------------------
// Board / column-state tables
// ------------------------------------------------------------------------------------------
static int W = 7, H = 6, CELLS = 42;
static int NS = 127, SB = 7;          // column states, bits per state id
static int SBITS = 6, XBITS = 5;      // bits for stone counters in DP keys
static int KEYBITS = 32;

static int BASE[8];
static std::vector<int> hgt_, pat_, xcnt_;
static std::vector<u16> cells_;       // 2 bits per row: 0 empty, 1 = O, 2 = X
static std::vector<u8> valid_;        // no vertical 4-run
static std::vector<int> validList_;
static u64* win4_ = nullptr;          // bitset [A][B][C][D]: D completes a horiz/diag 4-row

static inline int cellOf(int s, int r) { return (cells_[s] >> (2 * r)) & 3; }

static void initTables(int w, int h) {
  W = w; H = h; CELLS = W * H;
  NS = (1 << (H + 1)) - 1;
  SB = 0; while ((1 << SB) <= NS) ++SB;   // NS fits: for H=6, NS=127 -> SB=7
  SBITS = 0; while ((1 << SBITS) <= CELLS) ++SBITS;
  XBITS = 0; while ((1 << XBITS) <= (CELLS + 1) / 2) ++XBITS;
  KEYBITS = 3 * SB + SBITS + XBITS;
  if (KEYBITS > 32) die("key does not fit in 32 bits for %dx%d", w, h);
  for (int i = 0; i <= 7; ++i) BASE[i] = (1 << i) - 1;
  hgt_.assign(NS, 0); pat_.assign(NS, 0); xcnt_.assign(NS, 0);
  cells_.assign(NS, 0); valid_.assign(NS, 0);
  validList_.clear();
  for (int hh = 0; hh <= H; ++hh)
    for (int p = 0; p < (1 << hh); ++p) {
      int s = BASE[hh] + p;
      hgt_[s] = hh; pat_[s] = p; xcnt_[s] = __builtin_popcount(p);
      u16 cl = 0;
      for (int r = 0; r < hh; ++r) cl |= (u16)(((p >> r) & 1) ? 2 : 1) << (2 * r);
      cells_[s] = cl;
      // vertical 4-run?
      bool bad = false;
      for (int r = 0; r + 3 < hh; ++r) {
        int c0 = (p >> r) & 1;
        if (((p >> (r + 1)) & 1) == c0 && ((p >> (r + 2)) & 1) == c0 && ((p >> (r + 3)) & 1) == c0)
          bad = true;
      }
      valid_[s] = !bad;
      if (!bad) validList_.push_back(s);
    }
}

// Precompute win4 bitset: index ((A*NS+B)*NS+C)*NS+D.
static void initWin4(int threads) {
  size_t nbits = (size_t)NS * NS * NS * NS;
  size_t nw = (nbits + 63) / 64;
  win4_ = (u64*)bigalloc(nw * 8);
  std::atomic<int> ctr{0};
  std::vector<std::thread> th;
  for (int t = 0; t < threads; ++t)
    th.emplace_back([&] {
      for (;;) {
        int A = ctr.fetch_add(1);
        if (A >= NS) break;
        for (int B = 0; B < NS; ++B)
          for (int Cc = 0; Cc < NS; ++Cc) {
            size_t base = (((size_t)A * NS + B) * NS + Cc) * NS;
            for (int D = 0; D < NS; ++D) {
              bool win = false;
              for (int r = 0; r < H && !win; ++r) {
                int d = cellOf(D, r);
                if (d && cellOf(A, r) == d && cellOf(B, r) == d && cellOf(Cc, r) == d) win = true;
              }
              for (int r = 0; r + 3 < H && !win; ++r) {
                int d = cellOf(D, r + 3);   // up-diagonal ending at D
                if (d && cellOf(A, r) == d && cellOf(B, r + 1) == d && cellOf(Cc, r + 2) == d) win = true;
                int d2 = cellOf(D, r);      // down-diagonal ending at D
                if (d2 && cellOf(A, r + 3) == d2 && cellOf(B, r + 2) == d2 && cellOf(Cc, r + 1) == d2)
                  win = true;
              }
              if (win) {
                size_t idx = base + D;
                __atomic_fetch_or(&win4_[idx >> 6], 1ull << (idx & 63), __ATOMIC_RELAXED);
              }
            }
          }
      }
    });
  for (auto& x : th) x.join();
}
static inline bool win4(int A, int B, int Cc, int D) {
  size_t idx = (((size_t)A * NS + B) * NS + Cc) * NS + D;
  return (win4_[idx >> 6] >> (idx & 63)) & 1;
}

// ------------------------------------------------------------------------------------------
// Forward counting DP over columns. Key at the boundary entering column c:
//   window (A,B,C) = states of columns c-3,c-2,c-1 (state 0 pad), s' / x' = stones and
//   X-stones in columns dropped from the window (columns <= c-4).
// Produces per-ply pseudo counts and (optionally) live-key bitmaps per boundary.
// ------------------------------------------------------------------------------------------
static inline u32 keyOf(int A, int B, int Cc, int sp, int xp) {
  return ((((u32)A << SB | B) << SB | Cc) << SBITS | sp) << XBITS | xp;
}

struct CountResult {
  std::vector<u64> perPly;      // pseudo non-terminal positions per ply
  u64 total = 0;
};

static CountResult countDP(int threads, bool verbose) {
  size_t KS = (size_t)1 << KEYBITS;
  u64* dp = (u64*)bigalloc(KS * 8);
  u64* dp2 = (u64*)bigalloc(KS * 8);
  dp[keyOf(0, 0, 0, 0, 0)] = 1;
  for (int c = 0; c < W; ++c) {
    double t0 = now_s();
    std::atomic<u64> chunk{0};
    const u64 CH = 1 << 18;
    std::vector<std::thread> th;
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&] {
        for (;;) {
          u64 b = chunk.fetch_add(1) * CH;
          if (b >= KS) break;
          u64 e = std::min(KS, b + CH);
          for (u64 key = b; key < e; ++key) {
            u64 cnt = dp[key];
            if (!cnt) continue;
            int xp = key & ((1 << XBITS) - 1);
            int sp = (key >> XBITS) & ((1 << SBITS) - 1);
            int Cc = (key >> (XBITS + SBITS)) & (NS ? ((1 << SB) - 1) : 0);
            int B = (key >> (XBITS + SBITS + SB)) & ((1 << SB) - 1);
            int A = (key >> (XBITS + SBITS + 2 * SB)) & ((1 << SB) - 1);
            int sp2 = sp + hgt_[A], xp2 = xp + xcnt_[A];
            for (int D : validList_) {
              if (win4(A, B, Cc, D)) continue;
              u32 nk = keyOf(B, Cc, D, sp2, xp2);
              __atomic_fetch_add(&dp2[nk], cnt, __ATOMIC_RELAXED);
            }
          }
        }
      });
    for (auto& x : th) x.join();
    // swap and clear
    std::swap(dp, dp2);
    memset(dp2, 0, KS * 8);
    if (verbose) printf("[count] column %d done in %.1fs\n", c, now_s() - t0);
    fflush(stdout);
  }
  // tally per ply
  CountResult res;
  res.perPly.assign(CELLS + 1, 0);
  {
    std::vector<std::thread> th;
    std::vector<std::vector<u64>> acc(threads, std::vector<u64>(CELLS + 1, 0));
    std::atomic<u64> chunk{0};
    const u64 CH = 1 << 18;
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&, t] {
        for (;;) {
          u64 b = chunk.fetch_add(1) * CH;
          if (b >= KS) break;
          u64 e = std::min(KS, b + CH);
          for (u64 key = b; key < e; ++key) {
            u64 cnt = dp[key];
            if (!cnt) continue;
            int xp = key & ((1 << XBITS) - 1);
            int sp = (key >> XBITS) & ((1 << SBITS) - 1);
            int Cc = (key >> (XBITS + SBITS)) & ((1 << SB) - 1);
            int B = (key >> (XBITS + SBITS + SB)) & ((1 << SB) - 1);
            int A = (key >> (XBITS + SBITS + 2 * SB)) & ((1 << SB) - 1);
            int s = sp + hgt_[A] + hgt_[B] + hgt_[Cc];
            int x = xp + xcnt_[A] + xcnt_[B] + xcnt_[Cc];
            if (x == (s + 1) / 2) acc[t][s] += cnt;
          }
        }
      });
    for (auto& x : th) x.join();
    for (int t = 0; t < threads; ++t)
      for (int k = 0; k <= CELLS; ++k) res.perPly[k] += acc[t][k];
  }
  for (int k = 0; k <= CELLS; ++k) res.total += res.perPly[k];
  munmap(dp, KS * 8);
  munmap(dp2, KS * 8);
  return res;
}

// Brute-force pseudo count for small boards (validation of the DP).
static void bruteCount(std::vector<u64>& perPly) {
  perPly.assign(CELLS + 1, 0);
  std::vector<int> col(W, 0);
  // iterate all W-tuples of valid states
  std::vector<u64> local(CELLS + 1, 0);
  size_t nv = validList_.size();
  std::vector<size_t> idx(W, 0);
  for (;;) {
    // check windows
    bool ok = true;
    int s = 0, x = 0;
    for (int c = 0; c < W && ok; ++c) {
      int D = validList_[idx[c]];
      s += hgt_[D]; x += xcnt_[D];
      int A = c >= 3 ? validList_[idx[c - 3]] : 0;
      int B = c >= 2 ? validList_[idx[c - 2]] : 0;
      int Cc = c >= 1 ? validList_[idx[c - 1]] : 0;
      if (win4(A, B, Cc, D)) ok = false;
    }
    if (ok && x == (s + 1) / 2) local[s]++;
    int c = 0;
    while (c < W && ++idx[c] == nv) { idx[c] = 0; ++c; }
    if (c == W) break;
  }
  perPly = local;
}

// ------------------------------------------------------------------------------------------
// Shared node pool with lock-free hash-consing. Node 0 = 0-leaf, node 1 = 1-leaf.
// Items: var d = c*SB + j asks bit (SB-1-j) of column c's state id (MSB first).
// ------------------------------------------------------------------------------------------
struct Pool {
  u8* var_ = nullptr; u32* lo_ = nullptr; u32* hi_ = nullptr;
  u64* cntlo_ = nullptr;
  std::atomic<u64> nnodes{2};
  u32* slots = nullptr; u64 nslots = 0;
  u64 maxn = 0;

  void alloc(u64 maxNodes, int slotsLog) {
    maxn = maxNodes;
    var_ = (u8*)bigalloc(maxn);
    lo_ = (u32*)bigalloc(maxn * 4);
    hi_ = (u32*)bigalloc(maxn * 4);
    nslots = 1ull << slotsLog;
    slots = (u32*)bigalloc(nslots * 4);   // zero-filled; 0 = empty (leaf ids never interned)
    var_[0] = var_[1] = 255;
    lo_[0] = hi_[0] = lo_[1] = hi_[1] = 0;
  }
  inline u32 intern(u8 v, u32 lo, u32 hi) {
    if (hi == 0) return lo;                       // zero-suppression
    u64 hsh = ((u64)v * 0x9E3779B97F4A7C15ull) ^ ((u64)lo * 0xC2B2AE3D27D4EB4Full)
              ^ ((u64)hi * 0x165667B19E3779F9ull);
    u64 i = (hsh ^ (hsh >> 29)) & (nslots - 1);
    u32 fresh = 0;
    for (;; i = (i + 1) & (nslots - 1)) {
      u32 id = __atomic_load_n(&slots[i], __ATOMIC_ACQUIRE);
      if (id == 0) {
        if (!fresh) {
          fresh = (u32)nnodes.fetch_add(1, std::memory_order_relaxed);
          if (fresh >= maxn) die("node pool exhausted (%" PRIu64 ")", maxn);
          var_[fresh] = v; lo_[fresh] = lo; hi_[fresh] = hi;
        }
        u32 exp = 0;
        if (__atomic_compare_exchange_n(&slots[i], &exp, fresh, false,
                                        __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
          return fresh;
        id = exp;                                  // lost race; fall through to compare
      }
      if (var_[id] == v && lo_[id] == lo && hi_[id] == hi) {
        if (fresh) { lo_[fresh] = 0; hi_[fresh] = 0; var_[fresh] = 255; }  // dead duplicate
        return id;
      }
    }
  }
};

// Open-addressing map u32 key -> u32 node id, parallel insert. Slot: (key+1)<<32 | id.
struct KeyMap {
  u64* slots = nullptr; u64 nslots = 0;
  void alloc(u64 count) {
    nslots = 4;
    while (nslots < count * 2) nslots <<= 1;
    slots = (u64*)bigalloc(nslots * 8);
  }
  void free_() { if (slots) munmap(slots, nslots * 8); slots = nullptr; }
  inline void insert(u32 key, u32 id) {
    u64 ent = ((u64)key + 1) << 32 | id;
    u64 hsh = (u64)key * 0x9E3779B97F4A7C15ull;
    for (u64 i = (hsh ^ (hsh >> 31)) & (nslots - 1);; i = (i + 1) & (nslots - 1)) {
      u64 cur = __atomic_load_n(&slots[i], __ATOMIC_RELAXED);
      if (cur == 0) {
        u64 exp = 0;
        if (__atomic_compare_exchange_n(&slots[i], &exp, ent, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
          return;
        cur = exp;
      }
      if ((cur >> 32) == (u64)key + 1) return;    // already inserted (same value)
    }
  }
  inline u32 get(u32 key) const {                  // 0 if absent (= 0-leaf)
    u64 hsh = (u64)key * 0x9E3779B97F4A7C15ull;
    for (u64 i = (hsh ^ (hsh >> 31)) & (nslots - 1);; i = (i + 1) & (nslots - 1)) {
      u64 cur = slots[i];
      if (cur == 0) return 0;
      if ((cur >> 32) == (u64)key + 1) return (u32)cur;
    }
  }
};

// ------------------------------------------------------------------------------------------
// ZDD forest over all plies (shared pool, one root per ply).
// ------------------------------------------------------------------------------------------
struct Forest {
  Pool P;
  std::vector<u32> roots;        // per ply 0..CELLS
  std::vector<u64> perPly;       // positions per ply (validated against countDP)
  u64 nnodes = 0;

  void build(int threads, bool verbose) {
    size_t KS = (size_t)1 << KEYBITS;
    // ---- forward liveness over (window, r, rx) keys ----
    std::vector<u64*> live(W + 1);
    for (int c = 0; c <= W; ++c) live[c] = (u64*)bigalloc(KS / 8);
    for (int k = 0; k <= CELLS; ++k) {
      u32 key = keyOf(0, 0, 0, k, (k + 1) / 2);
      live[0][key >> 6] |= 1ull << (key & 63);
    }
    for (int c = 0; c < W; ++c) {
      double t0 = now_s();
      std::atomic<u64> chunk{0};
      const u64 CH = 1 << 15;                      // in units of 64-bit words
      u64 words = KS / 64;
      std::vector<std::thread> th;
      for (int t = 0; t < threads; ++t)
        th.emplace_back([&] {
          for (;;) {
            u64 wb = chunk.fetch_add(1) * CH;
            if (wb >= words) break;
            u64 we = std::min(words, wb + CH);
            for (u64 wi = wb; wi < we; ++wi) {
              u64 wv = live[c][wi];
              while (wv) {
                int b = __builtin_ctzll(wv);
                wv &= wv - 1;
                u32 key = (u32)(wi * 64 + b);
                int rx = key & ((1 << XBITS) - 1);
                int r = (key >> XBITS) & ((1 << SBITS) - 1);
                int Cc = (key >> (XBITS + SBITS)) & ((1 << SB) - 1);
                int B = (key >> (XBITS + SBITS + SB)) & ((1 << SB) - 1);
                int A = (key >> (XBITS + SBITS + 2 * SB)) & ((1 << SB) - 1);
                int cap = H * (W - 1 - c);
                for (int D : validList_) {
                  int r2 = r - hgt_[D], rx2 = rx - xcnt_[D];
                  if (r2 < 0 || rx2 < 0 || rx2 > r2 || r2 > cap) continue;
                  if (win4(A, B, Cc, D)) continue;
                  u32 nk = keyOf(B, Cc, D, r2, rx2);
                  __atomic_fetch_or(&live[c + 1][nk >> 6], 1ull << (nk & 63), __ATOMIC_RELAXED);
                }
              }
            }
          }
        });
      for (auto& x : th) x.join();
      if (verbose) { printf("[zdd] liveness col %d in %.1fs\n", c, now_s() - t0); fflush(stdout); }
    }

    // ---- node pool ----
    int slotsLog = (W * H >= 40) ? 32 : 27;
    u64 maxNodes = (W * H >= 40) ? 3800000000ull : 120000000ull;
    P.alloc(maxNodes, slotsLog);

    // ---- backward construction ----
    KeyMap maps[8];
    for (int c = W - 1; c >= 0; --c) {
      double t0 = now_s();
      // gather live keys of level c
      std::vector<u32> keys;
      {
        u64 words = KS / 64;
        std::vector<u64> cnts(threads, 0);
        std::vector<std::thread> th;
        u64 per = (words + threads - 1) / threads;
        for (int t = 0; t < threads; ++t)
          th.emplace_back([&, t] {
            u64 b = t * per, e = std::min(words, b + per), c2 = 0;
            for (u64 wi = b; wi < e; ++wi) c2 += __builtin_popcountll(live[c][wi]);
            cnts[t] = c2;
          });
        for (auto& x : th) x.join();
        u64 total = 0;
        std::vector<u64> off(threads + 1, 0);
        for (int t = 0; t < threads; ++t) { off[t] = total; total += cnts[t]; }
        off[threads] = total;
        keys.resize(total);
        th.clear();
        for (int t = 0; t < threads; ++t)
          th.emplace_back([&, t] {
            u64 b = t * per, e = std::min(words, b + per), o = off[t];
            for (u64 wi = b; wi < e; ++wi) {
              u64 wv = live[c][wi];
              while (wv) {
                int b2 = __builtin_ctzll(wv);
                wv &= wv - 1;
                keys[o++] = (u32)(wi * 64 + b2);
              }
            }
          });
        for (auto& x : th) x.join();
      }
      maps[c].alloc(std::max<u64>(keys.size(), 16));
      std::atomic<u64> ctr{0};
      std::vector<std::thread> th;
      for (int t = 0; t < threads; ++t)
        th.emplace_back([&] {
          std::vector<u32> tri(2 << SB);
          for (;;) {
            u64 i0 = ctr.fetch_add(256);
            if (i0 >= keys.size()) break;
            u64 i1 = std::min((u64)keys.size(), i0 + 256);
            for (u64 i = i0; i < i1; ++i) {
              u32 key = keys[i];
              int rx = key & ((1 << XBITS) - 1);
              int r = (key >> XBITS) & ((1 << SBITS) - 1);
              int Cc = (key >> (XBITS + SBITS)) & ((1 << SB) - 1);
              int B = (key >> (XBITS + SBITS + SB)) & ((1 << SB) - 1);
              int A = (key >> (XBITS + SBITS + 2 * SB)) & ((1 << SB) - 1);
              int cap = H * (W - 1 - c);
              // children per next-column state D
              u32* leaf = tri.data() + (1 << SB);
              for (int D = 0; D < (1 << SB); ++D) leaf[D] = 0;
              for (int D : validList_) {
                int r2 = r - hgt_[D], rx2 = rx - xcnt_[D];
                if (r2 < 0 || rx2 < 0 || rx2 > r2 || r2 > cap) continue;
                if (win4(A, B, Cc, D)) continue;
                if (c + 1 == W) leaf[D] = (r2 == 0 && rx2 == 0) ? 1u : 0u;
                else leaf[D] = maps[c + 1].get(keyOf(B, Cc, D, r2, rx2));
              }
              // binary trie over the SB bits of D, MSB first
              for (int j = SB - 1; j >= 0; --j) {
                u32* up = tri.data() + (1 << j);
                u32* dn = tri.data() + (2 << j);
                for (int p = 0; p < (1 << j); ++p)
                  up[p] = P.intern((u8)(c * SB + j), dn[2 * p], dn[2 * p + 1]);
              }
              if (tri[1]) maps[c].insert(key, tri[1]);
            }
          }
        });
      for (auto& x : th) x.join();
      if (c + 1 < W) maps[c + 1].free_();
      munmap(live[c + 1], KS / 8);
      if (verbose) {
        printf("[zdd] level %d built: %s keys, %s nodes total, %.1fs\n",
               c, commas(keys.size()).c_str(), commas(P.nnodes.load()).c_str(), now_s() - t0);
        fflush(stdout);
      }
    }
    roots.assign(CELLS + 1, 0);
    for (int k = 0; k <= CELLS; ++k)
      roots[k] = maps[0].get(keyOf(0, 0, 0, k, (k + 1) / 2));
    maps[0].free_();
    munmap(live[0], KS / 8);
    nnodes = P.nnodes.load();

    // ---- counts ----
    u64* cnt = (u64*)bigalloc(nnodes * 8);
    cnt[0] = 0; cnt[1] = 1;
    for (u64 i = 2; i < nnodes; ++i) cnt[i] = cnt[P.lo_[i]] + cnt[P.hi_[i]];
    perPly.assign(CELLS + 1, 0);
    for (int k = 0; k <= CELLS; ++k) perPly[k] = cnt[roots[k]];
    P.cntlo_ = (u64*)bigalloc(nnodes * 8);
    std::vector<std::thread> th2;
    u64 per2 = (nnodes + threads - 1) / threads;
    for (int t = 0; t < threads; ++t)
      th2.emplace_back([&, t] {
        u64 b = std::max<u64>(2, t * per2), e = std::min(nnodes, (t + 1) * per2);
        for (u64 i = b; i < e; ++i) P.cntlo_[i] = cnt[P.lo_[i]];
      });
    for (auto& x : th2) x.join();
    munmap(cnt, nnodes * 8);
  }

  // rank of position (column states arr[0..W-1]) within root's family
  inline u64 rankFrom(u32 n, u64 k, const u8* arr) const {
    while (n > 1) {
      int d = P.var_[n], c = d / SB, j = d % SB;
      if ((arr[c] >> (SB - 1 - j)) & 1) { k += P.cntlo_[n]; n = P.hi_[n]; }
      else n = P.lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }
  u64 rank(int ply, const u8* arr) const { return rankFrom(roots[ply], 0, arr); }

  void unrank(int ply, u64 k, u8* arr) const {
    memset(arr, 0, W);
    u32 n = roots[ply];
    while (n > 1) {
      int d = P.var_[n], c = d / SB, j = d % SB;
      if (P.cntlo_[n] <= k) { k -= P.cntlo_[n]; arr[c] |= (u8)(1 << (SB - 1 - j)); n = P.hi_[n]; }
      else n = P.lo_[n];
    }
    assert(n == 1 && k == 0);
  }

  // per-column-boundary prefix cache of a walk following arr in root `ply`
  void buildPrefix(int ply, const u8* arr, u32* nodeAt, u64* kAt) const {
    u32 n = roots[ply]; u64 k = 0; int last = -1;
    for (;;) {
      int t = (n > 1) ? P.var_[n] / SB : W;
      for (int j = last + 1; j <= t; ++j) { nodeAt[j] = n; kAt[j] = k; }
      last = t;
      if (n <= 1 || t >= W) break;
      int d = P.var_[n], c = d / SB, j = d % SB;
      (void)c;
      if ((arr[d / SB] >> (SB - 1 - j)) & 1) { k += P.cntlo_[n]; n = P.hi_[n]; }
      else n = P.lo_[n];
    }
  }

  void save(const std::string& path) const {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) die("cannot write %s", tmp.c_str());
    u64 hdr[8] = {0xC4C4C4C4C4C4C4C4ull, (u64)W, (u64)H, nnodes, (u64)(CELLS + 1), 0, 0, 0};
    fwrite(hdr, 8, 8, f);
    fwrite(roots.data(), 4, CELLS + 1, f);
    fwrite(perPly.data(), 8, CELLS + 1, f);
    fwrite(P.var_, 1, nnodes, f);
    fwrite(P.lo_, 4, nnodes, f);
    fwrite(P.hi_, 4, nnodes, f);
    fwrite(P.cntlo_, 8, nnodes, f);
    fclose(f);
    if (rename(tmp.c_str(), path.c_str())) die("rename failed");
  }
  bool load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    u64 hdr[8];
    if (fread(hdr, 8, 8, f) != 8 || hdr[0] != 0xC4C4C4C4C4C4C4C4ull ||
        hdr[1] != (u64)W || hdr[2] != (u64)H) die("zdd file mismatch");
    nnodes = hdr[3];
    roots.assign(CELLS + 1, 0);
    perPly.assign(CELLS + 1, 0);
    if (fread(roots.data(), 4, CELLS + 1, f) != (size_t)CELLS + 1) die("zdd read");
    if (fread(perPly.data(), 8, CELLS + 1, f) != (size_t)CELLS + 1) die("zdd read");
    P.var_ = (u8*)bigalloc(nnodes);
    P.lo_ = (u32*)bigalloc(nnodes * 4);
    P.hi_ = (u32*)bigalloc(nnodes * 4);
    P.cntlo_ = (u64*)bigalloc(nnodes * 8);
    if (fread(P.var_, 1, nnodes, f) != nnodes) die("zdd read");
    if (fread(P.lo_, 4, nnodes, f) != nnodes) die("zdd read");
    if (fread(P.hi_, 4, nnodes, f) != nnodes) die("zdd read");
    if (fread(P.cntlo_, 8, nnodes, f) != nnodes) die("zdd read");
    fclose(f);
    P.nnodes = nnodes;
    return true;
  }
};

struct FIter {
  const Forest* z = nullptr;
  int sp = 0;
  u32 nstack[64];
  u8 br[64];
  u8 arr[8];

  void descend(u32 m) {
    while (m > 1) {
      nstack[sp] = m;
      if (z->P.lo_[m] == 0) {
        int d = z->P.var_[m];
        br[sp] = 1; arr[d / SB] |= (u8)(1 << (SB - 1 - d % SB));
        ++sp; m = z->P.hi_[m];
      } else { br[sp] = 0; ++sp; m = z->P.lo_[m]; }
    }
    assert(m == 1);
  }
  void initAt(const Forest& zz, int ply, u64 k) {
    z = &zz; sp = 0;
    memset(arr, 0, 8);
    u32 n = z->roots[ply];
    while (n > 1) {
      nstack[sp] = n;
      int d = z->P.var_[n];
      if (z->P.cntlo_[n] <= k) {
        k -= z->P.cntlo_[n]; br[sp] = 1;
        arr[d / SB] |= (u8)(1 << (SB - 1 - d % SB));
        n = z->P.hi_[n];
      } else { br[sp] = 0; n = z->P.lo_[n]; }
      ++sp;
    }
    assert(n == 1 && k == 0);
  }
  bool next() {
    while (sp) {
      --sp;
      u32 n = nstack[sp];
      int d = z->P.var_[n];
      if (br[sp]) { arr[d / SB] &= (u8)~(1 << (SB - 1 - d % SB)); }
      else {
        br[sp] = 1; arr[d / SB] |= (u8)(1 << (SB - 1 - d % SB));
        ++sp; descend(z->P.hi_[n]);
        return true;
      }
    }
    return false;
  }
};

// ------------------------------------------------------------------------------------------
// Bitboards (bit index c*(H+1)+r) and game helpers
// ------------------------------------------------------------------------------------------
static inline void toBitboards(const u8* arr, u64& xbb, u64& obb) {
  xbb = obb = 0;
  for (int c = 0; c < W; ++c) {
    int s = arr[c], hh = hgt_[s], p = pat_[s];
    xbb |= (u64)(p & ((1 << hh) - 1)) << (c * (H + 1));
    obb |= (u64)(~p & ((1 << hh) - 1)) << (c * (H + 1));
  }
}
static inline bool hasWin(u64 bb) {
  int H1 = H + 1;
  u64 m = bb & (bb >> H1);          // horizontal
  if (m & (m >> (2 * H1))) return true;
  m = bb & (bb >> 1);               // vertical
  if (m & (m >> 2)) return true;
  m = bb & (bb >> H);               // diagonal "\"
  if (m & (m >> (2 * H))) return true;
  m = bb & (bb >> (H1 + 1));        // diagonal "/"
  if (m & (m >> (2 * (H1 + 1)))) return true;
  return false;
}
static inline int pushState(int s, int stoneX) {
  int hh = hgt_[s];
  return BASE[hh + 1] + (pat_[s] | (stoneX << hh));
}

// ------------------------------------------------------------------------------------------
// Solver: single backward sweep over plies. Slab files slab_{k}.bin, 2 bits per position
// (0 draw, 1 win, 2 loss for the side to move).
// ------------------------------------------------------------------------------------------
struct C4Solver {
  Forest Z;
  std::string dir;
  int threads;

  std::string slabPath(int k) const { return dir + "/slab_" + std::to_string(k) + ".bin"; }

  u64* mapSlab(int k, bool write, u64 n) const {
    u64 bytes = (n + 31) / 32 * 8;
    std::string p = slabPath(k) + (write ? ".tmp" : "");
    int fd = open(p.c_str(), write ? (O_RDWR | O_CREAT | O_TRUNC) : O_RDONLY, 0644);
    if (fd < 0) die("open %s failed", p.c_str());
    if (write && ftruncate(fd, (off_t)bytes)) die("ftruncate failed");
    void* m = mmap(nullptr, bytes, write ? (PROT_READ | PROT_WRITE) : PROT_READ,
                   write ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) die("mmap slab failed");
    close(fd);
    return (u64*)m;
  }

  void solvePly(int k, u64* out, const u64* next, u64* tallies) {
    u64 n = Z.perPly[k];
    std::atomic<u64> chunkCtr{0};
    const u64 CHUNK = 1 << 18;
    int stmX = (k % 2 == 0);
    std::vector<std::thread> th;
    std::vector<std::array<u64, 3>> tl(threads, {0, 0, 0});
    for (int t = 0; t < threads; ++t)
      th.emplace_back([&, t] {
        FIter it;
        u32 nodeAt[9]; u64 kAt[9];
        u8 child[8];
        for (;;) {
          u64 lo = chunkCtr.fetch_add(1) * CHUNK;
          if (lo >= n) break;
          u64 hi = std::min(n, lo + CHUNK);
          it.initAt(Z, k, lo);
          u64 idx = lo;
          while (idx < hi) {
            u64 wi = idx >> 5;
            u64 spanEnd = std::min(hi, (wi + 1) << 5);
            u64 wv = 0;
            for (; idx < spanEnd; ++idx) {
              u64 xbb, obb;
              toBitboards(it.arr, xbb, obb);
              u64 own = stmX ? xbb : obb;
              int best = 2;                        // loss unless improved
              bool prefixBuilt = false;
              for (int c = 0; c < W; ++c) {
                int s = it.arr[c], hh = hgt_[s];
                if (hh >= H) continue;
                u64 nb = own | (1ull << (c * (H + 1) + hh));
                if (hasWin(nb)) { best = 1; break; }
                int v;
                if (k + 1 == CELLS) v = 0;         // full board, no win: draw
                else {
                  if (!prefixBuilt) { Z.buildPrefix(k + 1, it.arr, nodeAt, kAt); prefixBuilt = true; }
                  u8 saved = it.arr[c];
                  memcpy(child, it.arr, W);
                  child[c] = (u8)pushState(s, stmX);
                  u64 ck = Z.rankFrom(nodeAt[c], kAt[c], child);
                  (void)saved;
                  if (ck == UINT64_MAX) die("child rank failed (ply %d)", k);
                  u32 cv = (u32)((next[ck >> 5] >> ((ck & 31) * 2)) & 3);
                  v = cv == 2 ? 1 : cv == 1 ? 2 : 0;   // child loss => my win
                }
                if (v == 1) { best = 1; break; }
                if (v == 0 && best == 2) best = 0;
              }
              wv |= (u64)best << ((idx & 31) * 2);
              ++tl[t][best];
              if (idx + 1 < hi) { bool ok = it.next(); (void)ok; assert(ok); }
            }
            out[wi] |= wv;
          }
        }
      });
    for (auto& x : th) x.join();
    for (int t = 0; t < threads; ++t)
      for (int v = 0; v < 3; ++v) tallies[v] += tl[t][v];
  }

  void solve() {
    mkdir(dir.c_str(), 0755);
    u64* next = nullptr;
    u64 nextBytes = 0;
    // resume: find highest k with no finished slab
    int startK = CELLS - 1;
    while (startK >= 0) {
      struct stat st;
      if (stat(slabPath(startK).c_str(), &st) != 0) break;
      --startK;
    }
    if (startK < CELLS - 1) {
      int kn = startK + 1;
      next = mapSlab(kn, false, Z.perPly[kn]);
      nextBytes = (Z.perPly[kn] + 31) / 32 * 8;
      printf("[solve] resuming at ply %d\n", startK);
    }
    for (int k = startK; k >= 0; --k) {
      double t0 = now_s();
      u64 n = Z.perPly[k];
      u64* out = mapSlab(k, true, n);
      u64 tallies[3] = {0, 0, 0};
      solvePly(k, out, next, tallies);
      u64 bytes = (n + 31) / 32 * 8;
      msync(out, bytes, MS_SYNC);
      if (rename((slabPath(k) + ".tmp").c_str(), slabPath(k).c_str())) die("rename failed");
      if (next) munmap(next, nextBytes);
      next = out;
      nextBytes = bytes;
      printf("[ply %2d] n=%-15s  W/D/L(stm) = %s / %s / %s   %.1fs\n",
             k, commas(n).c_str(), commas(tallies[1]).c_str(), commas(tallies[0]).c_str(),
             commas(tallies[2]).c_str(), now_s() - t0);
      fflush(stdout);
    }
    if (next) munmap(next, nextBytes);
    printf("[solve] complete.\n");
  }
};

// ------------------------------------------------------------------------------------------
// Independent negamax (own logic, no ZDD/slabs) for spot checks. Returns 1/0/-1 for stm.
// ------------------------------------------------------------------------------------------
namespace bf {
  static std::unordered_map<u64, int> memo;
  static int negamax(u64 own, u64 opp, int stones) {
    if (stones == CELLS) return 0;
    int H1 = H + 1;
    u64 mask = own | opp;
    u64 bottom = 0;
    for (int c = 0; c < W; ++c) bottom |= 1ull << (c * H1);
    u64 key = own + mask + bottom;   // classic unique C4 position key
    auto itf = memo.find(key);
    if (itf != memo.end()) return itf->second;
    int best = -2;
    for (int c = 0; c < W && best < 1; ++c) {
      u64 colmask = ((1ull << H) - 1) << (c * H1);
      if ((mask & colmask) == colmask) continue;
      u64 bit = (mask + (1ull << (c * H1))) & colmask;
      u64 nown = own | bit;
      int v;
      if (hasWin(nown)) v = 1;
      else v = -negamax(opp, nown, stones + 1);
      if (v > best) best = v;
    }
    memo[key] = best;
    return best;
  }
  static int eval(const u8* arr, int stones) {
    u64 xbb, obb;
    toBitboards(arr, xbb, obb);
    bool stmX = stones % 2 == 0;
    return negamax(stmX ? xbb : obb, stmX ? obb : xbb, stones);
  }
}

// ------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s count [--board WxH] [--brute] | buildzdd | solve | selftest |\n"
      "       %s probe <moveseq|-> | analyze | bfcheck [n]\n"
      "       common flags: --board WxH (default 7x6), --threads N, --dir D\n",
      argv[0], argv[0]);
    return 1;
  }
  std::string cmd = argv[1];
  int threads = (int)std::thread::hardware_concurrency();
  int w = 7, h = 6;
  bool brute = false;
  std::string dir = "";
  std::vector<std::string> rest;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--board" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
    else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--brute") brute = true;
    else rest.push_back(a);
  }
  if (w < 4 || w > 7 || h < 4 || h > 6) die("supported: 4<=W<=7, 4<=H<=6");
  initTables(w, h);
  // Relative to the working directory, so the solver tree can be moved without editing source.
  if (dir.empty()) dir = "c4_wdl_" + std::to_string(w) + "x" + std::to_string(h);
  std::string zddPath = dir + "/zdd.bin";
  double t0 = now_s();
  initWin4(threads);

  auto ensureForest = [&](Forest& Z, bool buildIfMissing) {
    mkdir(dir.c_str(), 0755);
    if (Z.load(zddPath)) {
      printf("[zdd] loaded: %s nodes\n", commas(Z.nnodes).c_str());
      return;
    }
    if (!buildIfMissing) die("no zdd at %s — run buildzdd first", zddPath.c_str());
    double tb = now_s();
    Z.build(threads, true);
    printf("[zdd] built: %s nodes in %.1fs\n", commas(Z.nnodes).c_str(), now_s() - tb);
    CountResult cr = countDP(threads, false);
    bool ok = true;
    for (int k = 0; k <= CELLS; ++k)
      if (cr.perPly[k] != Z.perPly[k]) {
        ok = false;
        printf("[zdd] COUNT MISMATCH ply %d: zdd=%s dp=%s\n", k,
               commas(Z.perPly[k]).c_str(), commas(cr.perPly[k]).c_str());
      }
    if (!ok) die("zdd root counts do not match counting DP");
    printf("[zdd] root counts match counting DP for all %d plies — saving\n", CELLS + 1);
    Z.save(zddPath);
  };

  if (cmd == "count") {
    t0 = now_s();
    CountResult res = countDP(threads, true);
    printf("[count] pseudo non-terminal positions per ply (%.1fs):\n", now_s() - t0);
    u64 tableBytes = 0;
    for (int k = 0; k <= CELLS; ++k) {
      if (res.perPly[k]) printf("  ply %2d: %s\n", k, commas(res.perPly[k]).c_str());
      tableBytes += (res.perPly[k] + 3) / 4;
    }
    printf("[count] TOTAL: %s positions -> 2-bit table ~%.2f TB (%.1f GB)\n",
           commas(res.total).c_str(), tableBytes / 1e12, tableBytes / 1e9);
    if (brute) {
      std::vector<u64> bp;
      bruteCount(bp);
      bool ok = true;
      for (int k = 0; k <= CELLS; ++k)
        if (bp[k] != res.perPly[k]) ok = false;
      printf("[brute] full enumeration crosscheck: %s\n", ok ? "OK" : "MISMATCH");
      if (!ok) exit(1);
    }
  } else if (cmd == "buildzdd") {
    Forest Z;
    ensureForest(Z, true);
  } else if (cmd == "solve") {
    C4Solver S;
    S.threads = threads;
    S.dir = dir;
    ensureForest(S.Z, true);
    t0 = now_s();
    S.solve();
    printf("[solve] total %.1fs\n", now_s() - t0);
  } else if (cmd == "selftest") {
    Forest Z;
    ensureForest(Z, true);
    std::mt19937_64 rng(31337);
    u8 arr[8], arr2[8];
    for (int rep = 0; rep < 30000; ++rep) {
      int k = (int)(rng() % (CELLS + 1));
      if (!Z.perPly[k]) continue;
      u64 r = rng() % Z.perPly[k];
      Z.unrank(k, r, arr);
      int s = 0, x = 0;
      u64 xbb, obb;
      for (int c = 0; c < W; ++c) { s += hgt_[arr[c]]; x += xcnt_[arr[c]]; }
      toBitboards(arr, xbb, obb);
      assert(s == k && x == (k + 1) / 2);
      assert(!hasWin(xbb) && !hasWin(obb));
      assert(Z.rank(k, arr) == r);
    }
    printf("[selftest] rank/unrank + invariants OK\n");
    FIter it;
    for (int rep = 0; rep < 6; ++rep) {
      int k = 8 + (int)(rng() % (CELLS - 8));
      if (Z.perPly[k] < 4000) continue;
      u64 k0 = rng() % (Z.perPly[k] - 2000);
      it.initAt(Z, k, k0);
      for (u64 r = k0; r < k0 + 2000; ++r) {
        Z.unrank(k, r, arr);
        assert(memcmp(arr, it.arr, W) == 0);
        if (r + 1 < Z.perPly[k]) assert(it.next());
      }
    }
    printf("[selftest] iterator OK\n");
    u32 nodeAt[9]; u64 kAt[9];
    int checked = 0;
    for (int rep = 0; rep < 20000; ++rep) {
      int k = (int)(rng() % CELLS);
      if (!Z.perPly[k]) continue;
      u64 r = rng() % Z.perPly[k];
      Z.unrank(k, r, arr);
      int stmX = (k % 2 == 0);
      u64 xbb, obb;
      toBitboards(arr, xbb, obb);
      u64 own = stmX ? xbb : obb;
      Z.buildPrefix(k + 1, arr, nodeAt, kAt);
      for (int c = 0; c < W; ++c) {
        int s = arr[c], hh = hgt_[s];
        if (hh >= H) continue;
        u64 nb = own | (1ull << (c * (H + 1) + hh));
        if (hasWin(nb)) continue;
        if (k + 1 == CELLS) continue;
        memcpy(arr2, arr, W);
        arr2[c] = (u8)pushState(s, stmX);
        u64 ck = Z.rankFrom(nodeAt[c], kAt[c], arr2);
        assert(ck != UINT64_MAX && ck == Z.rank(k + 1, arr2));
        ++checked;
      }
    }
    printf("[selftest] cached child-rank OK (%d children)\n", checked);
    printf("[selftest] ALL OK\n");
  } else if (cmd == "bfcheck") {
    // compare table values with independent negamax on random positions
    Forest Z;
    ensureForest(Z, false);
    int nsamp = rest.empty() ? 500 : atoi(rest[0].c_str());
    int minPly = (W * H > 30) ? 26 : 0;   // keep negamax tractable on the full board
    std::mt19937_64 rng(2025);
    u8 arr[8];
    int done = 0;
    while (done < nsamp) {
      int k = minPly + (int)(rng() % (CELLS - minPly));
      if (!Z.perPly[k]) continue;
      u64 r = rng() % Z.perPly[k];
      Z.unrank(k, r, arr);
      int bv = bf::eval(arr, k);
      u64 ck = Z.rank(k, arr);
      std::string sp = dir + "/slab_" + std::to_string(k) + ".bin";
      int fd = open(sp.c_str(), O_RDONLY);
      if (fd < 0) die("missing slab %d — solve first", k);
      u64 wv = 0;
      if (pread(fd, &wv, 8, (off_t)((ck >> 5) * 8)) != 8) die("pread failed");
      close(fd);
      u32 tv = (u32)((wv >> ((ck & 31) * 2)) & 3);
      int tvi = tv == 1 ? 1 : tv == 2 ? -1 : 0;
      if (tvi != bv) die("bfcheck MISMATCH ply %d rank %" PRIu64 ": table=%d bf=%d", k, r, tvi, bv);
      ++done;
      if (bf::memo.size() > 40000000) bf::memo.clear();
    }
    printf("[bfcheck] %d random positions agree with independent negamax\n", nsamp);
  } else if (cmd == "bigtally") {
    // Streaming reachability tally for 7x6: BFS ply by ply (two visited bitmaps in RAM),
    // tallying reachable non-terminal W/D/L (first-player perspective) and comparing with
    // the reference solution table (Edelkamp/Kissmann-style BDD solver, repo README).
    if (W != 7 || H != 6) die("bigtally is for 7x6");
    static const u64 EW[43] = {1,1,27,35,690,1080,10889,17507,124624,197749,1122696,1734122,
      8191645,12333735,49756539,73263172,255117922,369230362,1112643249,1589752959,
      4132585341,5849074428,13031002559,18317405077,34623818387,48376711901,76568242258,
      106274173915,138476323812,190301585678,199698237436,269818663336,221858140210,
      291549830422,180530409295,226007657501,98839977654,114359332473,32161409500,
      33666235957,4831822472,4282128782,0};
    static const u64 ED[43] = {0,2,12,58,200,697,1943,5944,14676,42896,97532,255780,541825,
      1286746,2583292,5596074,10681110,21226658,38582237,70754712,122495056,208240707,
      342506047,543074854,845872717,1256717558,1846266966,2578399088,3567644646,4687144532,
      6071049190,7481813611,9048082187,10381952902,11668229290,12225240861,12431825174,
      11509102126,10220085105,7792641079,5153271363,2496557393,713298878};
    static const u64 EL[43] = {0,4,10,145,230,2486,3590,31408,44975,317541,442395,2578781,
      3502631,17308630,23097764,97682013,128792359,467761723,612658408,1907752131,
      2491075548,6616029910,8637315382,19402748258,25361122355,47632685500,62314059815,
      96436935052,126013643486,157638115456,204609218821,201906000786,258000224786,
      194705107378,241273091751,132714989361,155042098394,57747247782,61622970744,
      13697133737,12710802660,1033139763,746034021};
    static const u64 ET[43] = {0,0,0,0,0,0,0,728,1892,19412,44225,273261,573323,2720636,
      5349954,20975690,38918821,130632515,229031670,670491437,1108210254,2858601535,
      4434627684,10130180393,14654767176,29672303474,39696898910,71042927249,86949129149,
      136563138602,150692335491,205243451746,200299011722,232494602432,195427938799,
      188065840647,131014104050,100184819358,54716901301,31270711562,11972173842,
      4282128782,746034021};
    Forest Z;
    ensureForest(Z, false);
    u64* vis = (u64*)bigalloc((Z.perPly[0] + 63) / 64 * 8);
    vis[0] = 1;
    bool allOK = true;
    for (int k = 0; k <= CELLS; ++k) {
      double t0 = now_s();
      u64 n = Z.perPly[k];
      u64* nvis = nullptr;
      if (k < CELLS) nvis = (u64*)bigalloc((Z.perPly[k + 1] + 63) / 64 * 8);
      u64* slab = nullptr;
      u64 slabBytes = 0;
      if (k < CELLS) {
        std::string sp = dir + "/slab_" + std::to_string(k) + ".bin";
        int fd = open(sp.c_str(), O_RDONLY);
        if (fd < 0) die("missing slab %d", k);
        slabBytes = (n + 31) / 32 * 8;
        slab = (u64*)mmap(nullptr, slabBytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (slab == MAP_FAILED) die("mmap slab failed");
        close(fd);
      }
      std::atomic<u64> chunkCtr{0};
      const u64 CHUNK = 1 << 18;
      int stmX = (k % 2 == 0);
      std::vector<std::array<u64, 4>> tl(threads, {0, 0, 0, 0});  // W,D,L,reach
      std::vector<std::thread> th;
      for (int t = 0; t < threads; ++t)
        th.emplace_back([&, t] {
          FIter it;
          u32 nodeAt[9]; u64 kAt[9];
          u8 child[8];
          for (;;) {
            u64 lo = chunkCtr.fetch_add(1) * CHUNK;
            if (lo >= n) break;
            u64 hi = std::min(n, lo + CHUNK);
            it.initAt(Z, k, lo);
            for (u64 r = lo; r < hi; ++r) {
              if ((vis[r >> 6] >> (r & 63)) & 1) {
                ++tl[t][3];
                if (k < CELLS) {
                  u32 v = (u32)((slab[r >> 5] >> ((r & 31) * 2)) & 3);
                  u32 fp = stmX ? v : (v == 1 ? 2u : v == 2 ? 1u : 0u);
                  ++tl[t][fp == 1 ? 0 : fp == 2 ? 2 : 1];
                  u64 xbb, obb;
                  toBitboards(it.arr, xbb, obb);
                  u64 own = stmX ? xbb : obb;
                  bool prefixBuilt = false;
                  for (int c = 0; c < W; ++c) {
                    int s = it.arr[c], hh = hgt_[s];
                    if (hh >= H) continue;
                    u64 nb = own | (1ull << (c * (H + 1) + hh));
                    if (hasWin(nb)) continue;
                    if (!prefixBuilt) { Z.buildPrefix(k + 1, it.arr, nodeAt, kAt); prefixBuilt = true; }
                    memcpy(child, it.arr, 8);
                    child[c] = (u8)pushState(s, stmX);
                    u64 ck = Z.rankFrom(nodeAt[c], kAt[c], child);
                    __atomic_fetch_or(&nvis[ck >> 6], 1ull << (ck & 63), __ATOMIC_RELAXED);
                  }
                } else {
                  ++tl[t][1];   // full board, no win: draw
                }
              }
              if (r + 1 < hi) it.next();
            }
          }
        });
      for (auto& x : th) x.join();
      u64 cw = 0, cd = 0, cl = 0, cr = 0;
      for (int t = 0; t < threads; ++t) {
        cw += tl[t][0]; cd += tl[t][1]; cl += tl[t][2]; cr += tl[t][3];
      }
      u64 xw = EW[k] - (k % 2 == 1 ? ET[k] : 0);
      u64 xl = EL[k] - (k % 2 == 0 ? ET[k] : 0);
      u64 xd = ED[k];
      bool ok = cw == xw && cd == xd && cl == xl && cr == xw + xd + xl;
      if (!ok) allOK = false;
      printf("[bigtally ply %2d] reach=%s W/D/L=%s/%s/%s  expected %s/%s/%s  %s  %.1fs\n",
             k, commas(cr).c_str(), commas(cw).c_str(), commas(cd).c_str(), commas(cl).c_str(),
             commas(xw).c_str(), commas(xd).c_str(), commas(xl).c_str(),
             ok ? "MATCH" : "MISMATCH!", now_s() - t0);
      fflush(stdout);
      munmap(vis, (n + 63) / 64 * 8);
      if (slab) munmap(slab, slabBytes);
      vis = nvis;
    }
    printf("[bigtally] %s\n", allOK ? "ALL 43 PLIES MATCH the reference solution table"
                                    : "MISMATCHES FOUND");
  } else if (cmd == "reachtally") {
    // BFS over reachable positions (small boards); tally per-ply won/drawn/lost from the
    // FIRST PLAYER's perspective, including terminal positions (deduplicated), matching
    // the reference solver's solve.out tables.
    Forest Z;
    ensureForest(Z, false);
    std::vector<std::vector<u64>> visited(CELLS + 1);
    for (int k = 0; k <= CELLS; ++k) visited[k].assign((Z.perPly[k] + 63) / 64, 0);
    visited[0][0] = 1;   // empty position, rank 0
    std::vector<u64> won(CELLS + 1, 0), drawn(CELLS + 1, 0), lost(CELLS + 1, 0),
        term(CELLS + 1, 0), totalv(CELLS + 1, 0);
    std::vector<u64> termCodes;
    auto codeOf = [&](const u8* a) {
      u64 cd = 0;
      for (int c = 0; c < W; ++c) cd = cd << SB | a[c];
      return cd;
    };
    for (int k = 0; k < CELLS; ++k) {
      termCodes.clear();
      int stmX = (k % 2 == 0);
      FIter it;
      if (!Z.perPly[k]) continue;
      it.initAt(Z, k, 0);
      u8 child[8];
      for (u64 r = 0;; ++r) {
        if ((visited[k][r >> 6] >> (r & 63)) & 1) {
          u64 xbb, obb;
          toBitboards(it.arr, xbb, obb);
          u64 own = stmX ? xbb : obb;
          for (int c = 0; c < W; ++c) {
            int s = it.arr[c], hh = hgt_[s];
            if (hh >= H) continue;
            u64 nb = own | (1ull << (c * (H + 1) + hh));
            memcpy(child, it.arr, 8);
            child[c] = (u8)pushState(s, stmX);
            if (hasWin(nb)) termCodes.push_back(codeOf(child));
            else {
              u64 ck = Z.rank(k + 1, child);
              visited[k + 1][ck >> 6] |= 1ull << (ck & 63);
            }
          }
        }
        if (r + 1 >= Z.perPly[k]) break;
        it.next();
      }
      std::sort(termCodes.begin(), termCodes.end());
      term[k + 1] = std::unique(termCodes.begin(), termCodes.end()) - termCodes.begin();
    }
    // tally values of reachable non-terminal positions
    for (int k = 0; k <= CELLS; ++k) {
      if (!Z.perPly[k]) continue;
      u64 reach = 0;
      for (u64 wv : visited[k]) reach += __builtin_popcountll(wv);
      if (!reach) continue;
      if (k == CELLS) {
        drawn[k] += reach;                       // full board, no win
      } else {
        std::string sp = dir + "/slab_" + std::to_string(k) + ".bin";
        int fd = open(sp.c_str(), O_RDONLY);
        if (fd < 0) die("missing slab %d — solve first", k);
        u64 bytes = (Z.perPly[k] + 31) / 32 * 8;
        u64* slab = (u64*)mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (slab == MAP_FAILED) die("mmap slab failed");
        close(fd);
        for (u64 r = 0; r < Z.perPly[k]; ++r) {
          if (!((visited[k][r >> 6] >> (r & 63)) & 1)) continue;
          u32 v = (u32)((slab[r >> 5] >> ((r & 31) * 2)) & 3);
          // convert side-to-move value to first-player perspective
          u32 fp = (k % 2 == 0) ? v : (v == 1 ? 2u : v == 2 ? 1u : 0u);
          if (fp == 1) ++won[k]; else if (fp == 2) ++lost[k]; else ++drawn[k];
        }
        munmap(slab, bytes);
      }
      // terminals: winner is the mover of ply k (odd ply => X moved => won for X)
      if (term[k]) {
        if (k % 2 == 1) won[k] += term[k]; else lost[k] += term[k];
      }
      totalv[k] = won[k] + drawn[k] + lost[k];
    }
    u64 tw = 0, td = 0, tlo = 0, tt = 0;
    printf("ply |            won |          drawn |           lost |          total | terminal\n");
    for (int k = 0; k <= CELLS; ++k) {
      if (!totalv[k]) continue;
      printf("%3d | %14s | %14s | %14s | %14s | %s\n", k, commas(won[k]).c_str(),
             commas(drawn[k]).c_str(), commas(lost[k]).c_str(), commas(totalv[k]).c_str(),
             commas(term[k]).c_str());
      tw += won[k]; td += drawn[k]; tlo += lost[k]; tt += totalv[k];
    }
    printf("SUM | %14s | %14s | %14s | %14s |\n", commas(tw).c_str(), commas(td).c_str(),
           commas(tlo).c_str(), commas(tt).c_str());
  } else if (cmd == "probe" || cmd == "analyze") {
    Forest Z;
    ensureForest(Z, false);
    std::string seq = cmd == "analyze" ? "-" : (rest.empty() ? "-" : rest[0]);
    u8 arr[8];
    memset(arr, 0, 8);
    int k = 0;
    if (seq != "-")
      for (char ch : seq) {
        int c = ch - '0';
        if (c < 0 || c >= W) die("bad column '%c'", ch);
        int s = arr[c];
        if (hgt_[s] >= H) die("column %d full", c);
        int stmX = (k % 2 == 0);
        u64 xbb, obb;
        toBitboards(arr, xbb, obb);
        if (hasWin(xbb) || hasWin(obb)) die("game already over before move %d", k + 1);
        arr[c] = (u8)pushState(s, stmX);
        ++k;
      }
    u64 xbb, obb;
    toBitboards(arr, xbb, obb);
    // print board
    printf("Connect Four %dx%d, ply %d, %s to move\n", W, H, k, k % 2 == 0 ? "X" : "O");
    for (int r = H - 1; r >= 0; --r) {
      printf("  ");
      for (int c = 0; c < W; ++c) {
        u64 b = 1ull << (c * (H + 1) + r);
        putchar(xbb & b ? 'x' : obb & b ? 'o' : '.');
        putchar(' ');
      }
      putchar('\n');
    }
    printf("  ");
    for (int c = 0; c < W; ++c) printf("%d ", c);
    printf("\n");
    if (hasWin(xbb)) { printf("TERMINAL: X has already won.\n"); return 0; }
    if (hasWin(obb)) { printf("TERMINAL: O has already won.\n"); return 0; }
    if (k == CELLS) { printf("TERMINAL: board full — DRAW.\n"); return 0; }
    auto slabVal = [&](int ply, u64 rk) -> u32 {
      std::string sp = dir + "/slab_" + std::to_string(ply) + ".bin";
      int fd = open(sp.c_str(), O_RDONLY);
      if (fd < 0) die("missing slab %d — solve first", ply);
      u64 wv = 0;
      if (pread(fd, &wv, 8, (off_t)((rk >> 5) * 8)) != 8) die("pread failed");
      close(fd);
      return (u32)((wv >> ((rk & 31) * 2)) & 3);
    };
    u64 rk = Z.rank(k, arr);
    if (rk == UINT64_MAX) die("position not in space?!");
    u32 v = slabVal(k, rk);
    printf("Value for side to move: %s\n", v == 1 ? "WIN" : v == 2 ? "LOSS" : "DRAW");
    printf("Move values (for the mover):\n");
    int stmX = (k % 2 == 0);
    u64 own = stmX ? xbb : obb;
    for (int c = 0; c < W; ++c) {
      int s = arr[c], hh = hgt_[s];
      if (hh >= H) { printf("  col %d: (full)\n", c); continue; }
      u64 nb = own | (1ull << (c * (H + 1) + hh));
      if (hasWin(nb)) { printf("  col %d: WIN (immediate)\n", c); continue; }
      if (k + 1 == CELLS) { printf("  col %d: DRAW (fills board)\n", c); continue; }
      u8 child[8];
      memcpy(child, arr, 8);
      child[c] = (u8)pushState(s, stmX);
      u64 ck = Z.rank(k + 1, child);
      u32 cv = slabVal(k + 1, ck);
      printf("  col %d: %s\n", c, cv == 2 ? "WIN" : cv == 1 ? "LOSS" : "DRAW");
    }
  } else {
    die("unknown command %s", cmd.c_str());
  }
  return 0;
}
