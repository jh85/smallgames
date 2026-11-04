/*
  dense — full-envelope, 1-bit-per-position retrograde tables for small Amazons
  boards, and exact minimal-DFA measurements derived from them.

  It exists to check the symbolic engine (amzdfa) with a different method and
  to measure, on real solved layers, how well DFAs compress Amazons.  It shares
  no code with amzdfa or with the older search solver.

  A position is (M, O, A): mover queens, opponent queens, arrows.  A *slab* is
  one queen placement (M, O).  Slabs are reduced by the board symmetries; every
  arrow subset of the free cells of a canonical slab has one bit, addressed by
  its colex rank.  Slabs are word-aligned, so workers never share a word in the
  backward pass and each worker reads a few child slabs repeatedly, which keeps
  most probes in cache.

      W_k(p) = exists a move to a child c with not W_{k+1}(c)

  The table covers every placement, so no forward pass is required.  --reach
  adds one, marking the positions reachable from the standard start.

  Commands
    dense solve WxH DIR [--threads T] [--queens Q] [--reach] [--verify-samples N]
        writes DIR/W<k>.bits (and R<k>.bits with --reach), prints per-layer counts
    dense dfa WxH DIR --k K [--set W|L|R|RW|RL|WN] [--order NAME] [--threads T]
        builds the minimal layered DFA of one layer's set by full enumeration
    dense stats WxH DIR [--threads T]
        entropy bounds on how far the tables could be compressed
    dense forest WxH DIR [--k K] [--threads T]
        size of a hash-consed forest of slab bit vectors
    dense dead WxH DIR [--set OUTDIR] [--threads T]
        share of positions with an unusable empty cell; optionally writes
        tables with those bits cleared, to measure the compression gain
    dense reorder WxH DIR --k K --set OUTFILE [--order rev|center|outer|col] [--threads T]
        one layer with slabs sorted by opponent set first or, with --order,
        with its arrow bits renumbered
    dense proof WxH DIR [--set T|S|TS] [--threads T]
        size of a greedy weak-solution proof from the start
        (T = use the HxW start, S = stop at positions a static bound decides)

  A table covers every placement, so the WxH table also answers the HxW board
  (the same board, rotated), and any other setup.  `solve` prints both roots.
*/

#include <fcntl.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

using Clock = std::chrono::steady_clock;
Clock::time_point g_t0;
int g_threads = 64;

double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "dense: FATAL: %s\n", msg.c_str());
  std::exit(1);
}

template <class F>
void ParallelFor(size_t n, F&& fn) {
  const size_t t = std::min<size_t>(size_t(g_threads), n);
  if (t <= 1) {
    for (size_t i = 0; i < n; ++i) fn(i);
    return;
  }
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n) break;
      fn(i);
    }
  };
  std::vector<std::thread> pool;
  pool.reserve(t - 1);
  for (size_t i = 1; i < t; ++i) pool.emplace_back(work);
  work();
  for (auto& th : pool) th.join();
}

// ------------------------------------------------------------------ board

int W, H, N, Q = 4, F;
u64 BOARD;
u64 RAY[64][8];
const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
const int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
bool POSDIR[8];
int GN;                 // symmetry group size
int GPERM[8][64];       // g, cell -> cell
u64 GBIT[8][64];        // g, cell -> bit of the image cell
u64 GT[8][3][1 << 11];  // g, chunk, 11 bits -> image board
u64 C[65][65];

inline u64 Transform(int g, u64 b) {
  return GT[g][0][b & 2047] | GT[g][1][(b >> 11) & 2047] | GT[g][2][(b >> 22) & 2047];
}

inline u64 Attacks(int c, u64 occ) {
  u64 r = 0;
  for (int d = 0; d < 8; ++d) {
    const u64 ray = RAY[c][d];
    const u64 b = ray & occ;
    if (!b) {
      r |= ray;
    } else if (POSDIR[d]) {
      r |= ray & ((u64(1) << __builtin_ctzll(b)) - 1);
    } else {
      r |= ray & ~((u64(2) << (63 - __builtin_clzll(b))) - 1);
    }
  }
  return r;
}

inline u64 RankSubset(u64 mask) {
  u64 r = 0;
  int i = 1;
  while (mask) {
    r += C[__builtin_ctzll(mask)][i++];
    mask &= mask - 1;
  }
  return r;
}

inline u64 NextSubset(u64 v) {  // Gosper
  const u64 t = v | (v - 1);
  return (t + 1) | (((~t & (t + 1)) - 1) >> (__builtin_ctzll(v) + 1));
}

void InitExpand();

void InitBoard(int w, int h, int q) {
  W = w, H = h, N = w * h, Q = q, F = N - 2 * q;
  if (N > 33) Die("board too large for dense tables");
  if (F > 26) Die("too many free cells for the arrow rank table");
  BOARD = (N == 64) ? ~u64(0) : (u64(1) << N) - 1;
  for (int n = 0; n <= 64; ++n) {
    C[n][0] = 1;
    for (int k = 1; k <= n; ++k) C[n][k] = C[n - 1][k - 1] + (k < n ? C[n - 1][k] : 0);
  }
  for (int d = 0; d < 8; ++d) POSDIR[d] = (DY[d] * w + DX[d]) > 0;
  for (int c = 0; c < N; ++c) {
    const int x = c % w, y = c / w;
    for (int d = 0; d < 8; ++d) {
      u64 m = 0;
      for (int nx = x + DX[d], ny = y + DY[d]; nx >= 0 && nx < w && ny >= 0 && ny < h; nx += DX[d], ny += DY[d]) {
        m |= u64(1) << (ny * w + nx);
      }
      RAY[c][d] = m;
    }
  }
  InitExpand();
  GN = (w == h) ? 8 : 4;
  for (int g = 0; g < GN; ++g) {
    const bool swap_axes = g & 4, flip_x = g & 1, flip_y = g & 2;
    for (int c = 0; c < N; ++c) {
      int x = c % w, y = c / w;
      if (swap_axes) std::swap(x, y);
      if (flip_x) x = w - 1 - x;
      if (flip_y) y = h - 1 - y;
      GPERM[g][c] = y * w + x;
      GBIT[g][c] = u64(1) << GPERM[g][c];
    }
    for (int chunk = 0; chunk < 3; ++chunk) {
      for (int v = 0; v < (1 << 11); ++v) {
        u64 r = 0;
        for (int b = 0; b < 11; ++b) {
          const int c = chunk * 11 + b;
          if ((v >> b) & 1 && c < N) r |= GBIT[g][c];
        }
        GT[g][chunk][v] = r;
      }
    }
  }
}

// Exact reduction: an empty cell in an 8-connected component of non-arrow
// cells that holds no queen can never be entered or shot at, so it plays like
// an arrow.  Such a position has the value of the position with that cell
// burned, one or more layers later.
u64 g_not_left, g_not_right;

inline u64 Expand8(u64 b) {
  const u64 h = b | ((b & g_not_right) << 1) | ((b & g_not_left) >> 1);
  return (h | (h << W) | (h >> W)) & BOARD;
}

inline u64 DeadCells(u64 queens, u64 arrows) {
  const u64 open = BOARD & ~arrows;
  u64 live = queens;
  for (;;) {
    const u64 next = Expand8(live) & open;
    if (next == live) break;
    live = next;
  }
  return open & ~live;
}

void InitExpand() {
  g_not_left = g_not_right = BOARD;
  for (int y = 0; y < H; ++y) {
    g_not_left &= ~(u64(1) << (y * W));
    g_not_right &= ~(u64(1) << (y * W + W - 1));
  }
}

// ------------------------------------------------------------------ slabs

struct Slab {
  u64 m, o;
};

std::vector<u64> g_ms;        // all mover subsets in numeric order
std::vector<u32> g_canon;     // raw slab -> slab id << 3 | g (raw image under g is the canonical one)
std::vector<Slab> g_slabs;    // canonical slabs
std::vector<u8> g_stab;       // slab id -> mask of symmetries fixing it
u64 g_raw_per_m;              // C(N-Q, Q)

inline u64 RawIndex(u64 m, u64 o) { return RankSubset(m) * g_raw_per_m + RankSubset(_pext_u64(o, ~m & BOARD)); }

