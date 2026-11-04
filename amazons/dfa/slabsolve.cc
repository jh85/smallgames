/*
  slabsolve — bit-parallel, out-of-core strong solver for small Amazons boards.

  `dense` decides one position at a time, walking every move until it finds a
  losing child.  This tool decides every arrow subset of one queen placement at
  once.  With the queens fixed, a move is legal exactly when no arrow lies on its
  path, so "this move wins" is a mask and a shift over a bit vector indexed by
  the arrow subset.  Those vectors are held as 512-bit words.

  The model.  A position is (X, Y, A): mover queens, opponent queens, arrows.  With
  F = cells \ (X | Y), the arrow set is indexed by pext(A, F): the rank of each free
  cell is its index bit.  The low 9 index bits select a bit inside a word, and the
  high index bits select the word.  A layer holds one fixed popcount, so of the 2^|F|
  bits only those of popcount k are live, and the others stay zero.

  A move f->t followed by a shot at s reaches (Y, X', A+s), where X' = X-f+t.  So all
  the children of the parents with opponent set Y lie in the row with mover set Y.
  One row is processed at a time, in two stages:

    Stage 1, once per child slab (Y, X') and per queen t of X':
        S_t(A') = exists s on a ray from t, path clear of A',
                  with L(Y, X', A'+s)
    Stage 2, once per parent slab (X, Y):
        W(A) = exists f in X and t on a ray from f, path clear of A,
               with S_t(A'), where A' is A in the child's index

  S_t does not depend on which queen moved to t, so parents share it.  A in the
  child's index is A with the bit of t removed and a zero inserted for f, which only
  moves the index bits between those two ranks.  Rotations within the high index bits
  renumber words; those that touch the low 9 bits become byte permutes and GF2 affine
  bit permutes (see LowPerm).

  Symmetry.  Only rows whose queen set is canonical are computed, each over all of its
  opponent placements.  Output comes out as columns (all X for a canonical Y) and is
  needed as rows (all X' for a canonical mover), so each child slab is fetched through
  a symmetry when its row is assembled.  Slabs on disk are compact: the live bits only,
  in numeric order of the arrow index, which is colex order.  Losing positions are 1.

  Tiles.  Canonical sets are split into D classes.  The output of class c (columns with
  Y in class c) is one file, holding D tiles; tile d holds the slabs whose mover set is
  canonically in class d.  Assembling the rows of class d then reads tile d of all D
  files, which is a forward sweep with D short seeks.  There is no sort and no random
  I/O, so layers can stream from a disk as well as from flash.

  Storage.  --root may be given several times.  Each class goes to the first root with
  room, so a hot root takes what it can and the next takes the rest.  When a layer is no
  longer needed it is deleted, or, if kept, moved to the last root.  A copy is read back
  and compared before the original is deleted.

  Crash safety.  A class becomes visible by an atomic rename after fsync, and a run
  restarted with the same arguments continues from the first missing class.  zstd frame
  checksums are verified on every read.

  Usage:
    slabsolve solve WxH --root DIR [--root DIR2] [options]
        --queens Q         queens per side (4)
        --classes D        classes per layer (from the peak layer size)
        --level L          zstd level (3)
        --level-keep L     zstd level for layers that are kept (same as --level); when a
                           disk is the bottleneck a higher level can be faster overall
        --keep all|parity|none   layers to keep (all).  parity keeps the layers with an
                           odd number of empty cells, which are the more compressible
                           half, and layer 0; a dropped layer costs one ply of search
        --threads T        kernel threads in total (one per physical core)
        --aux T            compression threads (one per second hardware thread of a core)
        --workers W        NUMA workers (one per node)
        --reserve GIB      space to leave free on each root (100)
        --kmin K           stop after layer K
        --canon min|max    which queen set of an orbit is canonical (see BuildRows)
        --compare DIR      check each layer bit for bit against a `dense` directory
        --verify-samples N positions per layer relabelled from the rules through the
                           files on disk (1000)
    slabsolve bench WxH [--k K] [--rows R] [--threads T] [--workers W]
        kernel timing on real board sizes
    slabsolve probe WxH --root DIR [--root DIR2] --m CELLS --o CELLS [--a CELLS] [--moves]
        value of one position; cells are comma separated, row by row from 0.  Works with
        any board setup.  A dropped layer is answered by one ply of search.  --moves also
        lists every legal move with its value
    slabsolve playout WxH --root DIR [--root DIR2] [--games G] [--sample S]
        plays whole games and checks the stored table against the rules at every ply
    slabsolve selftest
        checks every low-bit permutation and index permutation against its definition

  Requires AVX-512 with VBMI and GFNI, and libzstd.
*/

#include <dirent.h>
#include <fcntl.h>
#include <immintrin.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;
using V = __m512i;

using Clock = std::chrono::steady_clock;
Clock::time_point g_t0;
double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "slabsolve: FATAL: %s\n", msg.c_str());
  std::fflush(stderr);
  std::fflush(stdout);
  // Other threads may be mid-write; nothing on disk counts until its rename, so stop now
  // rather than run destructors under them.
  ::_exit(1);
}

inline constexpr double kGiB = double(u64(1) << 30);

void PinCaller(int cpu) {
  if (cpu < 0) return;
  cpu_set_t s;
  CPU_ZERO(&s);
  CPU_SET(cpu, &s);
  ::sched_setaffinity(0, sizeof(s), &s);
}

// ------------------------------------------------------------- thread pools

// Fork-join pool for kernel stages.  The caller is thread 0.
class Pool {
 public:
  explicit Pool(const std::vector<int>& cpus) : n_(int(cpus.size())), cpus_(cpus) {
    for (int t = 1; t < n_; ++t) {
      th_.emplace_back([this, t] {
        PinCaller(cpus_[size_t(t)]);
        Loop(t);
      });
    }
  }
  ~Pool() {
    stop_.store(true);
    gen_.fetch_add(1);
    gen_.notify_all();
    for (auto& t : th_) t.join();
  }
  int size() const { return n_; }
  int cpu0() const { return cpus_.empty() ? -1 : cpus_[0]; }
  template <class Fn>
  void For(size_t count, size_t chunk, Fn&& fn) {
    if (count == 0) return;
    if (n_ == 1 || count <= chunk) {
      for (size_t i = 0; i < count; ++i) fn(i, 0);
      _mm_sfence();
      return;
    }
    std::atomic<size_t> next{0};
    auto body = [&](int tid) {
      for (;;) {
        const size_t b = next.fetch_add(chunk, std::memory_order_relaxed);
        if (b >= count) break;
        const size_t e = std::min(count, b + chunk);
        for (size_t i = b; i < e; ++i) fn(i, tid);
      }
      _mm_sfence();  // kernels write with non-temporal stores
    };
    job_ = body;
    pending_.store(n_ - 1, std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_release);
    gen_.notify_all();
    body(0);
    while (pending_.load(std::memory_order_acquire)) _mm_pause();
  }

 private:
  void Loop(int tid) {
    u64 seen = 0;
    for (;;) {
      u64 g;
      int spins = 0;
      while ((g = gen_.load(std::memory_order_acquire)) == seen) {
        if (++spins < 4000) {
          _mm_pause();
        } else {
          gen_.wait(g, std::memory_order_acquire);
        }
      }
      seen = g;
      if (stop_.load()) return;
      job_(tid);
      pending_.fetch_sub(1, std::memory_order_acq_rel);
    }
  }
  int n_;
  std::vector<int> cpus_;
  std::vector<std::thread> th_;
  std::function<void(int)> job_;
  std::atomic<u64> gen_{0};
  std::atomic<int> pending_{0};
  std::atomic<bool> stop_{false};
};

// FIFO pool for compression and decompression.
class TaskPool {
 public:
  explicit TaskPool(int n) {
    for (int t = 0; t < n; ++t) {
      th_.emplace_back([this] {
        for (;;) {
          std::function<void()> f;
          {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stop_ || !q_.empty(); });
            if (q_.empty()) return;
            f = std::move(q_.front());
            q_.pop_front();
          }
          f();
        }
      });
    }
  }
  ~TaskPool() {
    {
      std::lock_guard<std::mutex> l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : th_) t.join();
  }
  void Submit(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> l(m_);
      q_.push_back(std::move(f));
    }
    cv_.notify_one();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> q_;
  std::vector<std::thread> th_;
  bool stop_ = false;
};

class Latch {
 public:
  void Add(i64 n = 1) {
    std::lock_guard<std::mutex> l(m_);
    n_ += n;
  }
  void Done() {
    std::lock_guard<std::mutex> l(m_);
    if (--n_ == 0) cv_.notify_all();
  }
  void Wait() {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { return n_ == 0; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  i64 n_ = 0;
};

// Limits memory held in flight.
class ByteGate {
 public:
  explicit ByteGate(u64 cap) : cap_(cap) {}
  void Acquire(u64 n) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { return used_ == 0 || used_ + n <= cap_; });
    used_ += n;
  }
  void Release(u64 n) {
    std::lock_guard<std::mutex> l(m_);
    used_ -= n;
    cv_.notify_all();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  u64 cap_, used_ = 0;
};

// Page-aligned, zeroed, huge pages where the kernel offers them.
template <class T>
T* BigAlloc(size_t count) {
  const size_t bytes = std::max<size_t>(count * sizeof(T), 4096);
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) Die("mmap failed for " + std::to_string(bytes) + " bytes");
  ::madvise(p, bytes, MADV_HUGEPAGE);
  return static_cast<T*>(p);
}
template <class T>
void BigFree(T* p, size_t count) {
  if (p) ::munmap(p, std::max<size_t>(count * sizeof(T), 4096));
}

// ------------------------------------------------------------------ board

u64 SPR;  // slabs per row, C(N-Q, Q)
int W, H, N, Q, F, HB;
u32 WORDS;
u64 BOARD;
bool g_canon_max = false;
int RAYLEN[64][8];
int RAYCELL[64][8][16];
const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
const int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
int GN;
int GPERM[8][64], GINV[8][64];
u64 GT[8][3][1 << 11];
u64 C[65][65];

inline int CellId(int x, int y) { return y * W + x; }
inline void CellXY(int c, int* x, int* y) {
  *x = c % W;
  *y = c / W;
}

