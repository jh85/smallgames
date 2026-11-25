// c4count.cpp — counts pseudo-legal non-terminal Connect Four positions on any W x H board.
//
// Counts the same space as `c4 count`: gravity stackings of k stones with ceil(k/2) first-player
// stones and no four-in-a-row, per ply k. `c4 count` keeps the last three columns' full states
// as its DP frontier, which needs V^3 x cells^2 counters and stops at 7 rows. This program
// keeps a smaller, exact frontier instead.
//
// Frontier ("class"). A four that is not vertical spans 4 consecutive columns, so every four
// that is still open after column c runs through column c. For each row r of column c the
// class records the cell's colour and the length (1..3) of the same-colour run ending there in
// each of the three non-vertical directions (horizontal, up-diagonal, down-diagonal). A run
// that can no longer reach length 4 — too few columns left, or it would leave the board — is
// dropped (0), and a cell with no live run is stored as empty. Two partial boards with the
// same class have exactly the same set of valid completions. Vertical fours are excluded by
// the list of column states. Column states are placed whole, left to right.
//
// Counts. Each class carries counts indexed by (stones s, first-player stones x) ("per-ply"
// mode), or only by d = x - o ("--total" mode, which gives the total and the even/odd-ply split
// without the s dimension). Entries that can no longer end with x - o in {0, 1} are pruned.
//
// Colour symmetry. Swapping the colours of a class maps its counts to (s, o, x). Only the
// smaller key of each {K, swap(K)} pair is stored, which halves the memory (--nosym disables).
//
// Overflow. Counters are 64-bit by default, and the final single-class column is 128-bit. Every
// addition checks for carry, and the run aborts if any 64-bit counter overflows (rerun with
// --u128). With --u128, all counters are 128-bit.
//
// usage: c4count W H [--total] [--u128] [--nosym] [--threads N] [--maxgb G] [--dry] [--brute]
//   --dry   build the class sets only and print the memory each column would need
//   --brute also count by brute-force enumeration and compare per ply (small boards only)
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <sys/mman.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef unsigned __int128 u128;

static int W, H, CELLS, THREADS;
static bool BRUTE = false, TOTAL = false, U128 = false, NOSYM = false, DRY = false;
static double MAXGB = 0;
static std::atomic<bool> OVERFLOW{false};

static double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
[[noreturn]] static void die(const char* m) { fprintf(stderr, "error: %s\n", m); exit(1); }
static void* bigalloc(size_t bytes) {
  if (!bytes) bytes = 1;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) die("mmap failed");
  return p;
}
static double memAvailGB() {
  FILE* f = fopen("/proc/meminfo", "r");
  char line[256];
  unsigned long long kb = 0;
  while (f && fgets(line, sizeof line, f))
    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
  if (f) fclose(f);
  return kb / 1048576.0;
}
static std::string u128str(u128 v) {
  if (!v) return "0";
  std::string s;
  int n = 0;
  while (v) {
    if (n && n % 3 == 0) s += ',';
    s += char('0' + (int)(v % 10));
    v /= 10;
    ++n;
  }
  return std::string(s.rbegin(), s.rend());
}

// ---- column states (no vertical four) ----
struct Col { int h, x; int cell[16]; };   // cell: 0 empty, 1 = O, 2 = X (first player)
static std::vector<Col> cols;
static void initCols() {
  for (int h = 0; h <= H; ++h)
    for (int p = 0; p < (1 << h); ++p) {
      bool bad = false;
      for (int r = 0; r + 3 < h; ++r) {
        int c = (p >> r) & 1;
        if (((p >> (r + 1)) & 1) == c && ((p >> (r + 2)) & 1) == c && ((p >> (r + 3)) & 1) == c) bad = true;
      }
      if (bad) continue;
      Col c{h, __builtin_popcount(p), {}};
      for (int r = 0; r < H; ++r) c.cell[r] = r < h ? (((p >> r) & 1) ? 2 : 1) : 0;
      cols.push_back(c);
    }
}