void BuildSlabs() {
  const double t0 = Now();
  g_raw_per_m = C[N - Q][Q];
  {
    u64 v = (u64(1) << Q) - 1;
    const u64 count = C[N][Q];
    g_ms.resize(count);
    for (u64 i = 0; i < count; ++i) {
      g_ms[i] = v;
      if (i + 1 < count) v = NextSubset(v);
    }
  }
  const u64 raw = g_ms.size() * g_raw_per_m;
  if (raw >= (u64(1) << 32)) Die("too many raw slabs");
  g_canon.resize(raw);
  std::vector<u64> canon_count(g_ms.size() + 1, 0);
  const u64 free_after = (u64(1) << Q) - 1;
  // First pass: mark canonical slabs; assign ids in raw order afterwards.
  ParallelFor(g_ms.size(), [&](size_t i) {
    const u64 m = g_ms[i];
    const u64 rest = ~m & BOARD;
    u64 v = free_after;
    u64 local = 0;
    for (u64 j = 0; j < g_raw_per_m; ++j) {
      const u64 o = _pdep_u64(v, rest);
      bool canon = true;
      for (int g = 1; g < GN && canon; ++g) {
        const u64 gm = Transform(g, m);
        if (gm < m) canon = false;
        else if (gm == m && Transform(g, o) < o) canon = false;
      }
      g_canon[i * g_raw_per_m + j] = canon ? 1 : 0;
      local += canon;
      if (j + 1 < g_raw_per_m) v = NextSubset(v);
    }
    canon_count[i] = local;
  });
  u64 total = 0;
  std::vector<u64> base(g_ms.size());
  for (size_t i = 0; i < g_ms.size(); ++i) {
    base[i] = total;
    total += canon_count[i];
  }
  if (total >= (u64(1) << 29)) Die("too many canonical slabs");
  g_slabs.resize(total);
  g_stab.resize(total);
  ParallelFor(g_ms.size(), [&](size_t i) {
    const u64 m = g_ms[i];
    const u64 rest = ~m & BOARD;
    u64 v = free_after;
    u64 id = base[i];
    for (u64 j = 0; j < g_raw_per_m; ++j) {
      if (g_canon[i * g_raw_per_m + j] == 1) {
        const u64 o = _pdep_u64(v, rest);
        g_slabs[id] = Slab{m, o};
        u8 stab = 0;
        for (int g = 0; g < GN; ++g)
          if (Transform(g, m) == m && Transform(g, o) == o) stab = u8(stab | (1u << g));
        g_stab[id] = stab;
        g_canon[i * g_raw_per_m + j] = u32(id << 3) | 0x80000000u;  // provisional mark, g = 0
        ++id;
      }
      if (j + 1 < g_raw_per_m) v = NextSubset(v);
    }
  });
  // Second pass: map every raw slab to its canonical id and symmetry.  Reads
  // only canonical entries, which no longer change.
  ParallelFor(g_ms.size(), [&](size_t i) {
    const u64 m = g_ms[i];
    const u64 rest = ~m & BOARD;
    u64 v = free_after;
    for (u64 j = 0; j < g_raw_per_m; ++j) {
      const u32 cur = g_canon[i * g_raw_per_m + j];
      if (!(cur & 0x80000000u)) {
        const u64 o = _pdep_u64(v, rest);
        u64 bm = ~u64(0), bo = ~u64(0);
        int bg = 0;
        for (int g = 0; g < GN; ++g) {
          const u64 gm = Transform(g, m), go = Transform(g, o);
          if (gm < bm || (gm == bm && go < bo)) {
            bm = gm, bo = go, bg = g;
          }
        }
        const u32 target = g_canon[RawIndex(bm, bo)];
        if (!(target & 0x80000000u)) Die("canonical slab not marked");
        g_canon[i * g_raw_per_m + j] = (target & 0x7FFFFFFFu) | u32(bg);
      }
      if (j + 1 < g_raw_per_m) v = NextSubset(v);
    }
  });
  // Clear the mark.  g = 0 maps a canonical slab to itself.
  ParallelFor(g_ms.size(), [&](size_t i) {
    for (u64 j = 0; j < g_raw_per_m; ++j) g_canon[i * g_raw_per_m + j] &= 0x7FFFFFFFu;
  });
  std::printf("# slabs: %" PRIu64 " raw, %" PRIu64 " canonical (%.1fs)\n", raw, total, Now() - t0);
  std::fflush(stdout);
}

// Colex rank of an arrow subset (numeric order within one popcount).
std::vector<u32> g_arank;
std::vector<std::vector<u32>> g_aunrank;

void BuildArrowRanks() {
  g_arank.resize(size_t(1) << F);
  g_aunrank.assign(size_t(F) + 1, {});
  for (int k = 0; k <= F; ++k) g_aunrank[size_t(k)].reserve(C[F][k]);
  for (u64 v = 0; v < (u64(1) << F); ++v) {
    const int k = __builtin_popcountll(v);
    g_arank[v] = u32(g_aunrank[size_t(k)].size());
    g_aunrank[size_t(k)].push_back(u32(v));
  }
}

inline u64 StrideWords(int k) { return (C[F][k] + 63) / 64; }

struct Table {
  std::unique_ptr<u64[]> owned;
  u64* d = nullptr;
  u64 words = 0;
  void* mapped = nullptr;

  void Alloc(u64 w) {
    words = w;
    owned.reset(new u64[w]);
    d = owned.get();
    ParallelFor(size_t(g_threads), [&](size_t t) {
      const u64 b = w * t / u64(g_threads), e = w * (t + 1) / u64(g_threads);
      std::memset(d + b, 0, (e - b) * 8);
    });
  }
  void Save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
    if (fd < 0) Die("cannot create " + tmp);
    const char* p = reinterpret_cast<const char*>(d);
    u64 left = words * 8;
    while (left) {
      const ssize_t got = ::write(fd, p, std::min<u64>(left, u64(1) << 30));
      if (got <= 0) Die("write failed: " + tmp);
      p += got;
      left -= u64(got);
    }
    ::close(fd);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) Die("rename failed: " + path);
  }
  bool Map(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0) Die("stat failed: " + path);
    words = u64(st.st_size) / 8;
    mapped = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) Die("mmap failed: " + path);
    d = static_cast<u64*>(mapped);
    return true;
  }
};

// Counts positions of a canonical-slab table: raw = weighted by orbit size;
// classes = distinct positions up to symmetry.
void CountTable(const u64* t, int k, u64* raw, u64* classes) {
  const u64 stride = StrideWords(k);
  std::atomic<u64> total_raw{0}, total_classes{0};
  const size_t chunk = 1 << 12;
  const size_t tasks = (g_slabs.size() + chunk - 1) / chunk;
  ParallelFor(tasks, [&](size_t task) {
    u64 local_raw = 0, local_classes = 0;
    const size_t b = task * chunk, e = std::min(g_slabs.size(), b + chunk);
    for (size_t id = b; id < e; ++id) {
      const u64* w = t + id * stride;
      u64 pc = 0;
      for (u64 i = 0; i < stride; ++i) pc += u64(__builtin_popcountll(w[i]));
      const u8 stab = g_stab[id];
      const int sz = __builtin_popcount(stab);
      local_raw += pc * u64(GN / sz);
      if (sz == 1 || pc == 0) {
        local_classes += pc;
      } else {
        // Count arrow sets that are minimal among their stabilizer images.
        const u64 free = ~(g_slabs[id].m | g_slabs[id].o) & BOARD;
        for (u64 i = 0; i < stride; ++i) {
          u64 word = w[i];
          while (word) {
            const u64 r = i * 64 + u64(__builtin_ctzll(word));
            word &= word - 1;
            const u64 a = _pdep_u64(g_aunrank[size_t(k)][r], free);
            bool minimal = true;
            for (int g = 1; g < GN && minimal; ++g) {
              if ((stab >> g) & 1) minimal = Transform(g, a) >= a;
            }
            if (minimal) ++local_classes;
          }
        }
      }
    }
    total_raw.fetch_add(local_raw);
    total_classes.fetch_add(local_classes);
  });
  *raw = total_raw.load();
  *classes = total_classes.load();
}

// Child slab info for one queen move of the slab being processed.
struct PairInfo {
  u64 child_free;
  const u64* base;
  u64 child_id;
  int g;
};

inline void FillPairs(const Slab& s, const u64* next, u64 next_stride, PairInfo pinfo[][64], int* queen_cell) {
  u64 mm = s.m;
  const u64 occq = s.m | s.o;
  for (int qi = 0; qi < Q; ++qi) {
    const int f = __builtin_ctzll(mm);
    mm &= mm - 1;
    queen_cell[qi] = f;
    u64 ts = Attacks(f, occq);
    while (ts) {
      const int t = __builtin_ctzll(ts);
      ts &= ts - 1;
      const u64 moved = (s.m & ~(u64(1) << f)) | (u64(1) << t);
      // The child is seen from the opponent's side: O moves, M' waits.
      const u32 ce = g_canon[RawIndex(s.o, moved)];
      const u64 cid = ce >> 3;
      PairInfo& p = pinfo[qi][t];
      p.g = int(ce & 7);
      p.child_id = cid;
      p.child_free = ~(g_slabs[cid].m | g_slabs[cid].o) & BOARD;
      p.base = next ? next + cid * next_stride : nullptr;
    }
  }
}