inline u64 Transform(int g, u64 b) {
  return GT[g][0][b & 2047] | GT[g][1][(b >> 11) & 2047] | GT[g][2][(b >> 22) & 2047];
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

inline u64 NextSubset(u64 v) {
  const u64 t = v | (v - 1);
  return (t + 1) | (((~t & (t + 1)) - 1) >> (__builtin_ctzll(v) + 1));
}

void InitBoard(int w, int h, int q) {
  W = w, H = h, N = w * h, Q = q, F = N - 2 * q;
  if (N > 33) Die("board too large");
  if (F < 1 || q < 1) Die("no free cells");
  HB = std::max(0, F - 9);
  WORDS = u32(1) << HB;
  BOARD = (u64(1) << N) - 1;
  for (int n = 0; n <= 64; ++n) {
    C[n][0] = 1;
    for (int k = 1; k <= n; ++k) C[n][k] = C[n - 1][k - 1] + (k < n ? C[n - 1][k] : 0);
  }
  SPR = C[N - Q][Q];
  for (int c = 0; c < N; ++c) {
    int x, y;
    CellXY(c, &x, &y);
    for (int d = 0; d < 8; ++d) {
      int len = 0;
      for (int nx = x + DX[d], ny = y + DY[d]; nx >= 0 && nx < w && ny >= 0 && ny < h; nx += DX[d], ny += DY[d]) {
        RAYCELL[c][d][len++] = CellId(nx, ny);
      }
      RAYLEN[c][d] = len;
    }
  }
  GN = (w == h) ? 8 : 4;
  for (int g = 0; g < GN; ++g) {
    const bool swap_axes = g & 4, flip_x = g & 1, flip_y = g & 2;
    for (int c = 0; c < N; ++c) {
      int x, y;
      CellXY(c, &x, &y);
      if (swap_axes) std::swap(x, y);
      if (flip_x) x = w - 1 - x;
      if (flip_y) y = h - 1 - y;
      const int gc = CellId(x, y);
      GPERM[g][c] = gc;
      GINV[g][gc] = c;
    }
    for (int chunk = 0; chunk < 3; ++chunk) {
      for (int v = 0; v < (1 << 11); ++v) {
        u64 r = 0;
        for (int b = 0; b < 11; ++b) {
          const int c = chunk * 11 + b;
          if (((v >> b) & 1) && c < N) r |= u64(1) << GPERM[g][c];
        }
        GT[g][chunk][v] = r;
      }
    }
  }
}

// ------------------------------------------------------------------- rows

std::vector<u64> g_qsub;    // Q-subsets of N-Q positions, numeric order; index = colex rank
std::vector<u64> g_rows;    // canonical queen sets
std::vector<int> g_orbit;   // orbit size of each row's queen set
std::vector<u32> g_rowmap;  // rank of a queen set -> row index << 3 | g, with g(S) canonical

inline u64 RankIn(u64 x, u64 y) { return RankSubset(_pext_u64(x, ~y & BOARD)); }

// The canonical set of an orbit is its smallest or largest member as a board mask.  The
// choice decides whether queens gather in the cells that are the least or the most
// significant arrow-index bits, which matters to compression and to nothing else.
void BuildRows() {
  g_rows.clear();
  g_orbit.clear();
  g_qsub.resize(SPR);
  u64 v = (u64(1) << Q) - 1;
  for (u64 i = 0; i < SPR; ++i) {
    g_qsub[i] = v;
    if (i + 1 < SPR) v = NextSubset(v);
  }
  const u64 sets = C[N][Q];
  g_rowmap.resize(sets);
  auto better = [](u64 a, u64 b) { return g_canon_max ? a > b : a < b; };
  std::vector<u64> all(sets);
  v = (u64(1) << Q) - 1;
  for (u64 i = 0; i < sets; ++i) {
    all[i] = v;
    if (i + 1 < sets) v = NextSubset(v);
  }
  for (u64 i = 0; i < sets; ++i) {
    bool canonical = true;
    int stab = 0;
    for (int g = 0; g < GN; ++g) {
      const u64 t = Transform(g, all[i]);
      if (t == all[i]) ++stab;
      if (better(t, all[i])) canonical = false;
    }
    if (canonical) {
      g_rowmap[i] = u32(g_rows.size() << 3);
      g_rows.push_back(all[i]);
      g_orbit.push_back(GN / stab);
    }
  }
  for (u64 i = 0; i < sets; ++i) {
    u64 best = all[i];
    int bg = 0;
    for (int g = 0; g < GN; ++g) {
      const u64 t = Transform(g, all[i]);
      if (better(t, best)) best = t, bg = g;
    }
    if (best != all[i]) g_rowmap[i] = (g_rowmap[RankSubset(best)] & ~u32(7)) | u32(bg);
  }
}

// ------------------------------------------------------- low-bit permutes

inline u32 LowMask(int b) { return (u32(1) << b) - 1; }
inline u32 RemoveBit(u32 v, int b) { return (v & LowMask(b)) | ((v >> 1) & ~LowMask(b)); }
inline u32 Insert0(u32 v, int b) { return (v & LowMask(b)) | ((v << 1) & ~LowMask(b + 1)); }

enum PermKind : u8 { kId, kByte, kBit, kMixDown, kMixUp };

struct alignas(64) LowPerm {
  V idx0, idx1;
  u64 a0 = 0, a1 = 0;
  PermKind kind = kId;
};

LowPerm BT[9][9];  // both ranks low: child z' = Insert0(RemoveBit(z, bt), bf)
LowPerm CT[9];     // t low, f high: index bit 9 of the parent is bit 8 of the child's z'
LowPerm DT[9];     // t high, f low: bit 8 of z selects one of two child words
V NOTSET[512];     // bits z with z & mask == 0
V LANEDOWN[3];     // lane j <- lane j + 2^i
V LANEUP[3];       // lane j <- lane j - 2^i
V SWAPMASK[3][6];  // bits with z_l = 1 and z_b = 0, for l < 3, b in 3..8
u64 MASK6[7];      // bits z in [0, 64) with popcount j
u64 LMASK[8][7];   // lane, popcount -> live bits; differs from MASK6 only for F < 9
int LCNT[8][7];

V MakeIdx(const u8 b[64]) { return _mm512_loadu_si512(b); }

u64 AffineRows(const int src[8]) {
  u64 a = 0;
  for (int p = 0; p < 8; ++p) {
    if (src[p] >= 0) a |= u64(u8(1u << src[p])) << (8 * (7 - p));
  }
  return a;
}

void InitLowPerms() {
  for (int m = 0; m < 512; ++m) {
    alignas(64) u64 lanes[8] = {};
    for (int z = 0; z < 512; ++z) {
      if (!(z & m)) lanes[z >> 6] |= u64(1) << (z & 63);
    }
    NOTSET[m] = _mm512_load_si512(lanes);
  }
  for (int i = 0; i < 3; ++i) {
    alignas(64) u64 down[8], up[8];
    for (int j = 0; j < 8; ++j) {
      down[j] = u64((j + (1 << i)) & 7);
      up[j] = u64((j - (1 << i)) & 7);
    }
    LANEDOWN[i] = _mm512_load_si512(down);
    LANEUP[i] = _mm512_load_si512(up);
  }
  for (int l = 0; l < 3; ++l) {
    for (int b = 3; b < 9; ++b) {
      alignas(64) u64 lanes[8] = {};
      for (int z = 0; z < 512; ++z) {
        if (((z >> l) & 1) && !((z >> b) & 1)) lanes[z >> 6] |= u64(1) << (z & 63);
      }
      SWAPMASK[l][b - 3] = _mm512_load_si512(lanes);
    }
  }
  for (int z = 0; z < 64; ++z) {
    MASK6[__builtin_popcount(z)] |= u64(1) << z;
  }

  for (int bt = 0; bt < 9; ++bt) {
    for (int bf = 0; bf < 9; ++bf) {
      LowPerm& p = BT[bt][bf];
      auto map = [&](int z) -> int { return int(Insert0(RemoveBit(u32(z), bt), bf)); };
      u8 i0[64] = {}, i1[64] = {};
      int s0[8], s1[8];
      std::fill(s0, s0 + 8, -1);
      std::fill(s1, s1 + 8, -1);
      if (bt == bf) {
        p.kind = kId;
      } else if (bt >= 3 && bf >= 3) {
        p.kind = kByte;
        for (int zb = 0; zb < 64; ++zb) {
          const int z = zb << 3;
          if (!((z >> bt) & 1)) i0[zb] = u8(map(z) >> 3);
        }
      } else if (bt < 3 && bf < 3) {
        p.kind = kBit;
        for (int b = 0; b < 8; ++b) {
          if (!((b >> bt) & 1)) s0[b] = map(b);
        }
      } else if (bt < 3) {  // bf >= 3: the parent's z_3 is the child's z'_2
        p.kind = kMixDown;
        for (int zb = 0; zb < 64; ++zb) i0[zb] = u8(map(zb << 3) >> 3);
        for (int b = 0; b < 8; ++b) {
          if ((b >> bt) & 1) continue;
          s0[b] = map(b) & 7;
          s1[b] = map(8 | b) & 7;
        }
      } else {  // bf < 3 <= bt: the parent's z_2 is the child's z'_3
        p.kind = kMixUp;
        for (int zb = 0; zb < 64; ++zb) {
          const int z = zb << 3;
          if ((z >> bt) & 1) continue;
          i0[zb] = u8(map(z) >> 3);
          i1[zb] = u8(map(z | 4) >> 3);
        }
        for (int b = 0; b < 4; ++b) s0[b] = map(b) & 7;
        for (int b = 4; b < 8; ++b) s1[b] = map(b) & 7;
      }
      p.idx0 = MakeIdx(i0);
      p.idx1 = MakeIdx(i1);
      p.a0 = AffineRows(s0);
      p.a1 = AffineRows(s1);
    }
  }
  for (int bt = 0; bt < 9; ++bt) {
    LowPerm& p = CT[bt];
    auto map = [&](int z, int h) -> int { return int((RemoveBit(u32(z), bt) & 255) | u32(h << 8)); };
    u8 i0[64] = {}, i1[64] = {};
    int s0[8], s1[8];
    std::fill(s0, s0 + 8, -1);
    std::fill(s1, s1 + 8, -1);
    p.kind = bt >= 3 ? kByte : kMixDown;
    for (int zb = 0; zb < 64; ++zb) {
      const int z = zb << 3;
      if (bt >= 3 && ((z >> bt) & 1)) continue;
      i0[zb] = u8(map(z, 0) >> 3);
      i1[zb] = u8(map(z, 1) >> 3);
    }
    if (bt < 3) {
      for (int b = 0; b < 8; ++b) {
        if ((b >> bt) & 1) continue;
        s0[b] = map(b, 0) & 7;
        s1[b] = map(8 | b, 0) & 7;
      }
    }
    p.idx0 = MakeIdx(i0);
    p.idx1 = MakeIdx(i1);
    p.a0 = AffineRows(s0);
    p.a1 = AffineRows(s1);
  }
  for (int bf = 0; bf < 9; ++bf) {
    LowPerm& p = DT[bf];
    auto map = [&](int z) -> int { return int(Insert0(u32(z) & 255, bf)); };
    u8 i0[64] = {}, i1[64] = {};
    int s0[8], s1[8];
    std::fill(s0, s0 + 8, -1);
    std::fill(s1, s1 + 8, -1);
    p.kind = bf >= 3 ? kByte : kMixUp;
    for (int zb = 0; zb < 64; ++zb) {
      const int z = zb << 3, h = zb >> 5;
      i0[zb] = u8((map(z) >> 3) | (h << 6));
      i1[zb] = u8((map(z | 4) >> 3) | (h << 6));
    }
    if (bf < 3) {
      for (int b = 0; b < 4; ++b) s0[b] = map(b) & 7;
      for (int b = 4; b < 8; ++b) s1[b] = map(b) & 7;
    }
    p.idx0 = MakeIdx(i0);
    p.idx1 = MakeIdx(i1);
    p.a0 = AffineRows(s0);
    p.a1 = AffineRows(s1);
  }
}

const __mmask64 kEvenBytes = 0x5555555555555555ULL;
const __mmask64 kOddBytes = 0xAAAAAAAAAAAAAAAAULL;

inline V ApplyMixDown(V byte_mapped, u64 a0, u64 a1) {
  const V even = _mm512_maskz_gf2p8affine_epi64_epi8(kEvenBytes, byte_mapped, _mm512_set1_epi64((long long)a0), 0);
  return _mm512_mask_gf2p8affine_epi64_epi8(even, kOddBytes, byte_mapped, _mm512_set1_epi64((long long)a1), 0);
}

inline V ApplyB(const LowPerm& p, V w) {
  switch (p.kind) {
    case kId:
      return w;
    case kByte:
      return _mm512_permutexvar_epi8(p.idx0, w);
    case kBit:
      return _mm512_gf2p8affine_epi64_epi8(w, _mm512_set1_epi64((long long)p.a0), 0);
    case kMixDown:
      return ApplyMixDown(_mm512_permutexvar_epi8(p.idx0, w), p.a0, p.a1);
    default: {
      const V a = _mm512_gf2p8affine_epi64_epi8(_mm512_permutexvar_epi8(p.idx0, w), _mm512_set1_epi64((long long)p.a0), 0);
      const V b = _mm512_gf2p8affine_epi64_epi8(_mm512_permutexvar_epi8(p.idx1, w), _mm512_set1_epi64((long long)p.a1), 0);
      return _mm512_or_si512(a, b);
    }
  }
}

inline V ApplyC(const LowPerm& p, V w, int h) {
  const V x = _mm512_permutexvar_epi8(h ? p.idx1 : p.idx0, w);
  return p.kind == kByte ? x : ApplyMixDown(x, p.a0, p.a1);
}

inline V ApplyD(const LowPerm& p, V w0, V w1) {
  if (p.kind == kByte) return _mm512_permutex2var_epi8(w0, p.idx0, w1);
  const V a = _mm512_gf2p8affine_epi64_epi8(_mm512_permutex2var_epi8(w0, p.idx0, w1), _mm512_set1_epi64((long long)p.a0), 0);
  const V b = _mm512_gf2p8affine_epi64_epi8(_mm512_permutex2var_epi8(w0, p.idx1, w1), _mm512_set1_epi64((long long)p.a1), 0);
  return _mm512_or_si512(a, b);
}

// ----------------------------------------------------------------- kernel

// a | (b & c)
inline V OrAnd(V a, V b, V c) { return _mm512_ternarylogic_epi64(a, b, c, 0xF8); }

// Which words of one layer hold live bits: popcount(i) in [k-9, k].
struct Live {
  int k = -1;
  int mode = 0;  // 0: nearly all words live, 1: test each word, 2: walk the list
  std::vector<u32> list;
  std::vector<u8> flag;
  void Set(int layer) {
    k = layer;
    list.clear();
    flag.assign(WORDS, 0);
    for (u32 i = 0; i < WORDS; ++i) {
      const int j = layer - __builtin_popcount(i);
      if (j >= 0 && j <= 9) {
        list.push_back(i);
        flag[i] = 1;
      }
    }
    if (list.size() * 4 < WORDS) mode = 2;
    else if (list.size() * 10 < u64(WORDS) * 9) mode = 1;
    else mode = 0;
  }
};

Live g_lq;  // words of the layer being computed, and of its shot tables
Live g_lc;  // words of the child layer

template <class Fn>
inline void ForWords(u32 ph, int run_limit, Fn&& fn) {
  if (g_lq.mode == 2) {
    for (const u32 i : g_lq.list) {
      if (!(i & ph)) fn(i, u32(1));
    }
    return;
  }
  if (g_lq.mode == 1) {
    const u8* flag = g_lq.flag.data();
    const u32 notp = ~ph;
    for (u32 i = 0; i < WORDS; i = ((i | ph) + 1) & notp) {
      if (flag[i]) fn(i, u32(1));
    }
    return;
  }
  const int run = std::min(ph ? __builtin_ctz(ph) : HB, run_limit);
  const u32 step = u32(1) << run;
  const u32 lowm = step - 1, notp = ~ph;
  for (u32 i = 0; i < WORDS; i = ((i | ph | lowm) + 1) & notp) fn(i, step);
}

inline void ClearLive(V* w) {
  if (g_lq.mode == 2) {
    const V z = _mm512_setzero_si512();
    for (const u32 i : g_lq.list) _mm512_store_si512(w + i, z);
  } else {
    std::memset(static_cast<void*>(w), 0, size_t(WORDS) * sizeof(V));
  }
}

// Stage 1 for one child slab: mover y, opponent queens xp; c holds L at layer k+1.
// Tables are built in a scratch buffer that stays in cache and streamed out, which
// saves the read-for-ownership traffic of writing them in place.
void BuildShots(u64 y, u64 xp, const V* c, V* out, V* q) {
  const u64 occ = y | xp;
  const u64 free_cells = ~occ & BOARD;
  u64 xs = xp;
  int ti = 0;
  const u32 words = WORDS;
  while (xs) {
    const int t = __builtin_ctzll(xs);
    xs &= xs - 1;
    V* dest = out + size_t(ti++) * words;
    ClearLive(q);
    for (int d = 0; d < 8; ++d) {
      u32 path = 0;
      for (int j = 0; j < RAYLEN[t][d]; ++j) {
        const int s = RAYCELL[t][d][j];
        if ((occ >> s) & 1) break;
        const int b = __builtin_popcountll(free_cells & ((u64(1) << s) - 1));
        path |= u32(1) << b;
        const u32 ph = path >> 9;
        const V mask = NOTSET[path & 511];
        if (b >= 9) {
          const u32 bit = u32(1) << (b - 9);
          ForWords(ph, HB, [&](u32 i, u32 step) {
            const V* src = c + (i | bit);
            V* dst = q + i;
            for (u32 n = 0; n < step; ++n) {
              _mm512_store_si512(dst + n, OrAnd(_mm512_load_si512(dst + n), _mm512_load_si512(src + n), mask));
            }
          });
        } else if (b < 6) {
          const unsigned sh = 1u << b;
          ForWords(ph, HB, [&](u32 i, u32 step) {
            const V* src = c + i;
            V* dst = q + i;
            for (u32 n = 0; n < step; ++n) {
              _mm512_store_si512(dst + n, OrAnd(_mm512_load_si512(dst + n), _mm512_srli_epi64(_mm512_load_si512(src + n), sh), mask));
            }
          });
        } else {
          const V idx = LANEDOWN[b - 6];
          ForWords(ph, HB, [&](u32 i, u32 step) {
            const V* src = c + i;
            V* dst = q + i;
            for (u32 n = 0; n < step; ++n) {
              _mm512_store_si512(dst + n, OrAnd(_mm512_load_si512(dst + n), _mm512_permutexvar_epi64(idx, _mm512_load_si512(src + n)), mask));
            }
          });
        }
      }
    }
    if (g_lq.mode == 0) {
      for (u32 i = 0; i < words; ++i) _mm512_stream_si512(dest + i, _mm512_load_si512(q + i));
    } else {
      for (const u32 i : g_lq.list) _mm512_stream_si512(dest + i, _mm512_load_si512(q + i));
    }
  }
}

// Stage 2 for one parent move: OR the child's shot table into w.
inline void ApplyMove(V* w, const V* q, u32 ph, u32 pl, int bt, int bf) {
  const V mask = NOTSET[pl];
  if (bt >= 9 && bf >= 9) {
    const int tb = bt - 9, fb = bf - 9;
    ForWords(ph, std::min(tb, fb), [&](u32 i, u32 step) {
      const u32 ip = Insert0(RemoveBit(i, tb), fb);
      for (u32 n = 0; n < step; ++n) {
        _mm512_store_si512(w + i + n, OrAnd(_mm512_load_si512(w + i + n), _mm512_load_si512(q + ip + n), mask));
      }
    });
  } else if (bt < 9 && bf < 9) {
    const LowPerm& p = BT[bt][bf];
    switch (p.kind) {
      case kId:
        ForWords(ph, HB, [&](u32 i, u32 step) {
          for (u32 n = 0; n < step; ++n) {
            _mm512_store_si512(w + i + n, OrAnd(_mm512_load_si512(w + i + n), _mm512_load_si512(q + i + n), mask));
          }
        });
        break;
      case kByte:
        ForWords(ph, HB, [&](u32 i, u32 step) {
          for (u32 n = 0; n < step; ++n) {
            _mm512_store_si512(w + i + n, OrAnd(_mm512_load_si512(w + i + n), _mm512_permutexvar_epi8(p.idx0, _mm512_load_si512(q + i + n)), mask));
          }
        });
        break;
      default:
        ForWords(ph, HB, [&](u32 i, u32 step) {
          for (u32 n = 0; n < step; ++n) {
            _mm512_store_si512(w + i + n, OrAnd(_mm512_load_si512(w + i + n), ApplyB(p, _mm512_load_si512(q + i + n)), mask));
          }
        });
        break;
    }
  } else if (bt < 9) {
    const LowPerm& p = CT[bt];
    const int fb = bf - 9;
    ForWords(ph, 0, [&](u32 i, u32) {
      const u32 ip = ((i >> 1) & LowMask(fb)) | (i & ~LowMask(fb + 1));
      _mm512_store_si512(w + i, OrAnd(_mm512_load_si512(w + i), ApplyC(p, _mm512_load_si512(q + ip), int(i & 1)), mask));
    });
  } else {
    const LowPerm& p = DT[bf];
    const int tb = bt - 9;
    ForWords(ph, 0, [&](u32 i, u32) {
      const u32 ip = ((i & LowMask(tb)) << 1) | (i & ~LowMask(tb + 1));
      _mm512_store_si512(w + i, OrAnd(_mm512_load_si512(w + i), ApplyD(p, _mm512_load_si512(q + ip), _mm512_load_si512(q + ip + 1)), mask));
    });
  }
}

// Stage 2 for one parent slab.  shots holds Q tables per child, in child rank order.
void SolveParent(u64 x, u64 y, const V* shots, V* w) {
  const u64 occ = x | y;
  const u64 free_cells = ~occ & BOARD;
  const size_t words = WORDS;
  ClearLive(w);
  u64 xs = x;
  while (xs) {
    const int f = __builtin_ctzll(xs);
    xs &= xs - 1;
    const u64 fbit = u64(1) << f;
    for (int d = 0; d < 8; ++d) {
      u32 path = 0;
      for (int j = 0; j < RAYLEN[f][d]; ++j) {
        const int t = RAYCELL[f][d][j];
        const u64 tbit = u64(1) << t;
        if (occ & tbit) break;
        const int bt = __builtin_popcountll(free_cells & (tbit - 1));
        path |= u32(1) << bt;
        const u64 xp = (x ^ fbit) | tbit;
        const u64 child_free = (free_cells ^ tbit) | fbit;
        const int bf = __builtin_popcountll(child_free & (fbit - 1));
        const int ti = __builtin_popcountll(xp & (tbit - 1));
        const V* q = shots + (RankIn(xp, y) * u64(Q) + u64(ti)) * words;
        ApplyMove(w, q, path >> 9, path & 511, bt, bf);
      }
    }
  }
}

// ---------------------------------------------------------- compact slabs

inline u64 StrideWords(int k) { return (C[F][k] + 63) / 64; }

void InitLaneMasks() {
  for (int l = 0; l < 8; ++l) {
    for (int j = 0; j < 7; ++j) {
      u64 valid = ~u64(0);
      if (F < 9) {
        const u64 base = u64(l) << 6;
        if (base >= (u64(1) << F)) {
          valid = 0;
        } else if ((u64(1) << F) - base < 64) {
          valid = (u64(1) << ((u64(1) << F) - base)) - 1;
        }
      }
      LMASK[l][j] = MASK6[j] & valid;
      LCNT[l][j] = __builtin_popcountll(LMASK[l][j]);
    }
  }
}

// One compact slab of layer k into 8 lanes, advancing the bit position.
inline void ExpandWord(const char* bytes, u64* pos, int j, u64* l) {
  for (int a = 0; a < 8; ++a) {
    const int ja = j - __builtin_popcount(a);
    if (ja < 0 || ja > 6 || !LCNT[a][ja]) {
      l[a] = 0;
      continue;
    }
    u64 v;
    std::memcpy(&v, bytes + (*pos >> 3), 8);
    v >>= (*pos & 7);
    l[a] = _pdep_u64(v, LMASK[a][ja]);
    *pos += u64(LCNT[a][ja]);
  }
}

// Into the child table, which is zero outside the live words of the layer.
void ExpandSlabStream(const u64* src, int k, V* out) {
  const char* bytes = reinterpret_cast<const char*>(src);
  u64 pos = 0;
  alignas(64) u64 l[8];
  for (const u32 i : g_lc.list) {
    ExpandWord(bytes, &pos, k - __builtin_popcount(i), l);
    _mm512_stream_si512(out + i, _mm512_load_si512(l));
  }
}

// Into a buffer that the caller has cleared.
void ExpandSlabPlain(const u64* src, int k, V* out) {
  const char* bytes = reinterpret_cast<const char*>(src);
  u64 pos = 0;
  u64* lanes = reinterpret_cast<u64*>(out);
  for (u32 i = 0; i < WORDS; ++i) {
    const int j = k - __builtin_popcount(i);
    if (j < 0 || j > 9) continue;
    ExpandWord(bytes, &pos, j, lanes + size_t(i) * 8);
  }
}

// The complement of w within the live bits, in numeric order.  Returns its popcount.
u64 CompactLosses(const V* w, int k, u64* dst) {
  const u64* lanes = reinterpret_cast<const u64*>(w);
  u64 acc = 0, total = 0;
  int fill = 0;
  u64* d = dst;
  for (u32 i = 0; i < WORDS; ++i) {
    const int j = k - __builtin_popcount(i);
    if (j < 0 || j > 9) continue;
    const u64* l = lanes + size_t(i) * 8;
    for (int a = 0; a < 8; ++a) {
      const int ja = j - __builtin_popcount(a);
      if (ja < 0 || ja > 6) continue;
      const int cnt = LCNT[a][ja];
      if (!cnt) continue;
      const u64 v = _pext_u64(~l[a], LMASK[a][ja]);
      total += u64(__builtin_popcountll(v));
      acc |= v << fill;
      fill += cnt;
      if (fill >= 64) {
        *d++ = acc;
        fill -= 64;
        acc = fill ? v >> (cnt - fill) : 0;
      }
    }
  }
  if (fill) *d++ = acc;
  return total;
}

std::vector<std::vector<u32>> g_unrank;  // k, colex rank -> arrow index
std::vector<u32> g_rank;                 // arrow index -> colex rank

void BuildArrowRanks() {
  g_rank.resize(size_t(1) << F);
  g_unrank.assign(size_t(F) + 1, {});
  for (int k = 0; k <= F; ++k) g_unrank[size_t(k)].reserve(C[F][k]);
  for (u64 v = 0; v < (u64(1) << F); ++v) {
    auto& u = g_unrank[size_t(__builtin_popcountll(v))];
    g_rank[v] = u32(u.size());
    u.push_back(u32(v));
  }
}

// Exchanges index bits l (below 9) and 9 + hb in place, over all words.
void SwapLowHigh(V* a, int l, int hb) {
  const u32 hbit = u32(1) << hb;
  const V m0 = NOTSET[1 << l];
  if (l < 6) {
    const unsigned s = 1u << l;
    for (u32 i = 0; i < WORDS; ++i) {
      if (i & hbit) continue;
      V w0 = _mm512_load_si512(a + i), w1 = _mm512_load_si512(a + (i | hbit));
      const V t = _mm512_and_si512(_mm512_xor_si512(_mm512_srli_epi64(w0, s), w1), m0);
      w1 = _mm512_xor_si512(w1, t);
      w0 = _mm512_xor_si512(w0, _mm512_slli_epi64(t, s));
      _mm512_store_si512(a + i, w0);
      _mm512_store_si512(a + (i | hbit), w1);
    }
  } else {
    const V down = LANEDOWN[l - 6], up = LANEUP[l - 6];
    for (u32 i = 0; i < WORDS; ++i) {
      if (i & hbit) continue;
      V w0 = _mm512_load_si512(a + i), w1 = _mm512_load_si512(a + (i | hbit));
      const V t = _mm512_and_si512(_mm512_xor_si512(_mm512_permutexvar_epi64(down, w0), w1), m0);
      w1 = _mm512_xor_si512(w1, t);
      w0 = _mm512_xor_si512(w0, _mm512_permutexvar_epi64(up, t));
      _mm512_store_si512(a + i, w0);
      _mm512_store_si512(a + (i | hbit), w1);
    }
  }
}

// Exchanges index bits l (below 3) and b (3..8) inside every word.
inline V SwapInWord(V w, int l, int b) {
  const V m = SWAPMASK[l][b - 3];
  if (b < 6) {
    const unsigned s = (1u << b) - (1u << l);
    const V t = _mm512_and_si512(_mm512_xor_si512(_mm512_srli_epi64(w, s), w), m);
    return _mm512_xor_si512(w, _mm512_xor_si512(t, _mm512_slli_epi64(t, s)));
  }
  const unsigned s = 1u << l;
  const V x = _mm512_slli_epi64(_mm512_permutexvar_epi64(LANEDOWN[b - 6], w), s);
  const V t = _mm512_and_si512(_mm512_xor_si512(x, w), m);
  const V w1 = _mm512_xor_si512(w, t);
  return _mm512_xor_si512(w1, _mm512_permutexvar_epi64(LANEUP[b - 6], _mm512_srli_epi64(t, s)));
}

struct PermScratch {
  std::vector<u32> t1, t2, word;
  PermScratch() : t1(1u << 11), t2(size_t(1) << std::max(0, F - 11)), word(WORDS) {}
};

// A compact slab seen through a symmetry, by walking its set bits.  Source index bit b
// becomes target index bit cur[b].  Worth it only when few bits are set.
void ExpandViaBits(const u64* src, int k, const int* cur, V* buf, V* out, PermScratch& sc) {
  sc.t1[0] = 0;
  for (u32 v = 1; v < (1u << 11); ++v) {
    const int low = __builtin_ctz(v);
    sc.t1[v] = sc.t1[v & (v - 1)] | (low < F ? u32(1) << cur[low] : 0);
  }
  sc.t2[0] = 0;
  for (size_t v = 1; v < sc.t2.size(); ++v) {
    const int low = __builtin_ctzll(v);
    sc.t2[v] = sc.t2[v & (v - 1)] | (u32(1) << cur[low + 11]);
  }
  const V z = _mm512_setzero_si512();
  for (const u32 i : g_lc.list) _mm512_store_si512(buf + i, z);
  u64* lanes = reinterpret_cast<u64*>(buf);
  const u32* un = g_unrank[size_t(k)].data();
  const u64 stride = StrideWords(k);
  for (u64 wi = 0; wi < stride; ++wi) {
    u64 word = src[wi];
    while (word) {
      const u64 r = wi * 64 + u64(__builtin_ctzll(word));
      word &= word - 1;
      const u32 u = un[r];
      const u32 t = sc.t1[u & 2047] | sc.t2[u >> 11];
      lanes[t >> 6] |= u64(1) << (t & 63);
    }
  }
  for (const u32 i : g_lc.list) _mm512_stream_si512(out + i, _mm512_load_si512(buf + i));
}

// As above by permuting whole words: index bits are exchanged across the word boundary,
// then permuted within the word and among words.  Cost depends on the swaps, not on the
// number of set bits.  cur is destroyed.
void ExpandViaWords(const u64* src, int k, int* cur, V* a, V* out, PermScratch& sc) {
  std::memset(static_cast<void*>(a), 0, size_t(WORDS) * sizeof(V));
  ExpandSlabPlain(src, k, a);
  for (int b = F; b < 9; ++b) cur[b] = b;
  for (int h = 9; h < F; ++h) {
    if (cur[h] >= 9) continue;
    int l = 0;
    while (cur[l] < 9) ++l;
    SwapLowHigh(a, l, h - 9);
    std::swap(cur[l], cur[h]);
  }
  int swaps[3][2];
  int nswaps = 0;
  for (int b = 0; b < 3; ++b) {
    if (cur[b] < 3) continue;
    int b2 = 3;
    while (cur[b2] >= 3) ++b2;
    swaps[nswaps][0] = b;
    swaps[nswaps][1] = b2;
    ++nswaps;
    std::swap(cur[b], cur[b2]);
  }
  bool byte_id = true, bit_id = true;
  for (int b = 0; b < 3; ++b) bit_id = bit_id && cur[b] == b;
  for (int b = 3; b < 9; ++b) byte_id = byte_id && cur[b] == b;
  alignas(64) u8 idx[64];
  for (int zb = 0; zb < 64; ++zb) {
    int o = 0;
    for (int b = 0; b < 6; ++b) {
      if ((zb >> b) & 1) o |= 1 << (cur[b + 3] - 3);
    }
    idx[o] = u8(zb);
  }
  int rows[8];
  for (int p = 0; p < 8; ++p) {
    int o = 0;
    for (int b = 0; b < 3; ++b) {
      if ((p >> b) & 1) o |= 1 << cur[b];
    }
    rows[o] = p;
  }
  const V vidx = _mm512_load_si512(idx);
  const V affine = _mm512_set1_epi64((long long)AffineRows(rows));
  u32* tw = sc.word.data();
  tw[0] = 0;
  for (u32 i = 1; i < WORDS; ++i) tw[i] = tw[i & (i - 1)] | (u32(1) << (cur[9 + __builtin_ctz(i)] - 9));
  const u8* live = g_lc.flag.data();
  for (u32 i = 0; i < WORDS; ++i) {
    const u32 ip = tw[i];
    if (!live[ip]) continue;
    V w = _mm512_load_si512(a + i);
    for (int s = 0; s < nswaps; ++s) w = SwapInWord(w, swaps[s][0], swaps[s][1]);
    if (!byte_id) w = _mm512_permutexvar_epi8(vidx, w);
    if (!bit_id) w = _mm512_gf2p8affine_epi64_epi8(w, affine, 0);
    _mm512_stream_si512(out + ip, w);
  }
}

inline void IndexMapVia(u64 src_free, u64 dst_free, int g, int* cur) {
  int b = 0;
  for (u64 m = src_free; m; m &= m - 1, ++b) {
    const int ct = GINV[g][__builtin_ctzll(m)];
    cur[b] = __builtin_popcountll(dst_free & ((u64(1) << ct) - 1));
  }
}

void ExpandSlabVia(const u64* src, int k, u64 src_free, u64 dst_free, int g, V* buf, V* out, PermScratch& sc) {
  int cur[32];
  IndexMapVia(src_free, dst_free, g, cur);
  if (HB == 0 || C[F][k] < u64(WORDS) * 8) {
    ExpandViaBits(src, k, cur, buf, out, sc);
  } else {
    ExpandViaWords(src, k, cur, buf, out, sc);
  }
}

// ------------------------------------------------------------------ tiles

int D = 1;
std::vector<u64> g_class_begin;  // class -> first row
std::vector<u16> g_set_class;    // rank of any queen set -> class of its canonical row
std::vector<u16> g_ordinal;      // col * SPR + j -> place among same-class slabs of the column
std::vector<u32> g_colstart;     // col * D + d -> slabs of tile (class(col), d) in earlier columns
std::vector<u64> g_tile_slabs;   // c * D + d -> slabs in the tile
constexpr u64 kChunkRaw = u64(16) << 20;

inline int ClassOfRow(u64 r) { return int(r * u64(D) / g_rows.size()); }

void BuildTiles() {
  const u64 rows = g_rows.size();
  if (u64(D) > rows) D = int(rows);
  g_class_begin.assign(size_t(D) + 1, rows);
  for (u64 r = rows; r-- > 0;) g_class_begin[size_t(ClassOfRow(r))] = r;
  g_set_class.resize(C[N][Q]);
  for (size_t i = 0; i < g_set_class.size(); ++i) g_set_class[i] = u16(ClassOfRow(g_rowmap[i] >> 3));
  g_ordinal.resize(size_t(rows * SPR));
  std::vector<u32> colcount(size_t(rows) * size_t(D));
  std::vector<std::thread> pool;
  const int threads = std::min<int>(32, int(std::thread::hardware_concurrency()));
  std::atomic<u64> next{0};
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      std::vector<u32> cnt(static_cast<size_t>(D));
      for (;;) {
        const u64 y = next.fetch_add(1);
        if (y >= rows) break;
        std::fill(cnt.begin(), cnt.end(), 0);
        const u64 rest = ~g_rows[y] & BOARD;
        for (u64 j = 0; j < SPR; ++j) {
          const u64 x = _pdep_u64(g_qsub[j], rest);
          const int d = g_set_class[RankSubset(x)];
          g_ordinal[y * SPR + j] = u16(cnt[size_t(d)]++);
        }
        for (int d = 0; d < D; ++d) colcount[y * u64(D) + u64(d)] = cnt[size_t(d)];
      }
    });
  }
  for (auto& t : pool) t.join();
  g_colstart.resize(size_t(rows) * size_t(D));
  g_tile_slabs.assign(size_t(D) * size_t(D), 0);
  for (u64 y = 0; y < rows; ++y) {
    const int c = ClassOfRow(y);
    for (int d = 0; d < D; ++d) {
      u64& t = g_tile_slabs[size_t(c) * size_t(D) + size_t(d)];
      g_colstart[y * u64(D) + u64(d)] = u32(t);
      t += colcount[y * u64(D) + u64(d)];
    }
  }
}