// ---- class keys: 8 bits per row = colour(2) | h(2)<<2 | u(2)<<4 | d(2)<<6 ----
static inline int fld(u128 k, int r) { return (int)(k >> (8 * r)) & 255; }
// Places column e after a frontier F with `rem` columns still to come. Returns false on a four.
static inline bool step(u128 F, const Col& e, int rem, u128& T) {
  T = 0;
  for (int r = 0; r < e.h; ++r) {
    int col = e.cell[r];
    int f = fld(F, r);
    int h = ((f & 3) == col && (f >> 2 & 3)) ? (f >> 2 & 3) + 1 : 1;
    int u = 1, d = 1;
    if (r > 0) { int g = fld(F, r - 1); if ((g & 3) == col && (g >> 4 & 3)) u = (g >> 4 & 3) + 1; }
    if (r + 1 < H) { int g = fld(F, r + 1); if ((g & 3) == col && (g >> 6 & 3)) d = (g >> 6 & 3) + 1; }
    if (h >= 4 || u >= 4 || d >= 4) return false;
    if (rem < 4 - h) h = 0;
    if (rem < 4 - u || r + (4 - u) > H - 1) u = 0;
    if (rem < 4 - d || r - (4 - d) < 0) d = 0;
    if (h | u | d) T |= (u128)(col | h << 2 | u << 4 | d << 6) << (8 * r);
  }
  return true;
}
static inline u128 swapKey(u128 K) {
  u128 S = 0;
  for (int r = 0; r < H; ++r) {
    int f = fld(K, r);
    if (f) f ^= 3;
    S |= (u128)f << (8 * r);
  }
  return S;
}
static inline u64 hash128(u128 k) {
  u64 a = (u64)k, b = (u64)(k >> 64);
  u64 h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
  return h;
}

// ---- sharded concurrent set of class keys ----
static const u128 EMPTYK = ~(u128)0;
struct KeySet {
  static const int SH = 4096;
  struct Shard { std::mutex m; u128* t = nullptr; u32* idx = nullptr; u64 cap = 0, n = 0; };
  Shard sh[SH];
  u64 total = 0;
  static void put(u128* t, u64 cap, u128 k, u64 h) {
    u64 p = h & (cap - 1);
    while (t[p] != EMPTYK) p = (p + 1) & (cap - 1);
    t[p] = k;
  }
  void insert(u128 k) {
    u64 h = hash128(k);
    Shard& s = sh[h >> 52];
    std::lock_guard<std::mutex> g(s.m);
    if (!s.t || (s.n + 1) * 2 > s.cap) {
      u64 nc = s.cap ? s.cap * 2 : 64;
      u128* nt = (u128*)malloc(nc * sizeof(u128));
      for (u64 i = 0; i < nc; ++i) nt[i] = EMPTYK;
      for (u64 i = 0; i < s.cap; ++i)
        if (s.t[i] != EMPTYK) put(nt, nc, s.t[i], hash128(s.t[i]));
      free(s.t);
      s.t = nt; s.cap = nc;
    }
    u64 p = h & (s.cap - 1);
    for (;;) {
      if (s.t[p] == k) return;
      if (s.t[p] == EMPTYK) { s.t[p] = k; ++s.n; return; }
      p = (p + 1) & (s.cap - 1);
    }
  }
  // Numbers the keys 0..total-1 and returns them in that order.
  std::vector<u128> freeze() {
    std::vector<u64> base(SH + 1, 0);
    for (int i = 0; i < SH; ++i) base[i + 1] = base[i] + sh[i].n;
    total = base[SH];
    std::vector<u128> keys(total);
    std::vector<std::thread> th;
    std::atomic<int> ctr{0};
    for (int t = 0; t < THREADS; ++t)
      th.emplace_back([&] {
        for (int i; (i = ctr.fetch_add(1)) < SH;) {
          Shard& s = sh[i];
          s.idx = (u32*)malloc((s.cap ? s.cap : 1) * sizeof(u32));
          u64 j = base[i];
          for (u64 p = 0; p < s.cap; ++p)
            if (s.t[p] != EMPTYK) { s.idx[p] = (u32)j; keys[j++] = s.t[p]; }
        }
      });
    for (auto& x : th) x.join();
    if (total >= (1ull << 32)) die("more than 2^32 classes");
    return keys;
  }
  u64 find(u128 k) const {
    u64 h = hash128(k);
    const Shard& s = sh[h >> 52];
    u64 p = h & (s.cap - 1);
    while (s.t[p] != k) p = (p + 1) & (s.cap - 1);
    return s.idx[p];
  }
  void clear() {
    for (auto& s : sh) { free(s.t); free(s.idx); s.t = nullptr; s.idx = nullptr; s.cap = s.n = 0; }
    total = 0;
  }
};