void SolveLayer(int k, const u64* next, u64* out, std::atomic<u64>* probes_total) {
  const u64 stride = StrideWords(k);
  const u64 next_stride = k < F ? StrideWords(k + 1) : 0;
  const u64 count = C[F][k];
  const size_t chunk = std::max<size_t>(1, std::min<size_t>(4096, size_t(1 << 24) / std::max<u64>(count, 1)));
  const size_t tasks = (g_slabs.size() + chunk - 1) / chunk;
  ParallelFor(tasks, [&](size_t task) {
    static thread_local PairInfo pinfo[8][64];
    int queen_cell[8];
    u64 probes = 0;
    const size_t b = task * chunk, e = std::min(g_slabs.size(), b + chunk);
    for (size_t id = b; id < e; ++id) {
      const Slab& s = g_slabs[id];
      u64* w = out + id * stride;
      if (k == F) continue;  // no empty cell: every position is lost
      FillPairs(s, next, next_stride, pinfo, queen_cell);
      const u64 occq = s.m | s.o;
      const u64 free = ~occq & BOARD;
      u64 v = k ? (u64(1) << k) - 1 : 0;
      for (u64 r = 0; r < count; ++r) {
        const u64 a = _pdep_u64(v, free);
        const u64 occ = occq | a;
        u64 ga[8];
        u8 have = 0;
        bool won = false;
        for (int qi = 0; qi < Q && !won; ++qi) {
          const int f = queen_cell[qi];
          const u64 occ2 = occ & ~(u64(1) << f);
          u64 ts = Attacks(f, occ);
          while (ts && !won) {
            const int t = __builtin_ctzll(ts);
            ts &= ts - 1;
            const PairInfo& p = pinfo[qi][t];
            const int g = p.g;
            if (!(have & (1u << g))) {
              ga[g] = Transform(g, a);
              have = u8(have | (1u << g));
            }
            u64 xs = Attacks(t, occ2);
            while (xs) {
              const int x = __builtin_ctzll(xs);
              xs &= xs - 1;
              const u64 a2 = ga[g] | GBIT[g][x];
              const u32 r2 = g_arank[_pext_u64(a2, p.child_free)];
              ++probes;
              if (!((p.base[r2 >> 6] >> (r2 & 63)) & 1)) {
                won = true;
                break;
              }
            }
          }
        }
        if (won) w[r >> 6] |= u64(1) << (r & 63);
        if (r + 1 < count) v = NextSubset(v);
      }
    }
    probes_total->fetch_add(probes, std::memory_order_relaxed);
  });
}

// Marks all children of the positions in `cur` in `next`.
void ForwardLayer(int k, const u64* cur, u64* next) {
  const u64 stride = StrideWords(k);
  const u64 next_stride = StrideWords(k + 1);
  const size_t chunk = 256;
  const size_t tasks = (g_slabs.size() + chunk - 1) / chunk;
  ParallelFor(tasks, [&](size_t task) {
    static thread_local PairInfo pinfo[8][64];
    int queen_cell[8];
    const size_t b = task * chunk, e = std::min(g_slabs.size(), b + chunk);
    for (size_t id = b; id < e; ++id) {
      const u64* w = cur + id * stride;
      bool any = false;
      for (u64 i = 0; i < stride; ++i) any = any || w[i];
      if (!any) continue;
      const Slab& s = g_slabs[id];
      FillPairs(s, nullptr, 0, pinfo, queen_cell);
      const u64 occq = s.m | s.o;
      const u64 free = ~occq & BOARD;
      for (u64 i = 0; i < stride; ++i) {
        u64 word = w[i];
        while (word) {
          const u64 r = i * 64 + u64(__builtin_ctzll(word));
          word &= word - 1;
          const u64 a = _pdep_u64(g_aunrank[size_t(k)][r], free);
          const u64 occ = occq | a;
          for (int qi = 0; qi < Q; ++qi) {
            const int f = queen_cell[qi];
            const u64 occ2 = occ & ~(u64(1) << f);
            u64 ts = Attacks(f, occ);
            while (ts) {
              const int t = __builtin_ctzll(ts);
              ts &= ts - 1;
              const PairInfo& p = pinfo[qi][t];
              const u64 ga = Transform(p.g, a);
              const u8 stab = g_stab[p.child_id];
              u64* cw = next + p.child_id * next_stride;
              u64 xs = Attacks(t, occ2);
              while (xs) {
                const int x = __builtin_ctzll(xs);
                xs &= xs - 1;
                const u64 a2 = ga | GBIT[p.g][x];
                for (int g2 = 0; g2 < GN; ++g2) {
                  if (!((stab >> g2) & 1)) continue;
                  const u64 a3 = g2 ? Transform(g2, a2) : a2;
                  const u32 r2 = g_arank[_pext_u64(a3, p.child_free)];
                  const u64 mask = u64(1) << (r2 & 63);
                  std::atomic_ref<u64> cell(cw[r2 >> 6]);
                  if (!(cell.load(std::memory_order_relaxed) & mask)) cell.fetch_or(mask, std::memory_order_relaxed);
                }
              }
            }
          }
        }
      }
    }
  });
}

inline bool ReadBit(const u64* t, u64 stride, u64 id, u32 r) { return (t[id * stride + (r >> 6)] >> (r & 63)) & 1; }

// Looks up an arbitrary position in a canonical table.
inline bool Lookup(const u64* t, int k, u64 m, u64 o, u64 a) {
  const u32 ce = g_canon[RawIndex(m, o)];
  const u64 id = ce >> 3;
  const int g = int(ce & 7);
  const u64 free = ~(g_slabs[id].m | g_slabs[id].o) & BOARD;
  const u32 r = g_arank[_pext_u64(Transform(g, a), free)];
  return ReadBit(t, StrideWords(k), id, r);
}

void StartPosition(u64* m, u64* o) {
  auto bit = [&](int x, int y) { return u64(1) << (y * W + x); };
  *m = bit(1, 0) | bit(W - 2, 0) | bit(0, 1) | bit(W - 1, 1);
  *o = bit(0, H - 2) | bit(W - 1, H - 2) | bit(1, H - 1) | bit(W - 2, H - 1);
}

// The standard start of the HxW board, written on this WxH board by
// transposing coordinates.
void StartPositionTransposed(u64* m, u64* o) {
  auto bit = [&](int x, int y) { return u64(1) << (x * W + y); };  // (x, y) on HxW is (y, x) here
  *m = bit(1, 0) | bit(H - 2, 0) | bit(0, 1) | bit(H - 1, 1);
  *o = bit(0, W - 2) | bit(H - 1, W - 2) | bit(1, W - 1) | bit(H - 2, W - 1);
}