// ---------------------------------------------------------------- storage

struct Geom {
  u64 stride, slab_bytes, chunk_slabs;
};
inline Geom GeomOf(int k) {
  Geom g;
  g.stride = StrideWords(k);
  g.slab_bytes = g.stride * 8;
  g.chunk_slabs = std::max<u64>(1, kChunkRaw / g.slab_bytes);
  return g;
}

inline u64 HashWords(u64 h, const char* p, size_t n) {
  while (n >= 8) {
    u64 w;
    std::memcpy(&w, p, 8);
    h = (h ^ w) * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    p += 8;
    n -= 8;
  }
  while (n--) {
    h = (h ^ u8(*p++)) * 0x100000001B3ULL;
  }
  return h;
}

// HashWords over a byte stream, however the stream is cut into pieces.
class StreamHash {
 public:
  void Update(const char* p, size_t n) {
    if (fill_) {
      while (n && fill_ < 8) {
        buf_[fill_++] = *p++;
        --n;
      }
      if (fill_ < 8) return;
      h_ = HashWords(h_, buf_, 8);
      fill_ = 0;
    }
    const size_t whole = n & ~size_t(7);
    h_ = HashWords(h_, p, whole);
    p += whole;
    n -= whole;
    while (n--) buf_[fill_++] = *p++;
  }
  u64 Final() const { return HashWords(h_, buf_, size_t(fill_)); }