// ---- per-class count layout after `placed` columns ----
struct Layout {
  int P, R, M;                 // stones placed so far (max), cells remaining, |d| bound (total mode)
  std::vector<int> lo, len;    // per-ply: per s, the x range kept
  std::vector<u64> off;
  u64 per = 0;                 // entries per class
  void make(int placed) {
    P = placed * H; R = CELLS - P;
    lo.clear(); len.clear(); off.clear(); per = 0;
    if (TOTAL) {
      M = std::min(P, R + 1);
      per = 2 * M + 1;
      return;
    }
    for (int s = 0; s <= P; ++s) {
      // keep d = 2x - s in [-(R+1), R+1], so the final x - o can still be 0 or 1
      int l = s - R - 1 > 0 ? (s - R) / 2 : 0;   // ceil((s - R - 1) / 2), at least 0
      int h = std::min(s, (s + R + 1) / 2);
      lo.push_back(l); len.push_back(h - l + 1); off.push_back(per);
      per += h - l + 1;
    }
  }
};

// counts storage: `wd` u64 words per entry (1 or 2)
struct Arr {
  u64* a = nullptr; int wd = 1; u64 n = 0;
  void alloc(u64 entries, int w) { wd = w; n = entries; a = (u64*)bigalloc(entries * wd * 8); }
  void release() { if (a) munmap(a, n * wd * 8); a = nullptr; }
  inline u128 get(u64 i) const { return wd == 1 ? (u128)a[i] : ((u128)a[2 * i + 1] << 64 | a[2 * i]); }
  inline void add(u64 i, u128 v) {
    if (wd == 1) {
      if (v >> 64) { OVERFLOW = true; return; }
      u64 old = __atomic_fetch_add(&a[i], (u64)v, __ATOMIC_RELAXED);
      if (old + (u64)v < old) OVERFLOW = true;
    } else {
      u64 vl = (u64)v, vh = (u64)(v >> 64);
      u64 old = __atomic_fetch_add(&a[2 * i], vl, __ATOMIC_RELAXED);
      vh += (old + vl < old);
      if (vh) {
        u64 oh = __atomic_fetch_add(&a[2 * i + 1], vh, __ATOMIC_RELAXED);
        if (oh + vh < oh) OVERFLOW = true;
      }
    }
  }
};

// Adds class src (layout L0) placed with column e into class dst (layout L1), optionally with
// the colours swapped (flip).
static inline void addInto(Arr& D, u64 dbase, const Layout& L1, const Arr& S, u64 sbase,
                           const Layout& L0, const Col& e, bool flip) {
  if (TOTAL) {
    int sh = 2 * e.x - e.h;
    for (int d = -L0.M; d <= L0.M; ++d) {
      u128 v = S.get(sbase + (u64)(d + L0.M));
      if (!v) continue;
      int d1 = d + sh;
      if (d1 < -L1.M || d1 > L1.M) continue;
      D.add(dbase + (u64)(flip ? L1.M - d1 : d1 + L1.M), v);
    }
    return;
  }
  for (int s = 0; s <= L0.P; ++s) {
    int s1 = s + e.h;
    int lo1 = L1.lo[s1], len1 = L1.len[s1];
    u64 o0 = sbase + L0.off[s], o1 = dbase + L1.off[s1];
    for (int i = 0; i < L0.len[s]; ++i) {
      u128 v = S.get(o0 + i);
      if (!v) continue;
      int j = L0.lo[s] + i + e.x - lo1;
      if (j < 0 || j >= len1) continue;
      D.add(o1 + (u64)(flip ? len1 - 1 - j : j), v);
    }
  }
}