// Explicit (grid-free, ray-walking) re-derivation of one position.  Used by
// the sample check: it walks rays cell by cell rather than using Attacks().
bool ExplicitWin(u64 m, u64 o, u64 a, int k, const u64* next) {
  const u64 occ = m | o | a;
  u64 mm = m;
  while (mm) {
    const int f = __builtin_ctzll(mm);
    mm &= mm - 1;
    const int fx = f % W, fy = f / W;
    for (int d = 0; d < 8; ++d) {
      for (int x = fx + DX[d], y = fy + DY[d]; x >= 0 && x < W && y >= 0 && y < H; x += DX[d], y += DY[d]) {
        const int t = y * W + x;
        if ((occ >> t) & 1) break;
        const u64 moved = (m & ~(u64(1) << f)) | (u64(1) << t);
        const u64 occ2 = (occ & ~(u64(1) << f)) | (u64(1) << t);
        for (int d2 = 0; d2 < 8; ++d2) {
          for (int ax = x + DX[d2], ay = y + DY[d2]; ax >= 0 && ax < W && ay >= 0 && ay < H; ax += DX[d2], ay += DY[d2]) {
            const int c = ay * W + ax;
            if ((occ2 >> c) & 1) break;
            if (!Lookup(next, k + 1, o, moved, a | (u64(1) << c))) return true;
          }
        }
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------- commands

int CmdSolve(const std::string& dir, bool reach, u64 samples) {
  ::mkdir(dir.c_str(), 0775);
  BuildSlabs();
  BuildArrowRanks();
  const u64 S = g_slabs.size();
  double table_gib = 0;
  for (int k = 0; k <= F; ++k) table_gib += double(S * StrideWords(k)) * 8 / (1ULL << 30);
  std::printf("# %dx%d q=%d cells=%d free=%d group=%d threads=%d: full table %.1f GiB per set\n", W, H, Q, N, F, GN,
              g_threads, table_gib);
  std::fflush(stdout);

  std::vector<u64> reach_raw(size_t(F) + 1, 0), reach_classes(size_t(F) + 1, 0);
  if (reach) {
    std::printf("# forward pass\n");
    u64 ms, os;
    StartPosition(&ms, &os);
    Table cur;
    cur.Alloc(S * StrideWords(0));
    {
      const u32 ce = g_canon[RawIndex(ms, os)];
      cur.d[(ce >> 3) * StrideWords(0)] |= 1;
    }
    for (int k = 0; k <= F; ++k) {
      const double t0 = Now();
      CountTable(cur.d, k, &reach_raw[size_t(k)], &reach_classes[size_t(k)]);
      cur.Save(dir + "/R" + std::to_string(k) + ".bits");
      std::printf("#  R k=%d raw=%" PRIu64 " classes=%" PRIu64 "\n", k, reach_raw[size_t(k)], reach_classes[size_t(k)]);
      std::fflush(stdout);
      Table next;
      if (k < F) {
        next.Alloc(S * StrideWords(k + 1));
        if (reach_raw[size_t(k)]) ForwardLayer(k, cur.d, next.d);
      }
      std::printf("#  forward from k=%d took %.1fs\n", k, Now() - t0);
      std::fflush(stdout);
      cur = std::move(next);
    }
  }

  std::printf("#%3s %18s %18s %18s %14s %8s %8s\n", "k", "|P_k|", "|W_k|", "|L_k|", "probes", "secs", "total_s");
  Table next, cur;
  u64 total_win = 0, total_loss = 0, reach_win = 0, reach_loss = 0;
  bool root_win = false;
  for (int k = F; k >= 0; --k) {
    const double t0 = Now();
    std::atomic<u64> probes{0};
    cur.Alloc(S * StrideWords(k));
    SolveLayer(k, next.d, cur.d, &probes);
    u64 win_raw, win_classes;
    CountTable(cur.d, k, &win_raw, &win_classes);
    const u64 p = C[N][Q] * C[N - Q][Q] * C[F][k];
    total_win += win_raw;
    total_loss += p - win_raw;
    if (samples && k < F) {
      std::atomic<u64> bad{0};
      ParallelFor(size_t(g_threads), [&](size_t th) {
        std::mt19937_64 rng(0xABCDEFULL * (u64(k) + 1) + th);
        std::vector<int> cells(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i) cells[size_t(i)] = i;
        for (u64 it = 0; it < samples / u64(g_threads) + 1; ++it) {
          std::shuffle(cells.begin(), cells.end(), rng);
          u64 m = 0, o = 0, a = 0;
          for (int i = 0; i < Q; ++i) m |= u64(1) << cells[size_t(i)];
          for (int i = Q; i < 2 * Q; ++i) o |= u64(1) << cells[size_t(i)];
          for (int i = 2 * Q; i < 2 * Q + k; ++i) a |= u64(1) << cells[size_t(i)];
          if (Lookup(cur.d, k, m, o, a) != ExplicitWin(m, o, a, k, next.d)) bad.fetch_add(1);
        }
      });
      if (bad.load()) Die("sample check failed at layer " + std::to_string(k));
    }
    cur.Save(dir + "/W" + std::to_string(k) + ".bits");
    std::printf(" %3d %18" PRIu64 " %18" PRIu64 " %18" PRIu64 " %14" PRIu64 " %8.1f %8.1f\n", k, p, win_raw, p - win_raw,
                probes.load(), Now() - t0, Now());
    if (reach) {
      Table r;
      if (!r.Map(dir + "/R" + std::to_string(k) + ".bits")) Die("missing reach table");
      // Reachable positions by value.
      const u64 stride = StrideWords(k);
      Table both;
      both.Alloc(S * stride);
      ParallelFor(size_t(g_threads), [&](size_t th) {
        const u64 b = both.words * th / u64(g_threads), e = both.words * (th + 1) / u64(g_threads);
        for (u64 i = b; i < e; ++i) both.d[i] = r.d[i] & cur.d[i];
      });
      u64 rw_raw, rw_classes;
      CountTable(both.d, k, &rw_raw, &rw_classes);
      reach_win += rw_raw;
      reach_loss += reach_raw[size_t(k)] - rw_raw;
      std::printf("#  reach k=%d raw=%" PRIu64 " win=%" PRIu64 " loss=%" PRIu64 " classes=%" PRIu64 " win_classes=%" PRIu64
                  " frac_of_P=%.4f\n",
                  k, reach_raw[size_t(k)], rw_raw, reach_raw[size_t(k)] - rw_raw, reach_classes[size_t(k)], rw_classes,
                  double(reach_raw[size_t(k)]) / double(p));
      ::munmap(r.mapped, r.words * 8);
    }
    std::fflush(stdout);
    if (k == 0 && std::min(W, H) >= 4 && Q == 4) {
      u64 ms, os;
      StartPosition(&ms, &os);
      root_win = Lookup(cur.d, 0, ms, os, 0);
      std::printf("# root %dx%d: %s\n", W, H, root_win ? "first player wins" : "second player wins");
      if (W != H) {
        StartPositionTransposed(&ms, &os);
        const bool other = Lookup(cur.d, 0, ms, os, 0);
        std::printf("# root %dx%d: %s\n", H, W, other ? "first player wins" : "second player wins");
      }
    }
    next = std::move(cur);
  }
  std::printf("# totals: win=%" PRIu64 " loss=%" PRIu64 "\n", total_win, total_loss);
  if (reach) std::printf("# reachable totals: win=%" PRIu64 " loss=%" PRIu64 "\n", reach_win, reach_loss);
  std::printf("# done %.1fs\n", Now());
  return 0;
}

// ---------------------------------------------------- minimal DFA measure

struct Node {
  u32 c[4];
};
inline bool operator==(const Node& a, const Node& b) { return std::memcmp(&a, &b, sizeof(Node)) == 0; }

struct SpinGuard {
  std::atomic_flag& f;
  explicit SpinGuard(std::atomic_flag& flag) : f(flag) {
    while (f.test_and_set(std::memory_order_acquire)) _mm_pause();
  }
  ~SpinGuard() { f.clear(std::memory_order_release); }
};

inline u64 NodeHash(const Node& n) {
  u64 a, b;
  std::memcpy(&a, &n.c[0], 8);
  std::memcpy(&b, &n.c[2], 8);
  u64 x = a * 0x9E3779B97F4A7C15ULL;
  x ^= x >> 29;
  x += b;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 32;
  x *= 0x94D049BB133111EBULL;
  return x ^ (x >> 29);
}

constexpr int kNodeShardBits = 12;

// Only the number of distinct nodes matters here, so IDs stay packed:
// (local + 1) << 12 | shard.
class LevelTable {
 public:
  struct Shard {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<Node> nodes;
    std::vector<u32> slots;
  };
  LevelTable() : shards_(new Shard[size_t(1) << kNodeShardBits]) {}
  u32 Intern(const Node& n, u64 h) {
    const size_t si = h >> (64 - kNodeShardBits);
    Shard& s = shards_[si];
    SpinGuard guard(s.lock);
    if ((s.nodes.size() + 1) * 3 > s.slots.size() * 2) {
      const size_t cap = s.slots.empty() ? 16 : s.slots.size() * 2;
      std::vector<u32> grown(cap, 0);
      for (u32 i = 0; i < s.nodes.size(); ++i) {
        size_t j = NodeHash(s.nodes[i]) & (cap - 1);
        while (grown[j]) j = (j + 1) & (cap - 1);
        grown[j] = i + 1;
      }
      s.slots.swap(grown);
    }
    const size_t mask = s.slots.size() - 1;
    size_t j = h & mask;
    while (const u32 v = s.slots[j]) {
      if (s.nodes[v - 1] == n) return (v << kNodeShardBits) | u32(si);
      j = (j + 1) & mask;
    }
    if (s.nodes.size() + 2 >= (size_t(1) << (32 - kNodeShardBits))) Die("DFA level too wide");
    s.nodes.push_back(n);
    s.slots[j] = u32(s.nodes.size());
    return (u32(s.nodes.size()) << kNodeShardBits) | u32(si);
  }
  u64 Count() const {
    u64 t = 0;
    for (size_t i = 0; i < (size_t(1) << kNodeShardBits); ++i) t += shards_[i].nodes.size();
    return t;
  }
  void Free() { shards_.reset(); }

 private:
  std::unique_ptr<Shard[]> shards_;
};

std::vector<int> MakeOrder(const std::string& name) {
  std::vector<int> o;
  auto cell = [&](int x, int y) { return y * W + x; };
  if (name == "row") {
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) o.push_back(cell(x, y));
  } else if (name == "col") {
    for (int x = 0; x < W; ++x)
      for (int y = 0; y < H; ++y) o.push_back(cell(x, y));
  } else if (name == "snake") {
    for (int y = 0; y < H; ++y)
      for (int i = 0; i < W; ++i) o.push_back(cell(y % 2 ? W - 1 - i : i, y));
  } else if (name == "diag") {
    for (int d = 0; d < W + H - 1; ++d)
      for (int x = 0; x < W; ++x)
        if (d - x >= 0 && d - x < H) o.push_back(cell(x, d - x));
  } else if (name == "rowsoutin") {
    for (int i = 0; i < H; ++i) {
      const int y = (i % 2 == 0) ? i / 2 : H - 1 - i / 2;
      for (int x = 0; x < W; ++x) o.push_back(cell(x, y));
    }
  } else if (name == "spiralin" || name == "spiralout") {
    int x0 = 0, x1 = W - 1, y0 = 0, y1 = H - 1;
    while (x0 <= x1 && y0 <= y1) {
      for (int x = x0; x <= x1; ++x) o.push_back(cell(x, y0));
      for (int y = y0 + 1; y <= y1; ++y) o.push_back(cell(x1, y));
      if (y1 > y0)
        for (int x = x1 - 1; x >= x0; --x) o.push_back(cell(x, y1));
      if (x1 > x0)
        for (int y = y1 - 1; y > y0; --y) o.push_back(cell(x0, y));
      ++x0, --x1, ++y0, --y1;
    }
    if (name == "spiralout") std::reverse(o.begin(), o.end());
  } else {
    Die("unknown order " + name);
  }
  return o;
}

enum SetKind { kSetW, kSetL, kSetR, kSetRW, kSetRL, kSetWN };

struct DfaBuild {
  int k;
  SetKind kind;
  const u64* tw = nullptr;
  const u64* tr = nullptr;
  u64 stride = 0;
  std::vector<int> order;
  std::vector<LevelTable> levels;
  explicit DfaBuild(int n) : levels(static_cast<size_t>(n)) {}
};

// Per-thread direct-mapped cache in front of the shared level tables.
struct CacheEntry {
  Node n;
  u32 id;
};
constexpr size_t kCacheSize = size_t(1) << 15;

struct Worker {
  DfaBuild* b;
  std::vector<std::unique_ptr<CacheEntry[]>> cache;
  explicit Worker(DfaBuild* bb) : b(bb) {
    cache.resize(size_t(N));
    for (auto& c : cache) {
      c.reset(new CacheEntry[kCacheSize]);
      std::memset(c.get(), 0xFF, kCacheSize * sizeof(CacheEntry));
    }
  }
  inline u32 Intern(int level, const Node& n) {
    const u64 h = NodeHash(n);
    CacheEntry& e = cache[size_t(level)][h & (kCacheSize - 1)];
    if (e.n == n) return e.id;
    const u32 id = b->levels[size_t(level)].Intern(n, h);
    e.n = n;
    e.id = id;
    return id;
  }
  inline bool Leaf(u64 m, u64 o, u64 a) const {
    const u32 ce = g_canon[RawIndex(m, o)];
    const u64 id = ce >> 3;
    const int g = int(ce & 7);
    const u64 free = ~(g_slabs[id].m | g_slabs[id].o) & BOARD;
    const u32 r = g_arank[_pext_u64(Transform(g, a), free)];
    switch (b->kind) {
      case kSetW:
        return ReadBit(b->tw, b->stride, id, r);
      case kSetL:
        return !ReadBit(b->tw, b->stride, id, r);
      case kSetR:
        return ReadBit(b->tr, b->stride, id, r);
      case kSetRW:
        return ReadBit(b->tr, b->stride, id, r) && ReadBit(b->tw, b->stride, id, r);
      case kSetWN:
        return ReadBit(b->tw, b->stride, id, r) && DeadCells(m | o, a) == 0;
      default:
        return ReadBit(b->tr, b->stride, id, r) && !ReadBit(b->tw, b->stride, id, r);
    }
  }
  // Fast path once every queen is placed: one slab, arrows only.
  u32 BuildArrows(int i, int al, u64 ga, const u64* slab_w, const u64* slab_r, int g, u64 free) {
    if (i == N) {
      const u32 r = g_arank[_pext_u64(ga, free)];
      bool in;
      switch (b->kind) {
        case kSetW:
          in = (slab_w[r >> 6] >> (r & 63)) & 1;
          break;
        case kSetL:
          in = !((slab_w[r >> 6] >> (r & 63)) & 1);
          break;
        case kSetR:
          in = (slab_r[r >> 6] >> (r & 63)) & 1;
          break;
        case kSetRW:
          in = ((slab_r[r >> 6] >> (r & 63)) & 1) && ((slab_w[r >> 6] >> (r & 63)) & 1);
          break;
        case kSetWN:
          in = ((slab_w[r >> 6] >> (r & 63)) & 1) && DeadCells(BOARD & ~free, ga) == 0;
          break;
        default:
          in = ((slab_r[r >> 6] >> (r & 63)) & 1) && !((slab_w[r >> 6] >> (r & 63)) & 1);
      }
      return in ? 1 : 0;
    }
    const int cell = b->order[size_t(i)];
    Node n{};
    bool any = false;
    if (al <= N - i - 1) {
      n.c[0] = BuildArrows(i + 1, al, ga, slab_w, slab_r, g, free);
      any = any || n.c[0];
    }
    if (al > 0) {
      n.c[3] = BuildArrows(i + 1, al - 1, ga | GBIT[g][cell], slab_w, slab_r, g, free);
      any = any || n.c[3];
    }
    return any ? Intern(i, n) : 0;
  }
  u32 Build(int i, int ml, int ol, int al, u64 m, u64 o, u64 a) {
    if (ml + ol + al > N - i) return 0;
    if (i == N) return Leaf(m, o, a) ? 1 : 0;
    if (ml == 0 && ol == 0) {
      const u32 ce = g_canon[RawIndex(m, o)];
      const u64 id = ce >> 3;
      const int g = int(ce & 7);
      const u64 free = ~(g_slabs[id].m | g_slabs[id].o) & BOARD;
      const u64* sw = b->tw ? b->tw + id * b->stride : nullptr;
      const u64* sr = b->tr ? b->tr + id * b->stride : nullptr;
      return BuildArrows(i, al, Transform(g, a), sw, sr, g, free);
    }
    const u64 bit = u64(1) << b->order[size_t(i)];
    Node n{};
    bool any = false;
    n.c[0] = Build(i + 1, ml, ol, al, m, o, a);
    any = any || n.c[0];
    if (ml) {
      n.c[1] = Build(i + 1, ml - 1, ol, al, m | bit, o, a);
      any = any || n.c[1];
    }
    if (ol) {
      n.c[2] = Build(i + 1, ml, ol - 1, al, m, o | bit, a);
      any = any || n.c[2];
    }
    if (al) {
      n.c[3] = Build(i + 1, ml, ol, al - 1, m, o, a | bit);
      any = any || n.c[3];
    }
    return any ? Intern(i, n) : 0;
  }
};

int CmdDfa(const std::string& dir, int k, const std::string& set_name, const std::string& order_name) {
  BuildSlabs();
  BuildArrowRanks();
  DfaBuild b(N);
  b.k = k;
  b.stride = StrideWords(k);
  b.order = MakeOrder(order_name);
  if (set_name == "W") b.kind = kSetW;
  else if (set_name == "L") b.kind = kSetL;
  else if (set_name == "R") b.kind = kSetR;
  else if (set_name == "RW") b.kind = kSetRW;
  else if (set_name == "RL") b.kind = kSetRL;
  else if (set_name == "WN") b.kind = kSetWN;
  else Die("unknown set " + set_name);
  Table tw, tr;
  if (b.kind != kSetR) {
    if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
    b.tw = tw.d;
  }
  if (b.kind == kSetR || b.kind == kSetRW || b.kind == kSetRL) {
    if (!tr.Map(dir + "/R" + std::to_string(k) + ".bits")) Die("missing R table");
    b.tr = tr.d;
  }
  // Split on every viable prefix of 6 cells.
  const int split = std::min(N - 1, 6);
  size_t tasks = size_t(1) << (2 * split);
  std::vector<u32> results(tasks, 0);
  const double t0 = Now();
  std::atomic<u64> done{0};
  ParallelFor(tasks, [&](size_t task) {
    int ml = Q, ol = Q, al = k;
    u64 m = 0, o = 0, a = 0;
    for (int i = 0; i < split; ++i) {
      const int sym = int((task >> (2 * (split - 1 - i))) & 3);
      const u64 bit = u64(1) << b.order[size_t(i)];
      if (sym == 1) --ml, m |= bit;
      if (sym == 2) --ol, o |= bit;
      if (sym == 3) --al, a |= bit;
    }
    if (ml < 0 || ol < 0 || al < 0) return;
    static thread_local std::unique_ptr<Worker> worker;
    if (!worker || worker->b != &b) worker = std::make_unique<Worker>(&b);
    results[task] = worker->Build(split, ml, ol, al, m, o, a);
    done.fetch_add(1);
  });
  // Upper levels, serially.
  Worker top(&b);
  std::function<u32(int, size_t, int, int, int)> up = [&](int i, size_t code, int ml, int ol, int al) -> u32 {
    if (ml < 0 || ol < 0 || al < 0) return 0;
    if (i == split) return results[code];
    Node n{};
    bool any = false;
    for (int c = 0; c < 4; ++c) {
      n.c[c] = up(i + 1, (code << 2) | size_t(c), ml - (c == 1), ol - (c == 2), al - (c == 3));
      any = any || n.c[c];
    }
    return any ? top.Intern(i, n) : 0;
  };
  const u32 root = up(0, 0, Q, Q, k);
  u64 total = 0, widest = 0;
  std::string widths;
  for (int i = 0; i < N; ++i) {
    const u64 c = b.levels[size_t(i)].Count();
    total += c;
    widest = std::max(widest, c);
    widths += (i ? " " : "") + std::to_string(c);
  }
  const u64 p = C[N][Q] * C[N - Q][Q] * C[F][k];
  std::printf("dfa %dx%d k=%d set=%s order=%s |P_k|=%" PRIu64 " nodes=%" PRIu64 " width=%" PRIu64
              " positions/node=%.1f root=%s secs=%.1f\n",
              W, H, k, set_name.c_str(), order_name.c_str(), p, total, widest, total ? double(p) / double(total) : 0.0,
              root ? "nonempty" : "empty", Now() - t0);
  std::printf("  widths: %s\n", widths.c_str());
  std::fflush(stdout);
  return 0;
}

inline double H2(double p) {
  if (p <= 0 || p >= 1) return 0;
  return -(p * std::log2(p) + (1 - p) * std::log2(1 - p));
}

// How far could the 1-bit tables be squeezed?  All sizes are entropy bounds
// over canonical slabs, excluding per-slab headers.
int CmdStats(const std::string& dir) {
  BuildSlabs();
  BuildArrowRanks();
  const u64 S = g_slabs.size();
  std::printf("#%3s %14s %6s %7s %7s %7s %7s %7s | %9s %9s %9s %9s\n", "k", "canon_bits", "W", "H0", "Hslab", "Hword", "const%",
              "reach%", "raw_GiB", "slab_GiB", "word_GiB", "Rsl_GiB");
  double sum_raw = 0, sum_slab = 0, sum_word = 0, sum_reach = 0;
  const double gib = 8.0 * (1ULL << 30);
  for (int k = 0; k <= F; ++k) {
    Table tw, tr;
    if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
    const bool have_r = tr.Map(dir + "/R" + std::to_string(k) + ".bits");
    const u64 stride = StrideWords(k);
    const u64 bits = C[F][k];
    std::atomic<u64> total_ones{0}, const_slabs{0}, total_reach{0};
    std::mutex mu;
    double slab_bits = 0, word_bits = 0, reach_bits = 0;
    const size_t chunk = 1 << 12;
    ParallelFor((S + chunk - 1) / chunk, [&](size_t task) {
      u64 ones_acc = 0, const_acc = 0, reach_acc = 0;
      double sb = 0, wb = 0, rb = 0;
      const size_t b = task * chunk, e = std::min<size_t>(S, b + chunk);
      for (size_t id = b; id < e; ++id) {
        const u64* w = tw.d + id * stride;
        u64 ones = 0;
        for (u64 i = 0; i < stride; ++i) {
          const int pc = __builtin_popcountll(w[i]);
          ones += u64(pc);
          const u64 block = (i + 1 == stride && (bits % 64)) ? bits % 64 : 64;
          wb += double(block) * H2(double(pc) / double(block));
        }
        ones_acc += ones;
        const_acc += (ones == 0 || ones == bits);
        sb += double(bits) * H2(double(ones) / double(bits));
        if (have_r) {
          const u64* r = tr.d + id * stride;
          u64 rc = 0, wr = 0;
          for (u64 i = 0; i < stride; ++i) {
            rc += u64(__builtin_popcountll(r[i]));
            wr += u64(__builtin_popcountll(r[i] & w[i]));
          }
          reach_acc += rc;
          rb += double(bits) * H2(double(rc) / double(bits));
          if (rc) rb += double(rc) * H2(double(wr) / double(rc));
        }
      }
      total_ones.fetch_add(ones_acc);
      const_slabs.fetch_add(const_acc);
      total_reach.fetch_add(reach_acc);
      std::lock_guard<std::mutex> lock(mu);
      slab_bits += sb;
      word_bits += wb;
      reach_bits += rb;
    });
    const double all = double(S) * double(bits);
    const double p = double(total_ones.load()) / all;
    std::printf(" %3d %14.0f %6.3f %7.3f %7.3f %7.3f %7.2f %7.2f | %9.3f %9.3f %9.3f %9.3f\n", k, all, p, H2(p), slab_bits / all,
                word_bits / all, 100.0 * double(const_slabs.load()) / double(S), 100.0 * double(total_reach.load()) / all,
                all / gib, slab_bits / gib, word_bits / gib, reach_bits / gib);
    std::fflush(stdout);
    sum_raw += all / gib;
    sum_slab += slab_bits / gib;
    sum_word += word_bits / gib;
    sum_reach += reach_bits / gib;
    ::munmap(tw.mapped, tw.words * 8);
    if (have_r) ::munmap(tr.mapped, tr.words * 8);
  }
  std::printf("# total GiB: raw=%.2f slab-entropy=%.2f word-entropy=%.2f reachable-only(slab)=%.2f\n", sum_raw, sum_slab, sum_word,
              sum_reach);
  return 0;
}

// A hash-consed forest over one layer: each slab's bit vector is split on its
// highest free cell (which is how colex ranks nest), down to 64-bit bitmap
// leaves, sharing equal subtrees across all slabs.  Counts distinct internal
// nodes and leaves by 64-bit fingerprint, so no node has to be stored.
class FingerprintSet {
 public:
  FingerprintSet() : shards_(new Shard[kShards]) {}
  void Insert(u64 fp) {
    Shard& s = shards_[fp >> 52];
    SpinGuard guard(s.lock);
    if ((s.count + 1) * 3 > s.slots.size() * 2) {
      std::vector<u64> grown(s.slots.empty() ? 64 : s.slots.size() * 2, 0);
      const size_t mask = grown.size() - 1;
      for (u64 v : s.slots) {
        if (!v) continue;
        size_t j = v & mask;
        while (grown[j]) j = (j + 1) & mask;
        grown[j] = v;
      }
      s.slots.swap(grown);
    }
    const u64 v = fp | 1;
    const size_t mask = s.slots.size() - 1;
    size_t j = v & mask;
    while (s.slots[j]) {
      if (s.slots[j] == v) return;
      j = (j + 1) & mask;
    }
    s.slots[j] = v;
    ++s.count;
  }
  u64 Count() const {
    u64 t = 0;
    for (size_t i = 0; i < kShards; ++i) t += shards_[i].count;
    return t;
  }

 private:
  static constexpr size_t kShards = 4096;
  struct Shard {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<u64> slots;
    u64 count = 0;
  };
  std::unique_ptr<Shard[]> shards_;
};

inline u64 MixFp(u64 a, u64 b) {
  u64 x = a * 0x9E3779B97F4A7C15ULL + b;
  x ^= x >> 29;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 32;
  x *= 0x94D049BB133111EBULL;
  return x ^ (x >> 29);
}

inline u64 ExtractBits(const u64* w, u64 offset, u64 len) {
  const u64 word = offset >> 6, bit = offset & 63;
  u64 v = w[word] >> bit;
  if (bit && bit + len > 64) v |= w[word + 1] << (64 - bit);
  return len == 64 ? v : v & ((u64(1) << len) - 1);
}

u64 ForestVisit(const u64* w, int f, int k, u64 offset, FingerprintSet& inner, FingerprintSet& leaves) {
  const u64 len = C[f][k];
  if (len <= 64) {
    const u64 fp = MixFp(ExtractBits(w, offset, len), len);
    leaves.Insert(fp);
    return fp;
  }
  const u64 low = C[f - 1][k];
  const u64 a = low ? ForestVisit(w, f - 1, k, offset, inner, leaves) : 0;
  const u64 b = k ? ForestVisit(w, f - 1, k - 1, offset + low, inner, leaves) : 0;
  const u64 fp = MixFp(MixFp(a, b), (u64(f) << 8) | u64(k));
  inner.Insert(fp);
  return fp;
}

int CmdForest(const std::string& dir, int only_k) {
  BuildSlabs();
  BuildArrowRanks();
  const u64 S = g_slabs.size();
  std::printf("#%3s %14s %12s %12s %10s %10s %9s\n", "k", "canon_bits", "inner", "leaves", "raw_MiB", "forest_MiB", "ratio");
  for (int k = 0; k <= F; ++k) {
    if (only_k >= 0 && k != only_k) continue;
    Table tw;
    if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
    const u64 stride = StrideWords(k);
    FingerprintSet inner, leaves;
    const size_t chunk = 256;
    ParallelFor((S + chunk - 1) / chunk, [&](size_t task) {
      const size_t b = task * chunk, e = std::min<size_t>(S, b + chunk);
      for (size_t id = b; id < e; ++id) (void)ForestVisit(tw.d + id * stride, F, k, 0, inner, leaves);
    });
    const double raw = double(S) * double(C[F][k]) / 8 / (1 << 20);
    // 2 x 5-byte child references per internal node, 8 bytes per leaf.
    const double est = (double(inner.Count()) * 10 + double(leaves.Count()) * 8) / (1 << 20);
    std::printf(" %3d %14.0f %12" PRIu64 " %12" PRIu64 " %10.1f %10.1f %9.2f\n", k, double(S) * double(C[F][k]), inner.Count(),
                leaves.Count(), raw, est, raw / est);
    std::fflush(stdout);
    ::munmap(tw.mapped, tw.words * 8);
  }
  return 0;
}

// Reports the share of positions with a dead cell.  With OUT, also writes
// W<k>.nodead: the table with those positions' bits cleared, to see what a
// compressor gains once they no longer need storing.
int CmdDead(const std::string& dir, const std::string& out_dir) {
  BuildSlabs();
  BuildArrowRanks();
  if (!out_dir.empty()) ::mkdir(out_dir.c_str(), 0775);
  const u64 S = g_slabs.size();
  std::printf("#%3s %14s %14s %8s\n", "k", "canon_bits", "with_dead", "share%");
  double sum_all = 0, sum_dead = 0;
  for (int k = 0; k <= F; ++k) {
    Table tw;
    if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
    const u64 stride = StrideWords(k);
    const u64 bits = C[F][k];
    std::atomic<u64> dead_total{0};
    Table filtered;
    if (!out_dir.empty()) filtered.Alloc(tw.words);
    const size_t chunk = 256;
    ParallelFor((S + chunk - 1) / chunk, [&](size_t task) {
      u64 acc = 0;
      const size_t b = task * chunk, e = std::min<size_t>(S, b + chunk);
      for (size_t id = b; id < e; ++id) {
        const u64 queens = g_slabs[id].m | g_slabs[id].o;
        const u64 free = ~queens & BOARD;
        u64 v = k ? (u64(1) << k) - 1 : 0;
        for (u64 r = 0; r < bits; ++r) {
          const u64 a = _pdep_u64(v, free);
          const bool has_dead = DeadCells(queens, a) != 0;
          acc += has_dead;
          if (!out_dir.empty() && !has_dead && ((tw.d[id * stride + (r >> 6)] >> (r & 63)) & 1)) {
            filtered.d[id * stride + (r >> 6)] |= u64(1) << (r & 63);
          }
          if (r + 1 < bits) v = NextSubset(v);
        }
      }
      dead_total.fetch_add(acc);
    });
    const double all = double(S) * double(bits);
    std::printf(" %3d %14.0f %14" PRIu64 " %8.2f\n", k, all, dead_total.load(), 100.0 * double(dead_total.load()) / all);
    std::fflush(stdout);
    sum_all += all;
    sum_dead += double(dead_total.load());
    if (!out_dir.empty()) filtered.Save(out_dir + "/W" + std::to_string(k) + ".nodead");
    ::munmap(tw.mapped, tw.words * 8);
  }
  std::printf("# overall share with a dead cell: %.2f%%\n", 100.0 * sum_dead / sum_all);
  return 0;
}

// Writes one layer with slabs sorted by opponent set first.  The out-of-core
// design alternates mover-major and opponent-major layers, so this measures
// whether the layout changes how well a layer compresses.
int CmdReorder(const std::string& dir, int k, const std::string& out_path, const std::string& bit_order) {
  BuildSlabs();
  BuildArrowRanks();
  Table tw;
  if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
  const u64 stride = StrideWords(k);
  if (!bit_order.empty()) {
    // Same slab order, but arrow bits renumbered: within a slab, the free cells
    // are sorted by the given key and the most significant cell comes last.
    auto key = [&](int c) -> double {
      const double x = c % W - (W - 1) / 2.0, y = c / W - (H - 1) / 2.0;
      if (bit_order == "rev") return -double(c);
      if (bit_order == "center") return -(x * x + y * y) + 1e-3 * c;  // centre most significant
      if (bit_order == "outer") return (x * x + y * y) + 1e-3 * c;    // rim most significant
      if (bit_order == "col") return double(c % W) * 100 + c / W;
      Die("unknown bit order " + bit_order);
    };
    Table out;
    out.Alloc(tw.words);
    const u64 count = C[F][k];
    const size_t chunk = 64;
    ParallelFor((g_slabs.size() + chunk - 1) / chunk, [&](size_t task) {
      const size_t b = task * chunk, e = std::min(g_slabs.size(), b + chunk);
      std::vector<int> cells;
      std::vector<int> new_pos(static_cast<size_t>(F));
      for (size_t id = b; id < e; ++id) {
        const u64 free = ~(g_slabs[id].m | g_slabs[id].o) & BOARD;
        cells.clear();
        for (u64 f2 = free; f2; f2 &= f2 - 1) cells.push_back(__builtin_ctzll(f2));
        std::vector<int> sorted(cells);
        std::sort(sorted.begin(), sorted.end(), [&](int x, int y) { return key(x) < key(y); });
        for (int i = 0; i < F; ++i) new_pos[size_t(i)] = int(std::find(sorted.begin(), sorted.end(), cells[size_t(i)]) - sorted.begin());
        const u64* w = tw.d + id * stride;
        u64* o = out.d + id * stride;
        for (u64 r = 0; r < count; ++r) {
          if (!((w[r >> 6] >> (r & 63)) & 1)) continue;
          u64 v = g_aunrank[size_t(k)][r], mapped = 0;
          for (; v; v &= v - 1) mapped |= u64(1) << new_pos[size_t(__builtin_ctzll(v))];
          const u32 r2 = g_arank[mapped];
          o[r2 >> 6] |= u64(1) << (r2 & 63);
        }
      }
    });
    out.Save(out_path);
    return 0;
  }
  std::vector<u32> ids(g_slabs.size());
  for (u32 i = 0; i < ids.size(); ++i) ids[i] = i;
  std::sort(ids.begin(), ids.end(), [](u32 a, u32 b) {
    if (g_slabs[a].o != g_slabs[b].o) return g_slabs[a].o < g_slabs[b].o;
    return g_slabs[a].m < g_slabs[b].m;
  });
  Table out;
  out.Alloc(tw.words);
  ParallelFor(ids.size(), [&](size_t i) { std::memcpy(out.d + i * stride, tw.d + ids[i] * stride, stride * 8); });
  out.Save(out_path);
  return 0;
}

// Sound static bounds.  Split the board into 8-connected areas of non-arrow
// cells.  No queen can leave its area, and each move removes exactly one empty
// cell of its area, so an area's empties bound the moves anyone can make there.
// In an area holding queens of only one color the owner can always "plod":
// step to an adjacent empty cell and shoot back at the vacated one.  A greedy
// sequence of such moves is a guaranteed lower bound, whatever the opponent does.
inline u64 PlodMoves(u64 queens, u64 empties) {
  u64 total = 0;
  while (queens) {
    int c = __builtin_ctzll(queens);
    queens &= queens - 1;
    for (;;) {
      const u64 near = Expand8(u64(1) << c) & empties;
      if (!near) break;
      // Warnsdorff's rule: go where the fewest onward steps remain.
      int best = -1, best_degree = 99;
      for (u64 n = near; n; n &= n - 1) {
        const int t = __builtin_ctzll(n);
        const int degree = __builtin_popcountll(Expand8(u64(1) << t) & empties & ~(u64(1) << t));
        if (degree < best_degree) best_degree = degree, best = t;
      }
      empties &= ~(u64(1) << best);
      c = best;
      ++total;
    }
  }
  return total;
}

// +1: the mover surely wins; -1: the mover surely loses; 0: unknown.
inline int StaticBound(u64 m, u64 o, u64 a) {
  const u64 open = BOARD & ~a;
  const u64 empties = open & ~(m | o);
  u64 todo = open;
  u64 m_lo = 0, o_lo = 0, m_hi = 0, o_hi = 0;
  while (todo) {
    u64 area = todo & -todo;
    for (;;) {
      const u64 next = (area | Expand8(area)) & open;
      if (next == area) break;
      area = next;
    }
    todo &= ~area;
    const u64 am = area & m, ao = area & o, ae = area & empties;
    const u64 e = u64(__builtin_popcountll(ae));
    if (am && ao) {
      m_hi += e;
      o_hi += e;
    } else if (am) {
      m_hi += e;
      m_lo += PlodMoves(am, ae);
    } else if (ao) {
      o_hi += e;
      o_lo += PlodMoves(ao, ae);
    }
  }
  if (m_lo > o_hi) return 1;
  if (o_lo >= m_hi) return -1;
  return 0;
}

// Size of a weak-solution proof from the standard start, using the tables as a
// perfect oracle: at the loser's turn all moves are included, at the winner's
// turn one winning move, preferring a child already in the proof and otherwise
// the child that leaves the opponent the fewest moves.  The result is an upper
// bound on the smallest proof, and what any weak solver has to get through
// unless static bounds cut branches off earlier.
inline u64 CountMoves(u64 mover, u64 occ) {
  u64 total = 0;
  while (mover) {
    const int f = __builtin_ctzll(mover);
    mover &= mover - 1;
    const u64 occ2 = occ & ~(u64(1) << f);
    u64 ts = Attacks(f, occ);
    while (ts) {
      const int t = __builtin_ctzll(ts);
      ts &= ts - 1;
      total += u64(__builtin_popcountll(Attacks(t, occ2)));
    }
  }
  return total;
}

inline void MarkChild(const PairInfo& p, u64 a2, u64* next, u64 next_stride) {
  const u8 stab = g_stab[p.child_id];
  u64* cw = next + p.child_id * next_stride;
  for (int g2 = 0; g2 < GN; ++g2) {
    if (!((stab >> g2) & 1)) continue;
    const u64 a3 = g2 ? Transform(g2, a2) : a2;
    const u32 r2 = g_arank[_pext_u64(a3, p.child_free)];
    const u64 mask = u64(1) << (r2 & 63);
    std::atomic_ref<u64> cell(cw[r2 >> 6]);
    if (!(cell.load(std::memory_order_relaxed) & mask)) cell.fetch_or(mask, std::memory_order_relaxed);
  }
}

int CmdProof(const std::string& dir, bool transposed, bool use_static) {
  BuildSlabs();
  BuildArrowRanks();
  const u64 S = g_slabs.size();
  u64 ms, os;
  if (transposed) {
    StartPositionTransposed(&ms, &os);
  } else {
    StartPosition(&ms, &os);
  }
  Table cur;
  cur.Alloc(S * StrideWords(0));
  cur.d[(g_canon[RawIndex(ms, os)] >> 3) * StrideWords(0)] |= 1;
  u64 total_classes = 0, total_raw = 0;
  std::printf("# proof from the %s start, static bounds %s\n", transposed ? "transposed" : "standard", use_static ? "on" : "off");
  std::printf("#%3s %16s %16s %14s\n", "k", "raw", "classes", "of_P");
  std::atomic<u64> static_leaves{0};
  for (int k = 0; k <= F; ++k) {
    u64 raw = 0, classes = 0;
    CountTable(cur.d, k, &raw, &classes);
    total_raw += raw;
    total_classes += classes;
    const double p = double(C[N][Q]) * double(C[N - Q][Q]) * double(C[F][k]);
    std::printf(" %3d %16" PRIu64 " %16" PRIu64 " %14.3e\n", k, raw, classes, double(raw) / p);
    std::fflush(stdout);
    if (k == F || raw == 0) break;
    Table tw, tn;
    if (!tw.Map(dir + "/W" + std::to_string(k) + ".bits")) Die("missing W table");
    if (!tn.Map(dir + "/W" + std::to_string(k + 1) + ".bits")) Die("missing W table");
    const u64 stride = StrideWords(k), next_stride = StrideWords(k + 1);
    Table next;
    next.Alloc(S * next_stride);
    const size_t chunk = 256;
    ParallelFor((S + chunk - 1) / chunk, [&](size_t task) {
      static thread_local PairInfo pinfo[8][64];
      int queen_cell[8];
      const size_t b = task * chunk, e = std::min<size_t>(S, b + chunk);
      for (size_t id = b; id < e; ++id) {
        const u64* w = cur.d + id * stride;
        bool any = false;
        for (u64 i = 0; i < stride; ++i) any = any || w[i];
        if (!any) continue;
        const Slab& s = g_slabs[id];
        FillPairs(s, tn.d, next_stride, pinfo, queen_cell);
        const u64 occq = s.m | s.o;
        const u64 free = ~occq & BOARD;
        for (u64 i = 0; i < stride; ++i) {
          u64 word = w[i];
          while (word) {
            const u64 r = i * 64 + u64(__builtin_ctzll(word));
            word &= word - 1;
            const bool winner_moves = ReadBit(tw.d, stride, id, u32(r));
            const u64 a = _pdep_u64(g_aunrank[size_t(k)][r], free);
            const u64 occ = occq | a;
            if (use_static) {
              const int verdict = StaticBound(s.m, s.o, a);
              if (verdict != 0) {
                if ((verdict > 0) != winner_moves) Die("static bound contradicts the table");
                static_leaves.fetch_add(1, std::memory_order_relaxed);
                continue;
              }
            }
            u64 best_score = ~u64(0);
            const PairInfo* best_p = nullptr;
            u64 best_a2 = 0;
            bool done = false;
            for (int qi = 0; qi < Q && !done; ++qi) {
              const int f = queen_cell[qi];
              const u64 occ2 = occ & ~(u64(1) << f);
              u64 ts = Attacks(f, occ);
              while (ts && !done) {
                const int t = __builtin_ctzll(ts);
                ts &= ts - 1;
                const PairInfo& p = pinfo[qi][t];
                const u64 ga = Transform(p.g, a);
                u64 xs = Attacks(t, occ2);
                while (xs) {
                  const int x = __builtin_ctzll(xs);
                  xs &= xs - 1;
                  const u64 a2 = ga | GBIT[p.g][x];
                  if (!winner_moves) {
                    MarkChild(p, a2, next.d, next_stride);
                    continue;
                  }
                  const u32 r2 = g_arank[_pext_u64(a2, p.child_free)];
                  if ((p.base[r2 >> 6] >> (r2 & 63)) & 1) continue;  // not a winning move
                  const u64 present =
                      std::atomic_ref<u64>(next.d[p.child_id * next_stride + (r2 >> 6)]).load(std::memory_order_relaxed);
                  if ((present >> (r2 & 63)) & 1) {  // already in the proof: free
                    best_p = nullptr;
                    done = true;
                    break;
                  }
                  const u64 child_occ = occ2 | (u64(1) << t) | (u64(1) << x);
                  const u64 score = CountMoves(s.o, child_occ);
                  if (score < best_score) {
                    best_score = score;
                    best_p = &p;
                    best_a2 = a2;
                  }
                }
              }
            }
            if (winner_moves && !done) {
              if (!best_p) Die("winning position without a winning move: tables are inconsistent");
              MarkChild(*best_p, best_a2, next.d, next_stride);
            }
          }
        }
      }
    });
    ::munmap(tw.mapped, tw.words * 8);
    ::munmap(tn.mapped, tn.words * 8);
    cur = std::move(next);
  }
  std::printf("# proof total: raw=%" PRIu64 " classes=%" PRIu64 " static_leaves=%" PRIu64 " (%.1fs)\n", total_raw, total_classes,
              static_leaves.load(), Now());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  g_t0 = Clock::now();
  g_threads = int(std::thread::hardware_concurrency());
  if (argc < 4) {
    std::puts(
        "usage: dense solve WxH DIR [--threads T] [--queens Q] [--reach] [--verify-samples N]\n"
        "       dense dfa WxH DIR --k K [--set W|L|R|RW|RL|WN] [--order NAME] [--threads T] [--queens Q]\n"
        "       dense stats WxH DIR [--threads T] [--queens Q]\n"
        "       dense forest WxH DIR [--k K] [--threads T] [--queens Q]\n"
        "       dense dead WxH DIR [--set OUTDIR] [--threads T] [--queens Q]\n"
        "       dense reorder WxH DIR --k K --set OUTFILE [--order rev|center|outer|col] [--threads T] [--queens Q]\n"
        "       dense proof WxH DIR [--set T|S|TS] [--threads T] [--queens Q]");
    return 2;
  }
  const std::string cmd = argv[1];
  int w = 0, h = 0, q = 4, k = -1;
  if (std::sscanf(argv[2], "%dx%d", &w, &h) != 2) Die("bad board");
  const std::string dir = argv[3];
  bool reach = false;
  u64 samples = 0;
  std::string set = "W", order = "row";
  for (int i = 4; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) Die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--threads") g_threads = std::atoi(next());
    else if (a == "--queens") q = std::atoi(next());
    else if (a == "--reach") reach = true;
    else if (a == "--verify-samples") samples = u64(std::atoll(next()));
    else if (a == "--k") k = std::atoi(next());
    else if (a == "--set") set = next();
    else if (a == "--order") order = next();
    else Die("unknown option " + a);
  }
  InitBoard(w, h, q);
  if (cmd == "solve") return CmdSolve(dir, reach, samples);
  if (cmd == "stats") return CmdStats(dir);
  if (cmd == "forest") return CmdForest(dir, k);
  if (cmd == "proof") return CmdProof(dir, set.find('T') != std::string::npos, set.find('S') != std::string::npos);
  if (cmd == "reorder") return CmdReorder(dir, k, set, order == "row" ? std::string() : order);
  if (cmd == "dead") return CmdDead(dir, set == "W" ? std::string() : set);
  if (cmd == "dfa") {
    if (k < 0) Die("--k required");
    return CmdDfa(dir, k, set, order);
  }
  Die("unknown command " + cmd);
}