 private:
  u64 h_ = 0x5EED;
  char buf_[8];
  int fill_ = 0;
};

std::vector<std::string> g_roots;
u64 g_reserve = u64(100) << 30;

std::string LayerDir(const std::string& root, int k) { return root + "/k" + std::to_string(k); }
std::string DataPath(const std::string& root, int k, int c) { return LayerDir(root, k) + "/c" + std::to_string(c) + ".tiles"; }
std::string IndexPath(const std::string& root, int k, int c) { return LayerDir(root, k) + "/c" + std::to_string(c) + ".idx"; }

bool FileExists(const std::string& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0;
}

void MkdirP(const std::string& p) {
  if (::mkdir(p.c_str(), 0775) != 0 && errno != EEXIST) Die("cannot create " + p + ": " + std::strerror(errno));
}

u64 FreeBytes(const std::string& root) {
  struct statvfs s {};
  if (::statvfs(root.c_str(), &s) != 0) return 0;
  return u64(s.f_bavail) * u64(s.f_frsize);
}

void FsyncDir(const std::string& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

// Writes everything or reports failure; a full or failing disk must not kill the run
// when another root can take the file.
bool WriteAll(int fd, const char* p, u64 n) {
  while (n) {
    const ssize_t got = ::write(fd, p, std::min<u64>(n, u64(1) << 27));
    if (got <= 0) {
      if (got < 0 && errno == EINTR) continue;
      return false;
    }
    p += got;
    n -= u64(got);
  }
  return true;
}

void ReadAllAt(int fd, char* p, u64 n, u64 off, const std::string& what) {
  while (n) {
    const ssize_t got = ::pread(fd, p, std::min<u64>(n, u64(1) << 27), off_t(off));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) Die("read failed on " + what + ": " + (got == 0 ? std::string("unexpected end of file") : std::strerror(errno)));
    p += got;
    n -= u64(got);
    off += u64(got);
  }
}

struct ClassIndex {
  bool present = false;
  int root = -1;
  u64 loss_raw = 0;
  u64 data_hash = 0;
  u64 data_bytes = 0;
  std::vector<u64> tile_off;                // D + 1
  std::vector<std::vector<u32>> frame_len;  // tile -> frame -> compressed bytes
};

const u64 kIndexMagic = 0x31544C424D5A4D41ULL;  // "AMZMBLT1"

std::string SerializeIndex(const ClassIndex& ci, int k, int c) {
  std::string s;
  auto put = [&](u64 v) { s.append(reinterpret_cast<const char*>(&v), 8); };
  put(kIndexMagic);
  put(u64(W));
  put(u64(H));
  put(u64(Q));
  put(u64(k));
  put(u64(D));
  put(u64(c));
  put(g_canon_max ? 1 : 0);
  put(ci.loss_raw);
  put(ci.data_hash);
  put(ci.data_bytes);
  for (int d = 0; d <= D; ++d) put(ci.tile_off[size_t(d)]);
  for (int d = 0; d < D; ++d) {
    const auto& fl = ci.frame_len[size_t(d)];
    put(fl.size());
    s.append(reinterpret_cast<const char*>(fl.data()), fl.size() * 4);
  }
  const u64 h = HashWords(0x1234567, s.data(), s.size());
  put(h);
  return s;
}

bool ParseIndex(const std::string& path, int k, int c, ClassIndex* ci) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string s = ss.str();
  if (s.size() < 8 * 12) Die("short index " + path);
  size_t pos = 0;
  auto get = [&]() {
    if (pos + 8 > s.size()) Die("truncated index " + path);
    u64 v;
    std::memcpy(&v, s.data() + pos, 8);
    pos += 8;
    return v;
  };
  u64 stored;
  std::memcpy(&stored, s.data() + s.size() - 8, 8);
  if (HashWords(0x1234567, s.data(), s.size() - 8) != stored) Die("corrupt index " + path);
  if (get() != kIndexMagic) Die("bad magic in " + path);
  const u64 w = get(), h = get(), q = get(), kk = get(), dd = get(), cc = get(), rev = get();
  if (w != u64(W) || h != u64(H) || q != u64(Q) || kk != u64(k) || dd != u64(D) || cc != u64(c) || rev != u64(g_canon_max)) {
    Die("index " + path + " belongs to another configuration (D=" + std::to_string(dd) + ", canon=" + std::to_string(rev) + ")");
  }
  ci->loss_raw = get();
  ci->data_hash = get();
  ci->data_bytes = get();
  ci->tile_off.resize(size_t(D) + 1);
  for (int d = 0; d <= D; ++d) ci->tile_off[size_t(d)] = get();
  ci->frame_len.resize(size_t(D));
  for (int d = 0; d < D; ++d) {
    const u64 n = get();
    auto& fl = ci->frame_len[size_t(d)];
    fl.resize(n);
    if (pos + n * 4 > s.size()) Die("truncated index " + path);
    std::memcpy(fl.data(), s.data() + pos, n * 4);
    pos += n * 4;
  }
  ci->present = true;
  return true;
}

// All the classes of one layer on disk.
struct LayerFiles {
  int k = -1;
  std::vector<ClassIndex> cls;
  std::vector<int> fds;
  void Load(int layer) {
    k = layer;
    cls.assign(size_t(D), ClassIndex());
    fds.assign(size_t(D), -1);
    for (int c = 0; c < D; ++c) {
      for (size_t r = 0; r < g_roots.size(); ++r) {
        if (ParseIndex(IndexPath(g_roots[r], k, c), k, c, &cls[size_t(c)])) {
          cls[size_t(c)].root = int(r);
          break;
        }
      }
    }
  }
  bool Complete() const {
    for (const auto& c : cls) {
      if (!c.present) return false;
    }
    return !cls.empty();
  }
  void Open() {
    for (int c = 0; c < D; ++c) {
      if (fds[size_t(c)] >= 0 || !cls[size_t(c)].present) continue;
      const std::string p = DataPath(g_roots[size_t(cls[size_t(c)].root)], k, c);
      const int fd = ::open(p.c_str(), O_RDONLY);
      if (fd < 0) Die("cannot open " + p + ": " + std::strerror(errno));
      fds[size_t(c)] = fd;
    }
  }
  void Close() {
    for (auto& fd : fds) {
      if (fd >= 0) ::close(fd);
      fd = -1;
    }
  }
  u64 TotalBytes() const {
    u64 t = 0;
    for (const auto& c : cls) t += c.data_bytes;
    return t;
  }
};

thread_local ZSTD_CCtx* tl_cctx = nullptr;
thread_local ZSTD_DCtx* tl_dctx = nullptr;
int g_level = 3;
int g_level_keep = 0;  // zero means the same as g_level

enum KeepPolicy { kKeepAll, kKeepParity, kKeepNone };
KeepPolicy g_keep = kKeepAll;

bool KeepLayer(int k) {
  if (g_keep == kKeepAll) return true;
  if (g_keep == kKeepNone) return false;
  return k == 0 || ((F - k) & 1) != 0;
}

inline int LevelFor(int k) { return (g_level_keep && KeepLayer(k)) ? g_level_keep : g_level; }

size_t CompressFrame(char* dst, size_t cap, const char* src, size_t n, int level) {
  if (!tl_cctx) {
    tl_cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(tl_cctx, ZSTD_c_checksumFlag, 1);
  }
  ZSTD_CCtx_setParameter(tl_cctx, ZSTD_c_compressionLevel, level);
  const size_t r = ZSTD_compress2(tl_cctx, dst, cap, src, n);
  if (ZSTD_isError(r)) Die(std::string("zstd compress: ") + ZSTD_getErrorName(r));
  return r;
}

void DecompressFrame(char* dst, size_t raw, const char* src, size_t n, const std::string& what) {
  if (!tl_dctx) tl_dctx = ZSTD_createDCtx();
  const size_t r = ZSTD_decompressDCtx(tl_dctx, dst, raw, src, n);
  if (ZSTD_isError(r)) Die("corrupt data in " + what + ": " + ZSTD_getErrorName(r));
  if (r != raw) Die("short frame in " + what);
}

struct IoStats {
  std::atomic<u64> read_bytes{0}, written_bytes{0}, raw_written{0};
  std::mutex m;
  double read_secs = 0, write_secs = 0, compress_secs = 0;
};
IoStats g_io;

// Reads tile d of every class of a layer into memory: the rows of class d.
void ReadRows(const LayerFiles& lf, int d, char* inbuf, const std::vector<u64>& base, TaskPool& aux) {
  const Geom g = GeomOf(lf.k);
  const double t0 = Now();
  Latch latch;
  ByteGate gate(u64(16) << 30);
  for (int c = 0; c < D; ++c) {
    const ClassIndex& ci = lf.cls[size_t(c)];
    const u64 off = ci.tile_off[size_t(d)], bytes = ci.tile_off[size_t(d) + 1] - off;
    if (!bytes) continue;
    gate.Acquire(bytes);
    auto buf = std::shared_ptr<char>(static_cast<char*>(std::malloc(bytes)), std::free);
    if (!buf) Die("out of memory reading a tile");
    const std::string what = "layer " + std::to_string(lf.k) + " class " + std::to_string(c) + " tile " + std::to_string(d);
    ReadAllAt(lf.fds[size_t(c)], buf.get(), bytes, off, what);
    ::posix_fadvise(lf.fds[size_t(c)], off_t(off), off_t(bytes), POSIX_FADV_DONTNEED);
    g_io.read_bytes.fetch_add(bytes);
    const u64 slabs = g_tile_slabs[size_t(c) * size_t(D) + size_t(d)];
    u64 at = 0;
    const auto& fl = ci.frame_len[size_t(d)];
    auto left = std::make_shared<std::atomic<int>>(int(fl.size()));
    for (size_t f = 0; f < fl.size(); ++f) {
      const u64 first = u64(f) * g.chunk_slabs;
      const u64 count = std::min(slabs - first, g.chunk_slabs);
      char* dst = inbuf + base[size_t(c)] + first * g.slab_bytes;
      const char* src = buf.get() + at;
      const size_t len = fl[f];
      at += len;
      latch.Add();
      // Each task holds the buffer, so it lives until the last frame is done.
      aux.Submit([=, &latch, &gate, keep = buf] {
        DecompressFrame(dst, count * g.slab_bytes, src, len, what);
        if (left->fetch_sub(1) == 1) gate.Release(bytes);
        latch.Done();
      });
    }
  }
  latch.Wait();
  std::lock_guard<std::mutex> l(g_io.m);
  g_io.read_secs += Now() - t0;
}