// Brute force for validation, independent of the class machinery: places whole columns on a
// cell grid, left to right, and checks every horizontal/diagonal four through each new column
// directly on the grid. Depth-first, parallel over the first two columns. Small boards only.
static std::vector<u128> bruteCount() {
  const int nv = (int)cols.size();
  std::vector<std::vector<u128>> acc(THREADS, std::vector<u128>(CELLS + 1, 0));
  std::atomic<int> ctr{0};
  std::vector<std::thread> th;
  for (int t = 0; t < THREADS; ++t)
    th.emplace_back([&, t] {
      std::vector<int> g(W * H, 0);
      auto at = [&](int c, int r) { return (c < 0 || r < 0 || r >= H) ? -1 : g[c * H + r]; };
      auto four = [&](int c) {
        for (int r = 0; r < H; ++r) {
          int v = g[c * H + r];
          if (!v) continue;
          for (int dr = -1; dr <= 1; ++dr) {
            int k = 1;
            while (k < 4 && at(c - k, r - k * dr) == v) ++k;
            if (k == 4) return true;
          }
        }
        return false;
      };
      auto put = [&](int c, const Col& e) { for (int r = 0; r < H; ++r) g[c * H + r] = e.cell[r]; };
      auto rec = [&](auto&& self, int c, int s, int x) -> void {
        if (c == W) { if (x == (s + 1) / 2) acc[t][s]++; return; }
        for (const Col& e : cols) {
          put(c, e);
          if (!four(c)) self(self, c + 1, s + e.h, x + e.x);
        }
        for (int r = 0; r < H; ++r) g[c * H + r] = 0;
      };
      for (int item; (item = ctr.fetch_add(1)) < nv * nv;) {
        std::fill(g.begin(), g.end(), 0);
        const Col& a = cols[item / nv]; const Col& b = cols[item % nv];
        if (W == 1) { if (item % nv) continue; put(0, a); if (a.x == (a.h + 1) / 2) acc[t][a.h]++; continue; }
        put(0, a); put(1, b);
        if (four(1)) continue;
        rec(rec, 2, a.h + b.h, a.x + b.x);
      }
    });
  for (auto& x : th) x.join();
  std::vector<u128> r(CELLS + 1, 0);
  for (int t = 0; t < THREADS; ++t)
    for (int k = 0; k <= CELLS; ++k) r[k] += acc[t][k];
  return r;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s W H [--total] [--u128] [--nosym] [--threads N] [--maxgb G] [--dry] [--brute]\n", argv[0]);
    return 1;
  }
  W = atoi(argv[1]); H = atoi(argv[2]); CELLS = W * H;
  THREADS = (int)std::thread::hardware_concurrency();
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--total") TOTAL = true;
    else if (a == "--u128") U128 = true;
    else if (a == "--nosym") NOSYM = true;
    else if (a == "--dry") DRY = true;
    else if (a == "--brute") BRUTE = true;
    else if (a == "--threads" && i + 1 < argc) THREADS = atoi(argv[++i]);
    else if (a == "--maxgb" && i + 1 < argc) MAXGB = atof(argv[++i]);
    else die("unknown argument");
  }
  if (W < 1 || H < 1 || H > 15) die("need W >= 1 and 1 <= H <= 15");
  if (MAXGB <= 0) MAXGB = 0.6 * memAvailGB();
  initCols();
  printf("[c4count] %dx%d, %zu column states, %s mode, %s counters, colour symmetry %s, %d threads, "
         "memory cap %.0f GB\n", W, H, cols.size(), TOTAL ? "total" : "per-ply", U128 ? "128-bit" : "64-bit",
         NOSYM ? "off" : "on", THREADS, MAXGB);
  fflush(stdout);
  double t00 = now_s();

  std::vector<u128> keys0{0};
  Layout L0; L0.make(0);
  Arr A0;
  if (!DRY) { A0.alloc(L0.per, 2); A0.add(0, 1); }   // the empty board
  KeySet* ks = new KeySet;
  double peakGB = 0;

  for (int c = 0; c < W; ++c) {
    double t0 = now_s();
    int rem = W - 1 - c;
    // phase A: the class set after column c
    std::atomic<u64> ctr{0};
    auto par = [&](auto&& fn) {
      ctr = 0;
      std::vector<std::thread> th;
      for (int t = 0; t < THREADS; ++t)
        th.emplace_back([&] {
          for (;;) {
            u64 b = ctr.fetch_add(64);
            if (b >= keys0.size()) break;
            u64 e = std::min<u64>(b + 64, keys0.size());
            for (u64 i = b; i < e; ++i) fn(i);
          }
        });
      for (auto& x : th) x.join();
    };
    par([&](u64 i) {
      u128 F = keys0[i], T;
      for (const Col& e : cols)
        if (step(F, e, rem, T)) {
          if (!NOSYM) { u128 S = swapKey(T); if (S < T) T = S; }
          ks->insert(T);
        }
    });
    std::vector<u128> keys1 = ks->freeze();
    Layout L1; L1.make(c + 1);
    int wd = (U128 || keys1.size() <= 4096) ? 2 : 1;
    double gb = (double)keys1.size() * L1.per * wd * 8 / 1e9;
    double gb0 = (double)keys0.size() * L0.per * (DRY ? wd : A0.wd) * 8 / 1e9;
    peakGB = std::max(peakGB, gb + gb0);
    printf("[col %2d] classes %15s   entries/class %5llu   counts %8.1f GB (+%.1f GB previous)   %.1fs\n", c,
           u128str(keys1.size()).c_str(), (unsigned long long)L1.per, gb, gb0, now_s() - t0);
    fflush(stdout);
    if (gb + gb0 > MAXGB) {
      printf("[c4count] would need %.1f GB at column %d, above the %.0f GB cap — stopping\n", gb + gb0, c, MAXGB);
      if (!DRY) return 2;
    }
    if (!DRY) {
      // phase C: accumulate counts
      Arr A1; A1.alloc(keys1.size() * L1.per, wd);
      par([&](u64 i) {
        u128 F = keys0[i], T;
        bool selfF = NOSYM || swapKey(F) == F;
        u64 sb = i * L0.per;
        for (const Col& e : cols) {
          if (!step(F, e, rem, T)) continue;
          if (NOSYM) { addInto(A1, ks->find(T) * L1.per, L1, A0, sb, L0, e, false); continue; }
          u128 S = swapKey(T);
          if (S == T) {
            u64 db = ks->find(T) * L1.per;
            addInto(A1, db, L1, A0, sb, L0, e, false);
            if (!selfF) addInto(A1, db, L1, A0, sb, L0, e, true);
          } else if (T < S) {
            addInto(A1, ks->find(T) * L1.per, L1, A0, sb, L0, e, false);
          } else if (!selfF) {
            addInto(A1, ks->find(S) * L1.per, L1, A0, sb, L0, e, true);
          }
        }
      });
      if (OVERFLOW) die("a 64-bit counter overflowed; rerun with --u128");
      A0.release();
      A0 = A1;
      printf("[col %2d] counted in %.1fs\n", c, now_s() - t0);
      fflush(stdout);
    }
    ks->clear();
    keys0.swap(keys1);
    L0 = L1;
  }
  printf("[c4count] peak count memory %.1f GB\n", peakGB);
  if (DRY) return 0;
  if (keys0.size() != 1 || keys0[0] != 0) die("final frontier is not the single empty class");

  u128 total = 0;
  if (TOTAL) {
    u128 ev = A0.get(L0.M), od = A0.get(L0.M + 1);
    total = ev + od;
    printf("[c4count] even plies: %s\n[c4count] odd plies:  %s\n", u128str(ev).c_str(), u128str(od).c_str());
  } else {
    printf("[c4count] pseudo non-terminal positions per ply:\n");
    for (int s = 0; s <= CELLS; ++s) {
      int x = (s + 1) / 2, j = x - L0.lo[s];
      u128 v = (j >= 0 && j < L0.len[s]) ? A0.get(L0.off[s] + j) : 0;
      total += v;
      printf("  ply %3d: %s\n", s, u128str(v).c_str());
    }
  }
  printf("[c4count] TOTAL: %s positions (%.1fs)\n", u128str(total).c_str(), now_s() - t00);
  if (BRUTE && !TOTAL) {
    double tb = now_s();
    std::vector<u128> b = bruteCount();
    bool ok = true;
    for (int s = 0; s <= CELLS; ++s) {
      int x = (s + 1) / 2, j = x - L0.lo[s];
      u128 v = (j >= 0 && j < L0.len[s]) ? A0.get(L0.off[s] + j) : 0;
      if (v != b[s]) ok = false;
    }
    printf("[brute] full enumeration crosscheck: %s (%.1fs)\n", ok ? "OK" : "MISMATCH", now_s() - tb);
    if (!ok) return 1;
  }
  return 0;
}