// Compresses the columns of class c into tiles and writes them as one file.
// outbuf holds rows_in_class * SPR slabs.
void WriteClass(int k, int c, const char* outbuf, u64 loss_raw, TaskPool& aux, ClassIndex* result) {
  const Geom g = GeomOf(k);
  const double t0 = Now();
  const u64 rb = g_class_begin[size_t(c)], re = g_class_begin[size_t(c) + 1];
  struct Tile {
    std::vector<u32> where;  // slab position in outbuf
    std::vector<std::vector<char>> frames;
  };
  std::vector<Tile> tiles(static_cast<size_t>(D));
  for (int d = 0; d < D; ++d) tiles[size_t(d)].where.reserve(g_tile_slabs[size_t(c) * size_t(D) + size_t(d)]);
  for (u64 y = rb; y < re; ++y) {
    const u64 rest = ~g_rows[y] & BOARD;
    for (u64 j = 0; j < SPR; ++j) {
      const int d = g_set_class[RankSubset(_pdep_u64(g_qsub[j], rest))];
      tiles[size_t(d)].where.push_back(u32((y - rb) * SPR + j));
    }
  }
  // One task per frame, so a single large tile still uses every thread.
  const int level = LevelFor(k);
  Latch latch;
  std::atomic<u64> raw_total{0};
  for (int d = 0; d < D; ++d) {
    Tile& t = tiles[size_t(d)];
    const size_t nframes = (t.where.size() + g.chunk_slabs - 1) / g.chunk_slabs;
    t.frames.resize(nframes);
    for (size_t f = 0; f < nframes; ++f) {
      latch.Add();
      aux.Submit([&, f] {
        thread_local std::vector<char> chunk, comp;
        const size_t raw_chunk = size_t(g.chunk_slabs * g.slab_bytes);
        const size_t cap = ZSTD_compressBound(raw_chunk);
        if (chunk.size() < raw_chunk) chunk.resize(raw_chunk);
        if (comp.size() < cap) comp.resize(cap);
        const size_t first = f * g.chunk_slabs;
        const size_t n = std::min<size_t>(g.chunk_slabs, t.where.size() - first);
        for (size_t s = 0; s < n; ++s) std::memcpy(chunk.data() + s * g.slab_bytes, outbuf + u64(t.where[first + s]) * g.slab_bytes, g.slab_bytes);
        const size_t got = CompressFrame(comp.data(), cap, chunk.data(), n * g.slab_bytes, level);
        t.frames[f].assign(comp.data(), comp.data() + got);
        raw_total.fetch_add(n * g.slab_bytes);
        latch.Done();
      });
    }
  }
  latch.Wait();
  const double t1 = Now();
  ClassIndex ci;
  ci.tile_off.assign(size_t(D) + 1, 0);
  ci.frame_len.resize(size_t(D));
  u64 total = 0;
  StreamHash hash;
  for (int d = 0; d < D; ++d) {
    ci.tile_off[size_t(d)] = total;
    for (const auto& fr : tiles[size_t(d)].frames) {
      total += fr.size();
      ci.frame_len[size_t(d)].push_back(u32(fr.size()));
      hash.Update(fr.data(), fr.size());
    }
  }
  ci.tile_off[size_t(D)] = total;
  ci.loss_raw = loss_raw;
  ci.data_hash = hash.Final();
  ci.data_bytes = total;

  // First root with room.  A root that fails is skipped, so a disk going away does not
  // stop the run while another can take the file.
  bool stored = false;
  for (size_t r = 0; r < g_roots.size() && !stored; ++r) {
    const std::string& root = g_roots[r];
    if (FreeBytes(root) < total + g_reserve) continue;
    // A kept layer ends up on the last root, so from a middle root it would only have to
    // move again.  The solver reads it once more, which checks every frame.
    if (r > 0 && r + 1 < g_roots.size() && g_keep != kKeepNone && KeepLayer(k) && FreeBytes(g_roots.back()) >= total + g_reserve) continue;
    const std::string dir = LayerDir(root, k);
    if (::mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "slabsolve: warning: cannot create %s: %s\n", dir.c_str(), std::strerror(errno));
      continue;
    }
    const std::string data_path = DataPath(root, k, c), idx_path = IndexPath(root, k, c);
    const std::string data_tmp = data_path + ".tmp", idx_tmp = idx_path + ".tmp";
    int fd = ::open(data_tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
    bool ok = fd >= 0;
    for (int d = 0; d < D && ok; ++d) {
      for (const auto& fr : tiles[size_t(d)].frames) {
        ok = ok && WriteAll(fd, fr.data(), fr.size());
      }
    }
    if (ok) ok = ::fdatasync(fd) == 0;
    if (fd >= 0) {
      if (ok) ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
      ::close(fd);
    }
    if (ok) {
      const std::string s = SerializeIndex(ci, k, c);
      fd = ::open(idx_tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
      ok = fd >= 0 && WriteAll(fd, s.data(), s.size()) && ::fsync(fd) == 0;
      if (fd >= 0) ::close(fd);
    }
    // The data file appears first: an index means the data is complete.
    if (ok) ok = ::rename(data_tmp.c_str(), data_path.c_str()) == 0;
    if (ok) ok = ::rename(idx_tmp.c_str(), idx_path.c_str()) == 0;
    if (ok) {
      FsyncDir(dir);
      ci.root = int(r);
      ci.present = true;
      stored = true;
    } else {
      std::fprintf(stderr, "slabsolve: warning: writing layer %d class %d to %s failed: %s\n", k, c, root.c_str(), std::strerror(errno));
      std::fflush(stderr);
      ::unlink(data_tmp.c_str());
      ::unlink(idx_tmp.c_str());
    }
  }
  if (!stored) {
    Die("no root can take layer " + std::to_string(k) + " class " + std::to_string(c) + " (" + std::to_string(total) +
        " bytes).  Make room or add a root, and rerun with the same arguments to resume.");
  }
  g_io.written_bytes.fetch_add(total);
  g_io.raw_written.fetch_add(raw_total.load());
  {
    std::lock_guard<std::mutex> l(g_io.m);
    g_io.compress_secs += t1 - t0;
    g_io.write_secs += Now() - t1;
  }
  *result = std::move(ci);
}

// Random access to the files of a layer, for verification and probes.
class SlabReader {
 public:
  explicit SlabReader(LayerFiles* lf) : lf_(lf), g_(GeomOf(lf->k)) {
    frame_off_.resize(size_t(D));
    for (int c = 0; c < D; ++c) {
      frame_off_[size_t(c)].resize(size_t(D));
      const auto& ci = lf_->cls[size_t(c)];
      for (int d = 0; d < D; ++d) {
        auto& v = frame_off_[size_t(c)][size_t(d)];
        u64 at = ci.tile_off[size_t(d)];
        for (const u32 len : ci.frame_len[size_t(d)]) {
          v.push_back(at);
          at += len;
        }
        v.push_back(at);
      }
    }
  }
  // The mover loses (m, o, a), with arrows a of the layer's popcount.
  bool Loss(u64 m, u64 o, u64 a) {
    const u32 e = g_rowmap[RankSubset(o)];
    const int g = int(e & 7);
    const u64 col = e >> 3;
    const u64 gm = Transform(g, m), go = Transform(g, o);
    const u64 free_cells = ~(gm | go) & BOARD;
    const u64 r = g_rank[_pext_u64(Transform(g, a), free_cells)];
    const u64 j = RankIn(gm, go);
    const int c = ClassOfRow(col), d = g_set_class[RankSubset(gm)];
    const u64 slab = u64(g_colstart[col * u64(D) + u64(d)]) + g_ordinal[col * SPR + j];
    const u64 frame = slab / g_.chunk_slabs;
    const char* raw = Chunk(c, d, frame);
    const u64 w = (slab % g_.chunk_slabs) * g_.stride + (r >> 6);
    u64 word;
    std::memcpy(&word, raw + w * 8, 8);
    return (word >> (r & 63)) & 1;
  }

 private:
  const char* Chunk(int c, int d, u64 frame) {
    const u64 key = (u64(c) << 44) | (u64(d) << 28) | frame;
    for (auto& e : cache_) {
      if (e.key == key) {
        e.used = ++tick_;
        return e.raw.data();
      }
    }
    Entry* v = nullptr;
    if (cache_.size() < 8) {
      cache_.emplace_back();
      v = &cache_.back();
    } else {
      v = &cache_[0];
      for (auto& e : cache_) {
        if (e.used < v->used) v = &e;
      }
    }
    const u64 slabs = g_tile_slabs[size_t(c) * size_t(D) + size_t(d)];
    const u64 count = std::min(slabs - frame * g_.chunk_slabs, g_.chunk_slabs);
    const auto& off = frame_off_[size_t(c)][size_t(d)];
    const u64 len = off[frame + 1] - off[frame];
    comp_.resize(len);
    const std::string what = "layer " + std::to_string(lf_->k) + " class " + std::to_string(c);
    ReadAllAt(lf_->fds[size_t(c)], comp_.data(), len, off[frame], what);
    v->raw.resize(count * g_.slab_bytes);
    DecompressFrame(v->raw.data(), v->raw.size(), comp_.data(), len, what);
    v->key = key;
    v->used = ++tick_;
    return v->raw.data();
  }
  struct Entry {
    u64 key = ~u64(0), used = 0;
    std::vector<char> raw;
  };
  LayerFiles* lf_;
  Geom g_;
  std::vector<std::vector<std::vector<u64>>> frame_off_;
  std::deque<Entry> cache_;
  std::vector<char> comp_;
  u64 tick_ = 0;
};

// ------------------------------------------------------------------ rules

// Walks the rules cell by cell, for checks that must not share code with the kernel.
template <class LossFn>
bool ExplicitWin(u64 m, u64 o, u64 a, LossFn&& child_loss) {
  const u64 occ = m | o | a;
  u64 mm = m;
  while (mm) {
    const int f = __builtin_ctzll(mm);
    mm &= mm - 1;
    int fx, fy;
    CellXY(f, &fx, &fy);
    for (int d = 0; d < 8; ++d) {
      for (int x = fx + DX[d], y = fy + DY[d]; x >= 0 && x < W && y >= 0 && y < H; x += DX[d], y += DY[d]) {
        const int t = CellId(x, y);
        if ((occ >> t) & 1) break;
        const u64 moved = (m & ~(u64(1) << f)) | (u64(1) << t);
        const u64 occ2 = (occ & ~(u64(1) << f)) | (u64(1) << t);
        for (int d2 = 0; d2 < 8; ++d2) {
          for (int ax = x + DX[d2], ay = y + DY[d2]; ax >= 0 && ax < W && ay >= 0 && ay < H; ax += DX[d2], ay += DY[d2]) {
            const int s = CellId(ax, ay);
            if ((occ2 >> s) & 1) break;
            if (child_loss(o, moved, a | (u64(1) << s))) return true;
          }
        }
      }
    }
  }
  return false;
}

void StartPosition(u64* m, u64* o) {
  auto bit = [&](int x, int y) { return u64(1) << CellId(x, y); };
  *m = bit(1, 0) | bit(W - 2, 0) | bit(0, 1) | bit(W - 1, 1);
  *o = bit(0, H - 2) | bit(W - 1, H - 2) | bit(1, H - 1) | bit(W - 2, H - 1);
}

// The standard start of the HxW board, written on this board by transposing coordinates.
void StartPositionTransposed(u64* m, u64* o) {
  auto bit = [&](int x, int y) { return u64(1) << CellId(y, x); };
  *m = bit(1, 0) | bit(H - 2, 0) | bit(0, 1) | bit(H - 1, 1);
  *o = bit(0, W - 2) | bit(H - 1, W - 2) | bit(1, W - 1) | bit(H - 2, W - 1);
}

// ---------------------------------------------------------------- workers

struct CpuTopology {
  std::vector<std::vector<int>> primary;    // node -> first hardware thread of each core
  std::vector<std::vector<int>> secondary;  // node -> other hardware threads
};

std::vector<int> ParseCpuList(const std::string& s) {
  std::vector<int> out;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ',')) {
    int a = 0, b = 0;
    const int n = std::sscanf(part.c_str(), "%d-%d", &a, &b);
    if (n == 1) out.push_back(a);
    else if (n == 2)
      for (int i = a; i <= b; ++i) out.push_back(i);
  }
  return out;
}

std::string ReadLine(const std::string& path) {
  std::ifstream in(path);
  std::string s;
  std::getline(in, s);
  return s;
}

CpuTopology ReadTopology() {
  CpuTopology t;
  for (int node = 0; node < 64; ++node) {
    const std::string list = ReadLine("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist");
    if (list.empty()) break;
    std::vector<int> p, s;
    std::set<int> seen;
    for (const int cpu : ParseCpuList(list)) {
      const auto sib = ParseCpuList(ReadLine("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list"));
      const int first = sib.empty() ? cpu : sib[0];
      if (first == cpu) p.push_back(cpu);
      else s.push_back(cpu);
    }
    t.primary.push_back(p);
    t.secondary.push_back(s);
  }
  if (t.primary.empty()) {
    std::vector<int> all;
    for (unsigned c = 0; c < std::thread::hardware_concurrency(); ++c) all.push_back(int(c));
    t.primary.push_back(all);
    t.secondary.emplace_back();
  }
  return t;
}

// Everything that is not kernel work runs on the other hardware thread of each core, so
// compression and I/O never preempt a kernel thread.  New threads inherit this.
void MoveCallerToSecondaryThreads() {
  const CpuTopology topo = ReadTopology();
  cpu_set_t s;
  CPU_ZERO(&s);
  int n = 0;
  for (const auto& list : topo.secondary) {
    for (const int c : list) {
      CPU_SET(c, &s);
      ++n;
    }
  }
  if (n) ::sched_setaffinity(0, sizeof(s), &s);
}

// One NUMA node's kernel threads and row buffers.  Memory is touched by the node's own
// threads, so first touch makes it local.
struct Worker {
  std::unique_ptr<Pool> pool;
  V* child = nullptr;
  V* shots = nullptr;
  V* scratch = nullptr;  // 3 slabs per thread: shots, parent, symmetry
  size_t child_vs = 0, shot_vs = 0;
  std::vector<std::unique_ptr<PermScratch>> perm;
  double t_gather = 0, t_shots = 0, t_parents = 0;

  void Init(const std::vector<int>& cpus) {
    pool = std::make_unique<Pool>(cpus);
    child_vs = size_t(SPR) * WORDS;
    shot_vs = child_vs * size_t(Q);
    child = BigAlloc<V>(child_vs);
    shots = BigAlloc<V>(shot_vs);
    scratch = BigAlloc<V>(size_t(pool->size()) * 3 * WORDS);
    for (int t = 0; t < pool->size(); ++t) perm.emplace_back(new PermScratch());
  }
  V* Scratch(int tid, int which) { return scratch + (size_t(tid) * 3 + size_t(which)) * WORDS; }
  // The tables are zero outside the live words of a layer; this restores that.
  void ClearTables() {
    PinCaller(pool->cpu0());
    const size_t total = child_vs + shot_vs;
    const size_t step = size_t(64) << 20;
    pool->For(total / (step / sizeof(V)) + 1, 1, [&](size_t b, int) {
      const size_t from = b * (step / sizeof(V));
      for (size_t i = from; i < std::min(total, from + step / sizeof(V)); i += 1) {
        V* p = i < child_vs ? child + i : shots + (i - child_vs);
        _mm512_stream_si512(p, _mm512_setzero_si512());
      }
    });
  }
};

std::vector<std::unique_ptr<Worker>> g_workers;

void InitWorkers(int total_threads, int workers) {
  const CpuTopology topo = ReadTopology();
  const int nodes = int(topo.primary.size());
  if (workers <= 0) workers = nodes;
  if (total_threads <= 0) {
    total_threads = 0;
    for (const auto& p : topo.primary) total_threads += int(p.size());
  }
  for (int w = 0; w < workers; ++w) {
    const int node = w % nodes;
    std::vector<int> cpus = topo.primary[size_t(node)];
    cpus.insert(cpus.end(), topo.secondary[size_t(node)].begin(), topo.secondary[size_t(node)].end());
    int want = total_threads / workers + (w < total_threads % workers ? 1 : 0);
    if (want < 1) want = 1;
    if (size_t(want) < cpus.size()) cpus.resize(size_t(want));
    auto wk = std::make_unique<Worker>();
    std::thread init([&] {
      PinCaller(cpus[0]);
      wk->Init(cpus);
      wk->ClearTables();
    });
    init.join();
    g_workers.push_back(std::move(wk));
  }
}

// ------------------------------------------------------------------ solve

struct KernelContext {
  int k;
  const char* inbuf;
  const std::vector<u64>* in_base;
  char* outbuf;
  u64 class_begin;
  int check_every;
  std::atomic<u64>* loss_raw;
  std::atomic<u64>* checked;
};

// Relabels a few positions of a finished parent slab from the rules, reading the child
// slabs directly.  This sees everything except the file path.
void CheckSlab(u64 x, u64 y, int k, const V* w, const V* child, std::mt19937_64& rng, std::atomic<u64>* checked) {
  const u64 fr = ~(x | y) & BOARD;
  const auto& un = g_unrank[size_t(k)];
  const u64* wl = reinterpret_cast<const u64*>(w);
  for (int n = 0; n < 4; ++n) {
    const u32 ai = un[rng() % un.size()];
    const u64 a = _pdep_u64(ai, fr);
    const bool got = (wl[ai >> 6] >> (ai & 63)) & 1;
    const bool want = ExplicitWin(x, y, a, [&](u64 mo, u64 op, u64 ar) {
      const u64 cf = ~(mo | op) & BOARD;
      const u64 idx = _pext_u64(ar, cf);
      const u64* c = reinterpret_cast<const u64*>(child + RankIn(op, mo) * WORDS);
      return bool((c[idx >> 6] >> (idx & 63)) & 1);
    });
    if (got != want) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "kernel check failed: k=%d x=%" PRIx64 " y=%" PRIx64 " a=%" PRIx64 " kernel=%d rules=%d", k, x, y, a, int(got), int(want));
      Die(buf);
    }
    checked->fetch_add(1, std::memory_order_relaxed);
  }
}

void ProcessRow(Worker& wk, u64 yi, const KernelContext& cx, size_t chunk) {
  const int k = cx.k;
  const Geom gn = GeomOf(k + 1), gc = GeomOf(k);
  const u64 y = g_rows[yi];
  const u64 rest = ~y & BOARD;
  const int dclass = ClassOfRow(yi);
  Pool& pool = *wk.pool;
  double t1 = Now();
  pool.For(size_t(SPR), chunk, [&](size_t j, int tid) {
    const u64 xp = _pdep_u64(g_qsub[j], rest);
    const u32 e = g_rowmap[RankSubset(xp)];
    const int g = int(e & 7);
    const u64 col = e >> 3;
    const u64 gy = Transform(g, y), gx = Transform(g, xp);
    const u64 sj = RankIn(gy, gx);
    const u64 slab = u64(g_colstart[col * u64(D) + u64(dclass)]) + g_ordinal[col * SPR + sj];
    const u64* src = reinterpret_cast<const u64*>(cx.inbuf + (*cx.in_base)[size_t(ClassOfRow(col))] + slab * gn.slab_bytes);
    V* out = wk.child + j * WORDS;
    if (g == 0) {
      ExpandSlabStream(src, k + 1, out);
    } else {
      ExpandSlabVia(src, k + 1, ~(gy | gx) & BOARD, ~(y | xp) & BOARD, g, wk.Scratch(tid, 2), out, *wk.perm[size_t(tid)]);
    }
  });
  double t2 = Now();
  wk.t_gather += t2 - t1;
  pool.For(size_t(SPR), chunk, [&](size_t j, int tid) {
    const u64 xp = _pdep_u64(g_qsub[j], rest);
    BuildShots(y, xp, wk.child + j * WORDS, wk.shots + j * size_t(Q) * WORDS, wk.Scratch(tid, 0));
  });
  t1 = Now();
  wk.t_shots += t1 - t2;
  const u64 orbit = u64(g_orbit[yi]);
  const u64 row_local = yi - cx.class_begin;
  pool.For(size_t(SPR), chunk, [&](size_t j, int tid) {
    const u64 x = _pdep_u64(g_qsub[j], rest);
    V* w = wk.Scratch(tid, 1);
    SolveParent(x, y, wk.shots, w);
    u64* dst = reinterpret_cast<u64*>(cx.outbuf + (row_local * SPR + j) * gc.slab_bytes);
    if (cx.check_every && ((yi * 2654435761ULL + j * 40503ULL + u64(k)) % u64(cx.check_every)) == 0) {
      std::mt19937_64 rng(yi * 1000003ULL + j * 31ULL + u64(k));
      CheckSlab(x, y, k, w, wk.child, rng, cx.checked);
    }
    const u64 losses = CompactLosses(w, k, dst);
    cx.loss_raw->fetch_add(losses * orbit, std::memory_order_relaxed);
  });
  wk.t_parents += Now() - t1;
}

// Column table of one layer in memory, for --compare.
std::unique_ptr<u64[]> LoadWholeLayer(LayerFiles& lf) {
  const Geom g = GeomOf(lf.k);
  const u64 rows = g_rows.size();
  std::unique_ptr<u64[]> table(new u64[size_t(rows * SPR * g.stride)]);
  std::vector<std::thread> pool;
  std::atomic<int> next{0};
  for (int t = 0; t < 16; ++t) {
    pool.emplace_back([&] {
      std::vector<char> comp, raw;
      for (;;) {
        const int c = next.fetch_add(1);
        if (c >= D) break;
        const auto& ci = lf.cls[size_t(c)];
        for (int d = 0; d < D; ++d) {
          const u64 slabs = g_tile_slabs[size_t(c) * size_t(D) + size_t(d)];
          raw.resize(slabs * g.slab_bytes);
          u64 at = ci.tile_off[size_t(d)];
          const auto& fl = ci.frame_len[size_t(d)];
          for (size_t f = 0; f < fl.size(); ++f) {
            comp.resize(fl[f]);
            ReadAllAt(lf.fds[size_t(c)], comp.data(), fl[f], at, "whole layer");
            at += fl[f];
            const u64 first = u64(f) * g.chunk_slabs;
            DecompressFrame(raw.data() + first * g.slab_bytes, std::min(slabs - first, g.chunk_slabs) * g.slab_bytes, comp.data(), fl[f], "whole layer");
          }
          u64 n = 0;
          for (u64 y = g_class_begin[size_t(c)]; y < g_class_begin[size_t(c) + 1]; ++y) {
            const u64 rest = ~g_rows[y] & BOARD;
            for (u64 j = 0; j < SPR; ++j) {
              if (g_set_class[RankSubset(_pdep_u64(g_qsub[j], rest))] != d) continue;
              std::memcpy(table.get() + (y * SPR + j) * g.stride, raw.data() + n * g.slab_bytes, g.slab_bytes);
              ++n;
            }
          }
        }
      }
    });
  }
  for (auto& t : pool) t.join();
  return table;
}

// Compares a column table with a `dense` W table, position by position.  `dense` picks
// canonical slabs by mover first, so this goes through the board masks and shares no
// addressing with the kernel.
void CompareWithDense(const u64* table, int k, const std::string& dir) {
  const std::string path = dir + "/W" + std::to_string(k) + ".bits";
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) Die("cannot open " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0) Die("stat failed");
  const u64* dt = static_cast<const u64*>(::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0));
  ::close(fd);
  if (dt == MAP_FAILED) Die("mmap failed");
  const u64 stride = StrideWords(k);
  const u64 count = C[F][k];
  auto canon = [&](u64 m, u64 o) {
    for (int g = 1; g < GN; ++g) {
      const u64 gm = Transform(g, m);
      if (gm < m) return false;
      if (gm == m && Transform(g, o) < o) return false;
    }
    return true;
  };
  std::vector<u64> ms(C[N][Q]);
  u64 v = (u64(1) << Q) - 1;
  for (u64 i = 0; i < ms.size(); ++i) {
    ms[i] = v;
    if (i + 1 < ms.size()) v = NextSubset(v);
  }
  std::vector<u64> local(ms.size(), 0), base(ms.size() + 1, 0);
  {
    std::vector<std::thread> pool;
    std::atomic<size_t> next{0};
    for (int t = 0; t < 64; ++t) {
      pool.emplace_back([&] {
        for (;;) {
          const size_t i = next.fetch_add(1);
          if (i >= ms.size()) break;
          const u64 rest = ~ms[i] & BOARD;
          u64 n = 0;
          for (u64 j = 0; j < SPR; ++j) {
            if (canon(ms[i], _pdep_u64(g_qsub[j], rest))) ++n;
          }
          local[i] = n;
        }
      });
    }
    for (auto& t : pool) t.join();
  }
  for (size_t i = 0; i < ms.size(); ++i) base[i + 1] = base[i] + local[i];
  if (base.back() * stride * 8 != u64(st.st_size)) Die("unexpected size of " + path);
  std::atomic<u64> bad{0}, checked{0};
  const u32* un = g_unrank[size_t(k)].data();
  std::vector<std::thread> pool;
  std::atomic<size_t> next{0};
  for (int t = 0; t < 64; ++t) {
    pool.emplace_back([&] {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= ms.size()) break;
        const u64 m = ms[i];
        const u64 rest = ~m & BOARD;
        u64 id = base[i], n = 0;
        for (u64 j = 0; j < SPR; ++j) {
          const u64 o = _pdep_u64(g_qsub[j], rest);
          if (!canon(m, o)) continue;
          const u64* w = dt + id * stride;
          ++id;
          const u64 fr = ~(m | o) & BOARD;
          const u32 e = g_rowmap[RankSubset(o)];
          const int g = int(e & 7);
          const u64 gm = Transform(g, m), go = Transform(g, o);
          const u64 col = e >> 3;
          const u64* s = table + (col * SPR + RankIn(gm, go)) * stride;
          const u64 our_free = ~(gm | go) & BOARD;
          for (u64 r = 0; r < count; ++r) {
            const bool win = (w[r >> 6] >> (r & 63)) & 1;
            const u64 a = _pdep_u64(un[r], fr);
            const u64 rr = g_rank[_pext_u64(Transform(g, a), our_free)];
            const bool loss = (s[rr >> 6] >> (rr & 63)) & 1;
            if (win == loss) {
              if (bad.fetch_add(1) < 5) std::fprintf(stderr, "mismatch k=%d m=%" PRIx64 " o=%" PRIx64 " dense_win=%d\n", k, m, o, int(win));
            }
          }
          n += count;
        }
        checked.fetch_add(n);
      }
    });
  }
  for (auto& t : pool) t.join();
  ::munmap(const_cast<u64*>(dt), size_t(st.st_size));
  if (bad.load()) Die("layer " + std::to_string(k) + " differs from " + path + ": " + std::to_string(bad.load()) + " positions");
  std::printf("#   k=%d: all %" PRIu64 " positions of %s identical\n", k, checked.load(), path.c_str());
}

// Relabels random raw positions from the rules through the files on disk.  Together with
// CheckSlab this covers the file format, the tile addressing and the symmetry mapping.
void VerifyLayerFromFiles(LayerFiles& cur, LayerFiles* next, u64 samples, int threads) {
  const int k = cur.k;
  std::atomic<u64> bad{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      std::mt19937_64 rng(0xABCDEFULL * (u64(k) + 1) + u64(t));
      SlabReader rc(&cur);
      std::unique_ptr<SlabReader> rn;
      if (next) rn = std::make_unique<SlabReader>(next);
      std::vector<int> cells(static_cast<size_t>(N));
      for (int i = 0; i < N; ++i) cells[size_t(i)] = i;
      for (u64 it = 0; it < samples / u64(threads) + 1; ++it) {
        std::shuffle(cells.begin(), cells.end(), rng);
        u64 m = 0, o = 0, a = 0;
        for (int i = 0; i < Q; ++i) m |= u64(1) << cells[size_t(i)];
        for (int i = Q; i < 2 * Q; ++i) o |= u64(1) << cells[size_t(i)];
        for (int i = 2 * Q; i < 2 * Q + k; ++i) a |= u64(1) << cells[size_t(i)];
        const bool loss = rc.Loss(m, o, a);
        const bool win = rn ? ExplicitWin(m, o, a, [&](u64 mo, u64 op, u64 ar) { return rn->Loss(mo, op, ar); }) : false;
        if (loss == win) bad.fetch_add(1);
      }
    });
  }
  for (auto& t : pool) t.join();
  if (bad.load()) Die("file check failed on layer " + std::to_string(k) + ": " + std::to_string(bad.load()) + " positions");
}

// ------------------------------------------------------------ layer upkeep

void DeleteLayer(int k) {
  for (const std::string& root : g_roots) {
    const std::string dir = LayerDir(root, k);
    if (!FileExists(dir)) continue;
    for (int c = 0; c < D; ++c) {
      // The index goes first: without it the class no longer counts as present.
      ::unlink(IndexPath(root, k, c).c_str());
      ::unlink(DataPath(root, k, c).c_str());
      ::unlink((IndexPath(root, k, c) + ".tmp").c_str());
      ::unlink((DataPath(root, k, c) + ".tmp").c_str());
    }
    ::rmdir(dir.c_str());
  }
}

// Moves a finished layer to the last root.  Each file is read back from the destination
// and compared before the original goes.  Returns the number of classes left where they
// were (no room, or a failed copy).
int MoveLayerToLastRoot(int k) {
  if (g_roots.size() < 2) return 0;
  const std::string& cold = g_roots.back();
  LayerFiles lf;
  lf.Load(k);
  std::vector<char> buf(size_t(64) << 20);
  bool warned = false;
  int stayed = 0;
  for (int c = 0; c < D; ++c) {
    const ClassIndex& ci = lf.cls[size_t(c)];
    if (!ci.present || ci.root == int(g_roots.size()) - 1) continue;
    const std::string& hot = g_roots[size_t(ci.root)];
    if (FreeBytes(cold) < ci.data_bytes + g_reserve) {
      if (!warned) {
        std::fprintf(stderr, "slabsolve: warning: %s is full; layer %d stays where it is\n", cold.c_str(), k);
        warned = true;
      }
      ++stayed;
      continue;
    }
    if (::mkdir(LayerDir(cold, k).c_str(), 0775) != 0 && errno != EEXIST) {
      if (!warned) {
        std::fprintf(stderr, "slabsolve: warning: cannot create %s (%s); layer %d stays where it is\n", LayerDir(cold, k).c_str(), std::strerror(errno), k);
        warned = true;
      }
      ++stayed;
      continue;
    }
    const std::string src = DataPath(hot, k, c), dst = DataPath(cold, k, c), tmp = dst + ".tmp";
    const int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) Die("cannot open " + src + ": " + std::strerror(errno));
    const int out = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0664);
    bool ok = out >= 0;
    u64 left = ci.data_bytes, off = 0;
    StreamHash h1, h2;
    while (ok && left) {
      const u64 n = std::min<u64>(left, buf.size());
      ReadAllAt(in, buf.data(), n, off, src);
      // A reader may be using this data now, so the source stays in the page cache only
      // as long as it is needed.
      ::posix_fadvise(in, off_t(off), off_t(n), POSIX_FADV_DONTNEED);
      h1.Update(buf.data(), n);
      ok = WriteAll(out, buf.data(), n);
      left -= n;
      off += n;
    }
    ::close(in);
    if (ok) ok = ::fdatasync(out) == 0;
    if (ok) {
      ::posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
      left = ci.data_bytes;
      off = 0;
      while (ok && left) {
        const u64 n = std::min<u64>(left, buf.size());
        u64 have = 0;
        while (have < n) {
          const ssize_t got = ::pread(out, buf.data() + have, n - have, off_t(off + have));
          if (got < 0 && errno == EINTR) continue;
          if (got <= 0) {
            ok = false;
            break;
          }
          have += u64(got);
        }
        if (!ok) break;
        h2.Update(buf.data(), n);
        left -= n;
        off += n;
      }
      ::posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
    }
    if (out >= 0) ::close(out);
    if (ok && (h1.Final() != ci.data_hash || h2.Final() != ci.data_hash)) {
      Die("copy of " + src + " does not match its index; the original is untouched");
    }
    if (ok) ok = ::rename(tmp.c_str(), dst.c_str()) == 0;
    if (ok) {
      const std::string s = SerializeIndex(ci, k, c);
      const std::string itmp = IndexPath(cold, k, c) + ".tmp";
      const int fd = ::open(itmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
      ok = fd >= 0 && WriteAll(fd, s.data(), s.size()) && ::fsync(fd) == 0;
      if (fd >= 0) ::close(fd);
      if (ok) ok = ::rename(itmp.c_str(), IndexPath(cold, k, c).c_str()) == 0;
    }
    if (!ok) {
      std::fprintf(stderr, "slabsolve: warning: moving %s to %s failed (%s); keeping the original\n", src.c_str(), cold.c_str(), std::strerror(errno));
      std::fflush(stderr);
      ::unlink(tmp.c_str());
      ++stayed;
      continue;
    }
    FsyncDir(LayerDir(cold, k));
    ::unlink(IndexPath(hot, k, c).c_str());
    ::unlink(src.c_str());
  }
  for (size_t r = 0; r + 1 < g_roots.size(); ++r) ::rmdir(LayerDir(g_roots[r], k).c_str());
  return stayed;
}

// Runs layer moves one at a time behind the solver.
class Mover {
 public:
  Mover() {
    th_ = std::thread([this] {
      for (;;) {
        int k;
        {
          std::unique_lock<std::mutex> l(m_);
          cv_.wait(l, [&] { return stop_ || !q_.empty(); });
          if (q_.empty()) return;
          k = q_.front();
          q_.pop_front();
        }
        const double t0 = Now();
        const int stayed = MoveLayerToLastRoot(k);
        if (stayed == 0) {
          std::printf("#   layer %d moved to %s in %.0fs\n", k, g_roots.back().c_str(), Now() - t0);
        } else {
          std::printf("#   layer %d: %d of %d classes left where they were (%.0fs)\n", k, stayed, D, Now() - t0);
        }
      }
    });
  }
  ~Mover() { Finish(); }
  void Finish() {
    {
      std::lock_guard<std::mutex> l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }
  void Enqueue(int k) {
    std::lock_guard<std::mutex> l(m_);
    q_.push_back(k);
    cv_.notify_one();
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<int> q_;
  bool stop_ = false;
  std::thread th_;
};

struct LayerResult {
  int k = -1;
  u64 positions = 0, wins = 0, losses = 0, bytes = 0, raw_bytes = 0;
  double secs = 0;
};

std::string ProgressPath() { return g_roots[0] + "/progress.csv"; }

std::vector<LayerResult> LoadProgress() {
  std::vector<LayerResult> out;
  std::ifstream in(ProgressPath());
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == 'k') continue;
    LayerResult r;
    if (std::sscanf(line.c_str(), "%d,%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%" SCNu64 ",%lf", &r.k, &r.positions, &r.wins, &r.losses, &r.bytes, &r.raw_bytes, &r.secs) == 7) {
      out.push_back(r);
    }
  }
  return out;
}

void AppendProgress(const LayerResult& r) {
  const std::string path = ProgressPath();
  const bool fresh = !FileExists(path);
  FILE* f = std::fopen(path.c_str(), "a");
  if (!f) Die("cannot write " + path);
  if (fresh) std::fprintf(f, "k,positions,wins,losses,compressed_bytes,raw_bytes,secs\n");
  std::fprintf(f, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.1f\n", r.k, r.positions, r.wins, r.losses, r.bytes, r.raw_bytes, r.secs);
  std::fflush(f);
  ::fsync(fileno(f));
  std::fclose(f);
}

void WriteConfig() {
  const std::string path = g_roots[0] + "/config.txt";
  if (FileExists(path)) return;
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) Die("cannot write " + path);
  std::fprintf(f, "board %dx%d\nqueens %d\nclasses %d\ncanon %s\nlevel %d\nchunk_raw %" PRIu64 "\n", W, H, Q, D, g_canon_max ? "max" : "min", g_level, kChunkRaw);
  std::fclose(f);
}

int CmdSolve(const std::string& compare_dir, int kmin, u64 verify_samples, int aux_threads, int check_every) {
  BuildRows();
  BuildArrowRanks();
  InitLaneMasks();
  const u64 rows = g_rows.size();
  double peak_bytes = 0, total_bytes = 0;
  for (int k = 0; k <= F; ++k) {
    const double b = double(rows * SPR * StrideWords(k)) * 8;
    total_bytes += b;
    peak_bytes = std::max(peak_bytes, b);
  }
  if (D <= 0) D = int(std::clamp<double>(std::ceil(peak_bytes / (32 * kGiB)), 1.0, 256.0));
  BuildTiles();
  for (const auto& r : g_roots) MkdirP(r);
  WriteConfig();
  std::printf("# %dx%d q=%d cells=%d free=%d group=%d rows=%" PRIu64 " slabs/row=%" PRIu64 " words/slab=%u classes=%d level=%d canon=%s\n", W, H, Q, N, F, GN, rows, SPR,
              WORDS, D, g_level, g_canon_max ? "max" : "min");
  std::printf("# raw table %.1f GiB in total, peak layer %.1f GiB, per class %.1f GiB\n", total_bytes / kGiB, peak_bytes / kGiB, peak_bytes / D / kGiB);
  for (const auto& r : g_roots) std::printf("# root %s: %.0f GiB free\n", r.c_str(), double(FreeBytes(r)) / kGiB);
  std::printf("# kernel: %zu workers, %d threads each; aux %d threads; row buffers %.1f GiB per worker\n", g_workers.size(), g_workers[0]->pool->size(), aux_threads,
              double(g_workers[0]->child_vs + g_workers[0]->shot_vs) * 64 / kGiB);

  TaskPool aux(aux_threads);
  Mover mover;
  auto done = LoadProgress();
  std::vector<LayerResult> results = done;
  int kstart = F;
  if (!done.empty()) {
    kstart = done.back().k - 1;
    std::printf("# resuming: layer %d is complete, continuing at %d\n", done.back().k, kstart);
    // A crash can fall between finishing a layer and retiring the one before it.
    if (done.back().k + 1 <= F) {
      if (KeepLayer(done.back().k + 1)) mover.Enqueue(done.back().k + 1);
      else DeleteLayer(done.back().k + 1);
    }
  }
  std::printf("#%3s %18s %18s %18s %10s %7s %8s %8s %8s %8s %8s %8s\n", "k", "|P_k|", "|W_k|", "|L_k|", "bytes", "ratio", "gather", "shots", "parents", "in_wait", "out_wait", "secs");

  LayerFiles next_files;
  if (kstart < F) {
    next_files.Load(kstart + 1);
    if (!next_files.Complete()) Die("layer " + std::to_string(kstart + 1) + " is recorded as complete but its files are missing");
    next_files.Open();
  }

  for (int k = kstart; k >= kmin; --k) {
    const double t0 = Now();
    const Geom gc = GeomOf(k);
    g_lq.Set(k);
    if (k < F) g_lc.Set(k + 1);
    {
      std::vector<std::thread> clear;
      for (auto& w : g_workers) {
        w->t_gather = w->t_shots = w->t_parents = 0;
        if (k < F) clear.emplace_back([&w] { w->ClearTables(); });
      }
      for (auto& t : clear) t.join();
    }
    // Classes already written, from a run that stopped mid-layer.
    LayerFiles cur_files;
    cur_files.Load(k);
    std::vector<ClassIndex>& cls = cur_files.cls;
    std::atomic<u64> loss_raw{0}, checked{0};
    for (const auto& c : cls) {
      if (c.present) loss_raw.fetch_add(c.loss_raw);
    }
    std::vector<int> todo;
    for (int c = 0; c < D; ++c) {
      if (!cls[size_t(c)].present) todo.push_back(c);
    }
    if (todo.size() < size_t(D)) std::printf("#   k=%d: %zu of %d classes already on disk\n", k, size_t(D) - todo.size(), D);

    // Buffers: the rows being used and the next ones being read; the columns being
    // written and the ones being compressed.
    u64 max_in = 0, max_rows = 0;
    std::vector<std::vector<u64>> in_base(static_cast<size_t>(D));
    if (k < F) {
      const Geom gn = GeomOf(k + 1);
      for (int d = 0; d < D; ++d) {
        auto& b = in_base[size_t(d)];
        b.resize(size_t(D) + 1);
        u64 at = 0;
        for (int c = 0; c < D; ++c) {
          b[size_t(c)] = at;
          at += g_tile_slabs[size_t(c) * size_t(D) + size_t(d)] * gn.slab_bytes;
        }
        b[size_t(D)] = at;
        max_in = std::max(max_in, at);
      }
    }
    for (int c = 0; c < D; ++c) max_rows = std::max(max_rows, g_class_begin[size_t(c) + 1] - g_class_begin[size_t(c)]);
    const u64 out_bytes = max_rows * SPR * gc.slab_bytes;
    const bool two_in = D > 1 && k < F, two_out = D > 1;
    char* in_slot[2] = {k < F ? BigAlloc<char>(max_in) : nullptr, two_in ? BigAlloc<char>(max_in) : nullptr};
    char* out_slot[2] = {BigAlloc<char>(out_bytes), two_out ? BigAlloc<char>(out_bytes) : nullptr};

    std::thread reader, writer;
    int reading_class = -1;
    auto start_read = [&](int d, char* slot) {
      reading_class = d;
      reader = std::thread([&next_files, d, slot, &in_base, &aux] { ReadRows(next_files, d, slot, in_base[size_t(d)], aux); });
    };
    double in_wait = 0, out_wait = 0, last_report = Now();
    int cur_in = 0, cur_out = 0;
    const size_t chunk = WORDS >= 512 ? 1 : (WORDS >= 64 ? 8 : 64);
    for (size_t ti = 0; ti < todo.size(); ++ti) {
      const int c = todo[ti];
      const double tw0 = Now();
      if (k < F) {
        if (reading_class != c) {
          if (reader.joinable()) reader.join();
          start_read(c, in_slot[cur_in]);
        }
        reader.join();
        reading_class = -1;
        if (two_in && ti + 1 < todo.size()) start_read(todo[ti + 1], in_slot[cur_in ^ 1]);
      }
      in_wait += Now() - tw0;
      const u64 rb = g_class_begin[size_t(c)], re = g_class_begin[size_t(c) + 1];
      char* outbuf = out_slot[cur_out];
      const u64 before = loss_raw.load();
      if (k == F) {
        // No empty cell: nobody can move, so every position is lost.
        u64 lr = 0;
        for (u64 y = rb; y < re; ++y) {
          for (u64 j = 0; j < SPR; ++j) reinterpret_cast<u64*>(outbuf)[(y - rb) * SPR + j] = 1;
          lr += u64(g_orbit[y]) * SPR;
        }
        loss_raw.fetch_add(lr);
      } else {
        KernelContext cx;
        cx.k = k;
        cx.inbuf = in_slot[cur_in];
        cx.in_base = &in_base[size_t(c)];
        cx.outbuf = outbuf;
        cx.class_begin = rb;
        cx.check_every = check_every;
        cx.loss_raw = &loss_raw;
        cx.checked = &checked;
        std::atomic<u64> next_row{rb};
        std::vector<std::thread> drivers;
        for (auto& wp : g_workers) {
          Worker* w = wp.get();
          drivers.emplace_back([w, &next_row, re, &cx, chunk] {
            PinCaller(w->pool->cpu0());
            for (;;) {
              const u64 y = next_row.fetch_add(1);
              if (y >= re) break;
              ProcessRow(*w, y, cx, chunk);
            }
          });
        }
        for (auto& t : drivers) t.join();
      }
      const u64 class_loss = loss_raw.load() - before;
      const double tw1 = Now();
      if (writer.joinable()) writer.join();
      out_wait += Now() - tw1;
      ClassIndex* slot_index = &cls[size_t(c)];
      if (two_out) {
        writer = std::thread([k, c, outbuf, class_loss, &aux, slot_index] { WriteClass(k, c, outbuf, class_loss, aux, slot_index); });
      } else {
        WriteClass(k, c, outbuf, class_loss, aux, slot_index);
      }
      cur_out ^= int(two_out);
      cur_in ^= int(two_in);
      if (Now() - last_report > 60) {
        last_report = Now();
        const double el = Now() - t0;
        std::printf("#   k=%d: class %zu/%zu, %.0fs in layer, about %.0fs left\n", k, ti + 1, todo.size(), el, el / double(ti + 1) * double(todo.size() - ti - 1));
      }
    }
    if (reader.joinable()) reader.join();
    if (writer.joinable()) writer.join();
    BigFree(in_slot[0], max_in);
    BigFree(in_slot[1], max_in);
    BigFree(out_slot[0], out_bytes);
    BigFree(out_slot[1], out_bytes);

    cur_files.Open();
    const u64 p = C[N][Q] * C[N - Q][Q] * C[F][k];
    const u64 l = loss_raw.load();
    if (l > p) Die("impossible loss count on layer " + std::to_string(k));
    LayerResult r;
    r.k = k;
    r.positions = p;
    r.wins = p - l;
    r.losses = l;
    r.bytes = cur_files.TotalBytes();
    r.raw_bytes = rows * SPR * gc.slab_bytes;
    double g = 0, s = 0, pa = 0;
    for (auto& w : g_workers) g += w->t_gather, s += w->t_shots, pa += w->t_parents;
    const double nw = double(g_workers.size());

    if (!compare_dir.empty()) {
      auto table = LoadWholeLayer(cur_files);
      CompareWithDense(table.get(), k, compare_dir);
    }
    if (verify_samples) VerifyLayerFromFiles(cur_files, k < F ? &next_files : nullptr, verify_samples, 32);
    r.secs = Now() - t0;
    AppendProgress(r);
    results.push_back(r);
    std::printf(" %3d %18" PRIu64 " %18" PRIu64 " %18" PRIu64 " %10" PRIu64 " %7.2f %8.1f %8.1f %8.1f %8.1f %8.1f %8.1f\n", k, p, p - l, l, r.bytes,
                double(r.raw_bytes) / double(std::max<u64>(r.bytes, 1)), g / nw, s / nw, pa / nw, in_wait, out_wait, r.secs);
    if (checked.load()) std::printf("#   k=%d: %" PRIu64 " positions relabelled in memory, no mismatch\n", k, checked.load());
    if (k == 0 && Q == 4 && std::min(W, H) >= 4) {
      SlabReader rd(&cur_files);
      u64 ms, os;
      StartPosition(&ms, &os);
      std::printf("# root %dx%d: %s\n", W, H, rd.Loss(ms, os, 0) ? "second player wins" : "first player wins");
      if (W != H) {
        StartPositionTransposed(&ms, &os);
        std::printf("# root %dx%d: %s\n", H, W, rd.Loss(ms, os, 0) ? "second player wins" : "first player wins");
      }
    }
    // The child layer has done its job.
    if (k < F) {
      next_files.Close();
      if (KeepLayer(k + 1)) mover.Enqueue(k + 1);
      else DeleteLayer(k + 1);
    }
    next_files = LayerFiles();
    next_files.Load(k);
    next_files.Open();
    cur_files.Close();
  }
  next_files.Close();
  u64 tw = 0, tl = 0, tb = 0;
  for (const auto& r : results) tw += r.wins, tl += r.losses, tb += r.bytes;
  std::printf("# totals: win=%" PRIu64 " loss=%" PRIu64 " compressed=%.2f GiB\n", tw, tl, double(tb) / kGiB);
  {
    std::lock_guard<std::mutex> lk(g_io.m);
    std::printf("# io: read %.2f GiB, written %.2f GiB; compress %.0fs, write %.0fs, read+decompress %.0fs\n", double(g_io.read_bytes.load()) / kGiB, double(g_io.written_bytes.load()) / kGiB,
                g_io.compress_secs, g_io.write_secs, g_io.read_secs);
  }
  std::printf("# computed in %.1fs\n", Now());
  if (kmin == 0 && KeepLayer(0)) mover.Enqueue(0);  // layer 0 has no parent to retire it
  mover.Finish();
  std::printf("# done %.1fs\n", Now());
  return 0;
}

// ------------------------------------------------------------------ probe

// The stored solution of a finished run, including layers that were dropped.
class Oracle {
 public:
  Oracle() {
    // The class count is part of the file format, so take it from the run's own record.
    const std::string cfg = g_roots[0] + "/config.txt";
    std::ifstream in(cfg);
    if (!in) Die("no config.txt in " + g_roots[0]);
    std::string key, val;
    D = 0;
    while (in >> key >> val) {
      if (key == "classes") D = std::atoi(val.c_str());
      if (key == "canon") g_canon_max = val == "max";
      if (key == "queens" && std::atoi(val.c_str()) != Q) Die("queens differ from the run's");
    }
    if (D <= 0) Die("bad config.txt");
    BuildRows();
    BuildArrowRanks();
    BuildTiles();
  }
  SlabReader* Reader(int layer) {
    auto it = readers_.find(layer);
    if (it != readers_.end()) return it->second.get();
    auto lf = std::make_unique<LayerFiles>();
    lf->Load(layer);
    if (!lf->Complete()) {
      readers_[layer] = nullptr;
      return nullptr;
    }
    lf->Open();
    auto r = std::make_unique<SlabReader>(lf.get());
    files_[layer] = std::move(lf);
    SlabReader* out = r.get();
    readers_[layer] = std::move(r);
    return out;
  }
  // The mover loses.  A dropped layer takes one ply of search over the next one.
  bool Loss(u64 m, u64 o, u64 a) {
    const int layer = __builtin_popcountll(a);
    if (SlabReader* r = Reader(layer)) return r->Loss(m, o, a);
    if (layer >= F) return true;
    SlabReader* n = Reader(layer + 1);
    if (!n) Die("neither layer " + std::to_string(layer) + " nor " + std::to_string(layer + 1) + " is available");
    return !ExplicitWin(m, o, a, [&](u64 m2, u64 o2, u64 a2) { return n->Loss(m2, o2, a2); });
  }

 private:
  std::unordered_map<int, std::unique_ptr<LayerFiles>> files_;
  std::unordered_map<int, std::unique_ptr<SlabReader>> readers_;
};

struct Move {
  int f, t, s;
};

std::vector<Move> LegalMoves(u64 m, u64 o, u64 a) {
  std::vector<Move> out;
  const u64 occ = m | o | a;
  for (u64 mm = m; mm; mm &= mm - 1) {
    const int f = __builtin_ctzll(mm);
    for (int d = 0; d < 8; ++d) {
      for (int j = 0; j < RAYLEN[f][d]; ++j) {
        const int t = RAYCELL[f][d][j];
        if ((occ >> t) & 1) break;
        const u64 occ2 = (occ & ~(u64(1) << f)) | (u64(1) << t);
        for (int d2 = 0; d2 < 8; ++d2) {
          for (int i = 0; i < RAYLEN[t][d2]; ++i) {
            const int s = RAYCELL[t][d2][i];
            if ((occ2 >> s) & 1) break;
            out.push_back(Move{f, t, s});
          }
        }
      }
    }
  }
  return out;
}

u64 CellsToMask(const std::string& s) {
  u64 m = 0;
  std::stringstream ss(s);
  std::string part;
  while (std::getline(ss, part, ',')) {
    if (part.empty()) continue;
    const int c = std::atoi(part.c_str());
    if (c < 0 || c >= N) Die("cell out of range: " + std::to_string(c));
    m |= u64(1) << c;
  }
  return m;
}

int CmdProbe(const std::string& ms, const std::string& os, const std::string& as, bool list_moves) {
  Oracle oracle;
  const u64 m = CellsToMask(ms), o = CellsToMask(os), a = CellsToMask(as);
  if (__builtin_popcountll(m) != Q || __builtin_popcountll(o) != Q || (m & o) || (a & (m | o))) Die("bad position");
  const int k = __builtin_popcountll(a);
  std::printf("%s to move at layer %d\n", oracle.Loss(m, o, a) ? "LOSS" : "WIN", k);
  if (list_moves) {
    for (const Move& v : LegalMoves(m, o, a)) {
      const u64 moved = (m & ~(u64(1) << v.f)) | (u64(1) << v.t);
      std::printf("%d-%d x%d: %s\n", v.f, v.t, v.s, oracle.Loss(o, moved, a | (u64(1) << v.s)) ? "wins for the mover" : "loses for the mover");
    }
  }
  return 0;
}

// Plays whole games with the rules and the stored table, and checks them against each
// other at every ply: a win must have a losing child, a loss must have no winning move
// (all of them, or a sample of `sample` where a position has more), and a player who
// cannot move must be recorded as lost.  The winner follows the table; the loser moves at
// random.  Games start from the standard setup of both orientations.
int CmdPlayout(int games, int sample) {
  Oracle oracle;
  std::mt19937_64 rng(20261102);
  u64 plies = 0, children = 0;
  const bool two_roots = W != H;
  for (int game = 0; game < games; ++game) {
    u64 m, o, a = 0;
    if (two_roots && (game & 1)) StartPositionTransposed(&m, &o);
    else StartPosition(&m, &o);
    const bool root_loss = oracle.Loss(m, o, a);
    int ply = 0;
    for (;; ++ply) {
      const bool loss = oracle.Loss(m, o, a);
      std::vector<Move> moves = LegalMoves(m, o, a);
      std::shuffle(moves.begin(), moves.end(), rng);
      auto child_loss = [&](const Move& v) {
        const u64 moved = (m & ~(u64(1) << v.f)) | (u64(1) << v.t);
        ++children;
        return oracle.Loss(o, moved, a | (u64(1) << v.s));
      };
      ++plies;
      if (moves.empty()) {
        if (!loss) Die("a player without a move is recorded as winning");
        break;
      }
      const Move* chosen = nullptr;
      if (loss) {
        const size_t n = std::min<size_t>(size_t(sample), moves.size());
        for (size_t i = 0; i < n; ++i) {
          if (child_loss(moves[i])) Die("a lost position has a winning move at ply " + std::to_string(ply));
        }
        chosen = &moves[0];
      } else {
        for (const Move& v : moves) {
          if (child_loss(v)) {
            chosen = &v;
            break;
          }
        }
        if (!chosen) Die("a won position has no winning move at ply " + std::to_string(ply));
      }
      const u64 moved = (m & ~(u64(1) << chosen->f)) | (u64(1) << chosen->t);
      a |= u64(1) << chosen->s;
      m = o;
      o = moved;
    }
    // Whoever is stuck has lost, and that must be the side the root value said.
    const bool first_player_stuck = (ply % 2) == 0;
    if (first_player_stuck != root_loss) Die("a game ended against the root value");
    std::printf("game %d (%s setup): %s, %d plies\n", game, (two_roots && (game & 1)) ? "transposed" : "standard", root_loss ? "second player won" : "first player won", ply);
  }
  std::printf("playout: %d games, %" PRIu64 " plies, %" PRIu64 " child values checked, no contradiction\n", games, plies, children);
  return 0;
}

// ------------------------------------------------------------------ bench

// Kernel timing on real board sizes.  The kernel has no early exit, so its running time
// does not depend on the data and random child words give an honest number.
int CmdBench(int k, int nrows) {
  BuildRows();
  BuildArrowRanks();
  InitLaneMasks();
  g_lq.Set(k);
  g_lc.Set(k + 1);
  const u64 rows = g_rows.size();
  std::printf("# bench %dx%d q=%d free=%d words/slab=%u rows=%" PRIu64 " slabs/row=%" PRIu64 " k=%d workers=%zu threads each=%d\n", W, H, Q, F, WORDS, rows, SPR, k, g_workers.size(),
              g_workers[0]->pool->size());
  std::printf("# live words %zu of %u, mode %d, positions/slab %" PRIu64 "\n", g_lq.list.size(), WORDS, g_lq.mode, C[F][k]);
  const size_t chunk = WORDS >= 512 ? 1 : (WORDS >= 64 ? 8 : 64);
  double slowest = 0;
  std::vector<std::thread> drivers;
  std::mutex out;
  const Geom gc = GeomOf(k);
  for (size_t wi = 0; wi < g_workers.size(); ++wi) {
    drivers.emplace_back([&, wi] {
      Worker& w = *g_workers[wi];
      PinCaller(w.pool->cpu0());
      w.pool->For(size_t(SPR), chunk, [&](size_t j, int) {
        std::mt19937_64 rng(j + 1);
        alignas(64) u64 l[8];
        for (const u32 i : g_lc.list) {
          const int jj = k + 1 - __builtin_popcount(i);
          for (int a = 0; a < 8; ++a) {
            const int ja = jj - __builtin_popcount(a);
            l[a] = (ja >= 0 && ja <= 6) ? (rng() & rng() & LMASK[a][ja]) : 0;
          }
          _mm512_stream_si512(w.child + j * WORDS + i, _mm512_load_si512(l));
        }
      });
      std::unique_ptr<u64[]> sink(new u64[size_t(w.pool->size()) * (gc.stride + 2)]);
      double sum_s = 0, sum_p = 0;
      for (int r = 0; r < nrows; ++r) {
        const u64 yi = (rows / u64(nrows + 1)) * u64(r + 1) + wi;
        const u64 y = g_rows[yi];
        const u64 rest = ~y & BOARD;
        const double t0 = Now();
        w.pool->For(size_t(SPR), chunk, [&](size_t j, int tid) {
          BuildShots(y, _pdep_u64(g_qsub[j], rest), w.child + j * WORDS, w.shots + j * size_t(Q) * WORDS, w.Scratch(tid, 0));
        });
        const double t1 = Now();
        w.pool->For(size_t(SPR), chunk, [&](size_t j, int tid) {
          V* b = w.Scratch(tid, 1);
          SolveParent(_pdep_u64(g_qsub[j], rest), y, w.shots, b);
          CompactLosses(b, k, sink.get() + size_t(tid) * (gc.stride + 2));
        });
        const double t2 = Now();
        sum_s += t1 - t0;
        sum_p += t2 - t1;
      }
      std::lock_guard<std::mutex> l(out);
      std::printf("#  worker %zu: shots %.3fs parents+compact %.3fs per row\n", wi, sum_s / nrows, sum_p / nrows);
      slowest = std::max(slowest, (sum_s + sum_p) / nrows);
    });
  }
  for (auto& t : drivers) t.join();
  const double rate = double(g_workers.size()) * double(SPR) * double(C[F][k]) / slowest;
  std::printf("# %.3f s per row per worker -> %.2e positions/s; central-layer time %.0fs\n", slowest, rate, slowest * double(rows) / double(g_workers.size()));
  return 0;
}

// --------------------------------------------------------------- selftest

// Every permute against its definition, on random words.
int CmdSelfTest() {
  std::mt19937_64 rng(12345);
  auto bit_of = [](const u64* w, int z) { return (w[z >> 6] >> (z & 63)) & 1; };
  u64 cases = 0;
  for (int rep = 0; rep < 200; ++rep) {
    alignas(64) u64 a[8], b[8], out[8];
    for (int i = 0; i < 8; ++i) a[i] = rng(), b[i] = rng();
    const V va = _mm512_load_si512(a), vb = _mm512_load_si512(b);
    for (int bt = 0; bt < 9; ++bt) {
      for (int bf = 0; bf < 9; ++bf) {
        _mm512_store_si512(out, ApplyB(BT[bt][bf], va));
        for (int z = 0; z < 512; ++z) {
          if ((z >> bt) & 1) continue;
          const int zp = int(Insert0(RemoveBit(u32(z), bt), bf));
          if (bit_of(out, z) != bit_of(a, zp)) {
            std::printf("B bt=%d bf=%d z=%d kind=%d\n", bt, bf, z, int(BT[bt][bf].kind));
            return 1;
          }
          ++cases;
        }
      }
      for (int h = 0; h < 2; ++h) {
        _mm512_store_si512(out, ApplyC(CT[bt], va, h));
        for (int z = 0; z < 512; ++z) {
          if ((z >> bt) & 1) continue;
          const int zp = int((RemoveBit(u32(z), bt) & 255) | u32(h << 8));
          if (bit_of(out, z) != bit_of(a, zp)) {
            std::printf("C bt=%d h=%d z=%d\n", bt, h, z);
            return 1;
          }
          ++cases;
        }
      }
    }
    for (int bf = 0; bf < 9; ++bf) {
      _mm512_store_si512(out, ApplyD(DT[bf], va, vb));
      for (int z = 0; z < 512; ++z) {
        const int zp = int(Insert0(u32(z) & 255, bf));
        const u64* src = (z >> 8) ? b : a;
        if (bit_of(out, z) != bit_of(src, zp)) {
          std::printf("D bf=%d z=%d\n", bf, z);
          return 1;
        }
        ++cases;
      }
    }
    for (int l = 0; l < 3; ++l) {
      for (int bb = 3; bb < 9; ++bb) {
        _mm512_store_si512(out, SwapInWord(va, l, bb));
        for (int z = 0; z < 512; ++z) {
          const int lb = (z >> l) & 1, hb = (z >> bb) & 1;
          const int zs = (z & ~((1 << l) | (1 << bb))) | (hb << l) | (lb << bb);
          if (bit_of(out, z) != bit_of(a, zs)) {
            std::printf("swap l=%d b=%d z=%d\n", l, bb, z);
            return 1;
          }
          ++cases;
        }
      }
    }
  }
  // Word-permuting symmetry fetch against the bit-walking one, on real geometry.
  for (const auto& [w, h, q] : {std::array<int, 3>{4, 5, 4}, std::array<int, 3>{4, 6, 4}, std::array<int, 3>{5, 5, 4}, std::array<int, 3>{4, 7, 4}, std::array<int, 3>{5, 6, 4}}) {
    InitBoard(w, h, q);
    BuildRows();
    BuildArrowRanks();
    InitLaneMasks();
    V* slab_a = BigAlloc<V>(WORDS);
    V* slab_b = BigAlloc<V>(WORDS);
    V* buf = BigAlloc<V>(WORDS);
    PermScratch sc;
    for (int kk = 0; kk <= F; kk += 3) {
      g_lc.Set(kk);
      const u64 stride = StrideWords(kk);
      std::vector<u64> src(stride + 1);
      for (int rep = 0; rep < 12; ++rep) {
        for (auto& x : src) x = rng();
        const u64 extra = C[F][kk] % 64;
        if (extra) src[stride - 1] &= (u64(1) << extra) - 1;
        src[stride] = 0;
        std::vector<int> cells(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i) cells[size_t(i)] = i;
        std::shuffle(cells.begin(), cells.end(), rng);
        u64 occ = 0;
        for (int i = 0; i < 2 * q; ++i) occ |= u64(1) << cells[size_t(i)];
        const int g = int(rng() % u64(GN));
        const u64 src_free = ~occ & BOARD;
        const u64 dst_free = Transform(int(0), 0) | (~[&] { u64 r = 0; u64 m = occ; while (m) { r |= u64(1) << GINV[g][__builtin_ctzll(m)]; m &= m - 1; } return r; }() & BOARD);
        int c1[32], c2[32];
        IndexMapVia(src_free, dst_free, g, c1);
        std::memcpy(c2, c1, sizeof(c1));
        std::memset(static_cast<void*>(slab_a), 0, WORDS * sizeof(V));
        std::memset(static_cast<void*>(slab_b), 0, WORDS * sizeof(V));
        ExpandViaBits(src.data(), kk, c1, buf, slab_a, sc);
        ExpandViaWords(src.data(), kk, c2, buf, slab_b, sc);
        _mm_sfence();
        if (std::memcmp(slab_a, slab_b, WORDS * sizeof(V)) != 0) {
          std::printf("symmetry fetch differs: %dx%d k=%d g=%d\n", w, h, kk, g);
          return 1;
        }
        ++cases;
      }
    }
    BigFree(slab_a, WORDS);
    BigFree(slab_b, WORDS);
    BigFree(buf, WORDS);
  }
  std::printf("selftest: %" PRIu64 " checks passed\n", cases);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  g_t0 = Clock::now();
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  InitLowPerms();
  const char* usage = "usage: slabsolve solve WxH --root DIR [--root DIR2] [options]\n       slabsolve bench WxH [--k K] [--rows R]\n       slabsolve probe WxH --root DIR --m CELLS --o CELLS [--a CELLS] [--moves]\n       slabsolve playout WxH --root DIR [--games G]\n       slabsolve selftest\nSee the top of slabsolve.cc for the options.\n";
  if (argc < 2) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "selftest") return CmdSelfTest();
  if ((cmd != "solve" && cmd != "bench" && cmd != "probe" && cmd != "playout") || argc < 3) {
    std::fputs(usage, stderr);
    return 2;
  }
  int w = 0, h = 0, q = 4, threads = 0, workers = 0, aux = 0, kmin = 0, bench_k = -1, bench_rows = 3, check_every = 64;
  u64 verify = 1000;
  bool list_moves = false;
  int games = 20, sample = 16;
  if (std::sscanf(argv[2], "%dx%d", &w, &h) != 2) Die("bad board");
  std::string compare, pm, po, pa;
  D = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    auto val = [&]() -> const char* {
      if (i + 1 >= argc) Die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--queens") q = std::atoi(val());
    else if (a == "--threads") threads = std::atoi(val());
    else if (a == "--workers") workers = std::atoi(val());
    else if (a == "--aux") aux = std::atoi(val());
    else if (a == "--compare") compare = val();
    else if (a == "--kmin") kmin = std::atoi(val());
    else if (a == "--k") bench_k = std::atoi(val());
    else if (a == "--rows") bench_rows = std::atoi(val());
    else if (a == "--root") g_roots.push_back(val());
    else if (a == "--classes") D = std::atoi(val());
    else if (a == "--level") g_level = std::atoi(val());
    else if (a == "--level-keep") g_level_keep = std::atoi(val());
    else if (a == "--reserve") g_reserve = u64(std::atoll(val())) << 30;
    else if (a == "--verify-samples") verify = u64(std::atoll(val()));
    else if (a == "--check-every") check_every = std::atoi(val());
    else if (a == "--canon") {
      const std::string o = val();
      if (o != "min" && o != "max") Die("canon must be min or max");
      g_canon_max = o == "max";
    } else if (a == "--keep") {
      const std::string o = val();
      if (o == "all") g_keep = kKeepAll;
      else if (o == "parity") g_keep = kKeepParity;
      else if (o == "none") g_keep = kKeepNone;
      else Die("keep must be all, parity or none");
    } else if (a == "--m") pm = val();
    else if (a == "--o") po = val();
    else if (a == "--a") pa = val();
    else if (a == "--moves") list_moves = true;
    else if (a == "--games") games = std::atoi(val());
    else if (a == "--sample") sample = std::atoi(val());
    else Die("unknown option " + a);
  }
  InitBoard(w, h, q);
  if (cmd == "probe" || cmd == "playout") {
    if (g_roots.empty()) Die(cmd + " needs --root");
    if (cmd == "playout") return CmdPlayout(games, sample);
    return CmdProbe(pm, po, pa, list_moves);
  }
  if (cmd == "bench") {
    InitWorkers(threads, workers);
    return CmdBench(bench_k < 0 ? F / 2 : bench_k, bench_rows);
  }
  if (g_roots.empty()) Die("solve needs --root");
  InitWorkers(threads, workers);
  MoveCallerToSecondaryThreads();
  if (aux <= 0) {
    for (const auto& list : ReadTopology().secondary) aux += int(list.size());
    aux = std::max(aux, 4);
  }
  return CmdSolve(compare, kmin, verify, aux, check_every);
}
