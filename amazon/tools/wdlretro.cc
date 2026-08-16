/*
  amazons — wdlretro: external-memory WDL table builder for standard-setup
  Amazons (target: 5x5 with 4 amazons per side, ~3e11-6e11 canonical
  reachable positions).

  The game graph is a strictly layered DAG (every move burns exactly one
  square; layer k = positions with k burned squares; depth <= nsq-8), which
  allows a single forward enumeration sweep followed by a single backward
  retrograde sweep.  No cycles, no fixpoint.

  Storage layout in the output directory (little-endian):
    meta.txt                 "wdlretro1 W H P" — checked on resume
    layerLL.qQQ.keys         uint64 canonical keys, unique within the part
    wdlLL.qQQ.bits           2-bit verdicts (1=W, 2=L), 4 per byte LSB-first,
                             aligned 1:1 with the .keys file of the same part
    stateLL.u16              transient retrograde state: 8-byte pass bitmask
                             header + uint16 per position; atomic via rename,
                             deleted on finalize
    done/...                 enumeration/finalize marker files; an
                             interrupted enum pass is idempotent and simply
                             redone

  Positions are canonicalized (board symmetries + color swap, stm
  normalized to white) and encoded as a base-4 integer over w*h squares
  (requires w*h <= 30, so a key fits in 60 bits; the top 4 bits carry
  verdicts inside probe tables).  Keys are partitioned by
  Part(k) = KeyHash(k) & (P-1).

  Both phases use the same partition-pass pattern, which keeps RAM bounded
  by roughly (layer size / P) instead of the full layer, and never stores
  edges (children are always regenerated from parents):

  ENUM(k, q):   expand every parent in layer k, keep canonical children
                with Part == q in a sharded in-RAM dedup set, dump the set
                to layer{k+1}.q{q}.keys.
  RETRO(k, q):  build a probe hash of layer k+1 part q (key -> verdict),
                stream layer k, regenerate each parent's children, and
                accumulate per-parent state (uint16: bit15 = found an L
                child i.e. position is W; low 12 bits = number of W children
                seen so far).  State is checkpointed after each pass.
  FINALIZE(k):  stream layer k once more, recomputing each non-W position's
                out-degree d (no probe needed).  Verdict: W if flagged,
                else L iff wcount == d (includes terminals, d == 0).
                Emits the 2-bit .bits files and deletes the state file.

  A position is W iff some child is L, L iff all children are W; Amazons
  has no draws.  The deepest enumerated layer is terminal by construction
  (its expansion produced zero children), so it finalizes to all-L without
  passes.

  Everything is resumable: rerun the same command and it continues from the
  last completed pass.  Sanity checks: --expect w|l compares the root
  verdict (5x5 is known to be a first-player win), and --verify N samples N
  positions per layer and re-derives their verdicts from the child tables.

  Resource model for 5x5 on a 750 GB / 7 TB machine with P=4:
  peak partition hash ~350 GB, state array ~200 GB, disk ~8.25 B/position
  total (keys + 2-bit verdicts), no edge lists.  Expect ~2-4 weeks of
  compute: enumeration costs P expansion passes over every layer,
  retrograde P+1.

  Usage:
    wdlretro WxH DIR [--parts P] [--threads T] [--ram-gb GB]
               [--expect w|l] [--verify N]
*/

#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "board.h"

namespace amazons {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint64_t kChunkKeys = 1 << 20;  // parent streaming chunk (8 MiB)
constexpr int kShards = 1024;
constexpr uint16_t kWinFlag = 0x8000;
constexpr uint16_t kWCountMask = 0x0FFF;
constexpr uint64_t kVerdictShift = 60;  // requires w*h <= 30

int g_w, g_h, g_nsq, g_parts, g_threads;
uint64_t g_ram_budget;
std::string g_dir;
Clock::time_point g_t0;

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("[%8.0fs] ",
              std::chrono::duration<double>(Clock::now() - g_t0).count());
  std::vprintf(fmt, ap);
  std::printf("\n");
  std::fflush(stdout);
  va_end(ap);
}

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "wdlretro: FATAL: %s\n", msg.c_str());
  std::exit(1);
}

// ---------- key codec ----------

uint64_t Encode(const Position& c) {
  uint64_t k = 0, p = 1;
  for (int sq = 0; sq < g_nsq; sq++, p *= 4) {
    const uint64_t b = uint64_t{1} << sq;
    if (c.queens[kWhite] & b)
      k += p;
    else if (c.queens[kBlack] & b)
      k += 2 * p;
    else if (c.burned & b)
      k += 3 * p;
  }
  return k;
}

Position Decode(uint64_t k) {
  Position p;
  p.w = g_w;
  p.h = g_h;
  p.stm = kWhite;
  for (int sq = 0; sq < g_nsq; sq++, k /= 4) {
    const uint64_t b = uint64_t{1} << sq;
    switch (k % 4) {
      case 1: p.queens[kWhite] |= b; break;
      case 2: p.queens[kBlack] |= b; break;
      case 3: p.burned |= b; break;
      default: break;
    }
  }
  return p;
}

// ---------- fast canonical key ----------
//
// The hot path of both phases is per-child canonicalization, so it is done
// directly on bitboards with precomputed symmetry tables instead of
// Position::Canonical() (which loops bit-by-bit per symmetry):
//
//   MapBB(s, bb): image of bb under symmetry s via byte-chunk gather
//                 tables (4 lookups for w*h <= 32).
//   CanonKey:     min over symmetries of the (white, black, burned) triple
//                 — the same representative Position::Canonical() picks —
//                 encoded as Spread(W) + 2*Spread(B) + 3*Spread(X), which
//                 equals Encode() of that position (carry-free: each base-4
//                 digit slot holds one disjoint 2-bit value).
//
// Positions in the table are white-to-move, so a child always has black to
// move and canonicalization swaps colors: ChildKey swaps the queen sets.

struct SymTables {
  uint64_t m[8][4][256];  // [sym][input byte 0..3][byte value] -> image bits
};
SymTables g_sym;

void BuildSymTables() {
  const bool square = (g_w == g_h);
  std::memset(&g_sym, 0, sizeof g_sym);
  for (int s = 0; s < 8; s++) {
    const bool swap_axes = square && (s & 4);
    const bool flip_x = s & 1, flip_y = s & 2;
    for (int j = 0; j < 4; j++)
      for (int v = 0; v < 256; v++) {
        uint64_t word = 0;
        for (int b = 0; b < 8; b++) {
          if (!((v >> b) & 1)) continue;
          const int sq = j * 8 + b;
          if (sq >= g_nsq) continue;
          int x = sq % g_w, y = sq / g_w;
          if (swap_axes) std::swap(x, y);
          if (flip_x) x = g_w - 1 - x;
          if (flip_y) y = g_h - 1 - y;
          word |= uint64_t{1} << (y * g_w + x);
        }
        g_sym.m[s][j][v] = word;
      }
  }
}

inline uint64_t MapBB(int s, uint64_t bb) {
  return g_sym.m[s][0][bb & 0xFF] | g_sym.m[s][1][(bb >> 8) & 0xFF] |
         g_sym.m[s][2][(bb >> 16) & 0xFF] | g_sym.m[s][3][(bb >> 24) & 0xFF];
}

inline uint64_t Spread(uint64_t v) {  // bit i -> bit 2i (input < 2^32)
  v = (v | (v << 16)) & 0x0000FFFF0000FFFFull;
  v = (v | (v << 8)) & 0x00FF00FF00FF00FFull;
  v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0Full;
  v = (v | (v << 2)) & 0x3333333333333333ull;
  v = (v | (v << 1)) & 0x5555555555555555ull;
  return v;
}

// Canonical base-4 key of the position with the given bitboards, colors
// already normalized so that the listed white set is the side to move.
inline uint64_t CanonKey(uint64_t wq, uint64_t bq, uint64_t burn) {
  const int nsym = (g_w == g_h) ? 8 : 4;
  uint64_t bw = ~0ull, bb = 0, bx = 0;
  for (int s = 0; s < nsym; s++) {
    const uint64_t w2 = MapBB(s, wq);
    if (w2 > bw) continue;
    const uint64_t b2 = MapBB(s, bq);
    if (w2 == bw && b2 > bb) continue;
    const uint64_t x2 = MapBB(s, burn);
    if (w2 == bw && b2 == bb && x2 >= bx) continue;
    bw = w2;
    bb = b2;
    bx = x2;
  }
  return Spread(bw) + 2 * Spread(bb) + 3 * Spread(bx);
}

// Canonical key of the child of white-to-move position `p` after move `m`.
inline uint64_t ChildKey(const Position& p, Move m) {
  Position c = p;
  c.DoMove(m);
  // Child has black to move; canonicalization swaps colors.
  return CanonKey(c.queens[kBlack], c.queens[kWhite], c.burned);
}

uint64_t KeyHash(uint64_t k) {
  k *= 0x9E3779B97F4A7C15ull;
  return k ^ (k >> 29);
}

int Part(uint64_t key) { return int(KeyHash(key) & uint64_t(g_parts - 1)); }

// ---------- paths / small file helpers ----------

std::string KeysPath(int layer, int q) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "layer%02d.q%02d.keys", layer, q);
  return g_dir + "/" + buf;
}
std::string BitsPath(int layer, int q) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "wdl%02d.q%02d.bits", layer, q);
  return g_dir + "/" + buf;
}
std::string StatePath(int layer) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "state%02d.u16", layer);
  return g_dir + "/" + buf;
}
std::string MarkerPath(const std::string& name) { return g_dir + "/done/" + name; }

bool FileExists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}
uint64_t FileSize(const std::string& p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) return 0;
  return uint64_t(st.st_size);
}
void Touch(const std::string& p) {
  int fd = ::open(p.c_str(), O_CREAT | O_WRONLY, 0644);
  if (fd < 0) Die("cannot create " + p);
  ::close(fd);
}
void AtomicRename(const std::string& tmp, const std::string& dst) {
  if (::rename(tmp.c_str(), dst.c_str()) != 0) Die("rename " + tmp + " -> " + dst);
}

// ---------- sharded growable open-addressed uint64 set ----------
// Slot value 0 = empty (key 0 never occurs: queens are always present).
// For probe tables the slot is key | (verdict << kVerdictShift).

struct KeySet {
  struct Shard {
    std::vector<uint64_t> slots;
    size_t mask = 0;
    size_t count = 0;
    std::mutex mu;
  };
  std::vector<Shard> sh{size_t(kShards)};
  std::atomic<uint64_t> total_slots{0};
  uint64_t slot_budget = 0;
  std::atomic<bool> oom{false};

  void Init(uint64_t expected, uint64_t budget_bytes) {
    slot_budget = budget_bytes / sizeof(uint64_t);
    size_t cap = 1 << 14;
    const size_t per = size_t(expected / kShards) * 2 + 16;
    while (cap < per) cap *= 2;
    for (auto& s : sh) {
      s.slots.assign(cap, 0);
      s.mask = cap - 1;
      s.count = 0;
    }
    total_slots = uint64_t(kShards) * cap;
    if (total_slots > slot_budget) oom.store(true);
  }

  bool Insert(uint64_t k) {
    Shard& s = sh[KeyHash(k) & (kShards - 1)];
    std::lock_guard<std::mutex> lk(s.mu);
    if (__builtin_expect((s.count + 1) * 2 > s.slots.size(), 0)) {
      const uint64_t add = s.slots.size();  // doubling adds this many slots
      if (total_slots.fetch_add(add) + add > slot_budget) {
        total_slots.fetch_sub(add);
        oom.store(true);
        return false;
      }
      std::vector<uint64_t> ns(s.slots.size() * 2, 0);
      const size_t nm = ns.size() - 1;
      for (uint64_t v : s.slots) {
        if (!v) continue;
        size_t i = KeyHash(v) & nm;
        while (ns[i]) i = (i + 1) & nm;
        ns[i] = v;
      }
      s.slots.swap(ns);
      s.mask = nm;
    }
    size_t i = KeyHash(k) & s.mask;
    while (s.slots[i]) {
      if (s.slots[i] == k) return true;
      i = (i + 1) & s.mask;
    }
    s.slots[i] = k;
    s.count++;
    return true;
  }

  uint64_t Count() const {
    uint64_t n = 0;
    for (const auto& s : sh) n += s.count;
    return n;
  }
};

// Lock-free probe into a fully-built KeySet.  Returns the verdict (1=W,
// 2=L) or 0 if absent.
uint8_t Probe(const KeySet& set, uint64_t key) {
  const KeySet::Shard& s = set.sh[KeyHash(key) & (kShards - 1)];
  const uint64_t kmask = (uint64_t{1} << kVerdictShift) - 1;
  size_t i = KeyHash(key) & s.mask;
  while (s.slots[i]) {
    if ((s.slots[i] & kmask) == key) return uint8_t(s.slots[i] >> kVerdictShift);
    i = (i + 1) & s.mask;
  }
  return 0;
}

// ---------- parallel helpers ----------

void RunParallel(const std::function<void(int)>& fn) {
  std::vector<std::thread> pool;
  for (int t = 0; t < g_threads; t++) pool.emplace_back(fn, t);
  for (auto& th : pool) th.join();
}

// Parallel read/write of a flat file from/into RAM.
void PreadAll(const std::string& path, void* dst, uint64_t bytes) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) Die("cannot open " + path);
  std::atomic<uint64_t> cur{0};
  RunParallel([&](int) {
    constexpr uint64_t kStep = uint64_t{1} << 28;
    for (;;) {
      const uint64_t off = cur.fetch_add(kStep);
      if (off >= bytes) break;
      const uint64_t n = std::min(kStep, bytes - off);
      uint64_t got = 0;
      while (got < n) {
        const ssize_t r = ::pread(fd, static_cast<char*>(dst) + off + got, n - got,
                                  off_t(off + got));
        if (r <= 0) Die("pread failed on " + path);
        got += uint64_t(r);
      }
    }
  });
  ::close(fd);
}

void PwriteAll(const std::string& path, const void* src, uint64_t bytes) {
  int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) Die("cannot create " + path);
  std::atomic<uint64_t> cur{0};
  RunParallel([&](int) {
    constexpr uint64_t kStep = uint64_t{1} << 28;
    for (;;) {
      const uint64_t off = cur.fetch_add(kStep);
      if (off >= bytes) break;
      const uint64_t n = std::min(kStep, bytes - off);
      uint64_t put = 0;
      while (put < n) {
        const ssize_t r = ::pwrite(fd, static_cast<const char*>(src) + off + put,
                                   n - put, off_t(off + put));
        if (r <= 0) Die("pwrite failed on " + path);
        put += uint64_t(r);
      }
    }
  });
  ::fsync(fd);
  ::close(fd);
}

// ---------- chunked streaming over a layer's part files ----------

struct LayerReader {
  struct Chunk {
    int part;
    uint64_t off, n;  // in keys
  };
  std::vector<int> fds;
  std::vector<uint64_t> base;  // global index base of each part
  std::vector<Chunk> chunks;
  std::atomic<size_t> cursor{0};
  uint64_t total = 0;

  void Open(int layer) {
    base.assign(g_parts, 0);
    uint64_t b = 0;
    for (int q = 0; q < g_parts; q++) {
      const std::string p = KeysPath(layer, q);
      int fd = ::open(p.c_str(), O_RDONLY);
      if (fd < 0) Die("cannot open " + p);
      const uint64_t n = FileSize(p) / 8;
      fds.push_back(fd);
      base[q] = b;
      b += n;
      for (uint64_t off = 0; off < n; off += kChunkKeys)
        chunks.push_back({q, off, std::min(kChunkKeys, n - off)});
    }
    total = b;
  }
  ~LayerReader() {
    for (int fd : fds) ::close(fd);
  }

  // fn(keys, n, global_index_of_first_key) — called from worker threads.
  template <typename Fn>
  void Run(Fn&& fn, std::atomic<uint64_t>* progress = nullptr) {
    auto worker = [&]() {
      std::vector<uint64_t> buf(kChunkKeys);
      for (;;) {
        const size_t i = cursor.fetch_add(1);
        if (i >= chunks.size()) break;
        const Chunk& c = chunks[i];
        uint64_t got = 0;
        while (got < c.n * 8) {
          const ssize_t r =
              ::pread(fds[c.part], reinterpret_cast<char*>(buf.data()) + got,
                      c.n * 8 - got, off_t(c.off * 8 + got));
          if (r <= 0) Die("pread failed on layer part");
          got += uint64_t(r);
        }
        fn(buf.data(), c.n, base[c.part] + c.off);
        if (progress) progress->fetch_add(1);
      }
    };
    RunParallel([&](int) { worker(); });
  }
};

struct Monitor {
  std::thread th;
  std::atomic<bool> stop{false};
  Monitor(std::atomic<uint64_t>* p, uint64_t goal, std::string what) {
    th = std::thread([p, goal, what, this]() {
      int ticks = 0;
      while (!stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (++ticks % 300 != 0) continue;  // report every ~30s
        if (stop.load()) break;
        Log("%s: %.1f%%", what.c_str(),
            100.0 * p->load() / std::max<uint64_t>(1, goal));
      }
    });
  }
  ~Monitor() {
    stop.store(true);
    th.join();
  }
};

// ---------- meta ----------

void CheckMeta() {
  const std::string p = g_dir + "/meta.txt";
  if (!FileExists(p)) {
    FILE* f = std::fopen(p.c_str(), "w");
    if (!f) Die("cannot write " + p);
    std::fprintf(f, "wdlretro1 %d %d %d\n", g_w, g_h, g_parts);
    std::fclose(f);
    return;
  }
  FILE* f = std::fopen(p.c_str(), "r");
  char tag[64];
  int w, h, parts;
  if (!f || std::fscanf(f, "%63s %d %d %d", tag, &w, &h, &parts) != 4 ||
      std::strcmp(tag, "wdlretro1") != 0)
    Die("bad meta.txt in " + g_dir);
  std::fclose(f);
  if (w != g_w || h != g_h) Die("meta.txt: dir is for a different board size");
  if (parts != g_parts)
    Die("meta.txt: dir was created with --parts " + std::to_string(parts) +
        "; reuse that value");
}

// ---------- enumeration ----------

uint64_t LayerSize(int layer) {
  uint64_t n = 0;
  for (int q = 0; q < g_parts; q++) n += FileSize(KeysPath(layer, q)) / 8;
  return n;
}

void WriteRoot() {
  const uint64_t key = Encode(Position::Standard(g_w, g_h).Canonical());
  for (int q = 0; q < g_parts; q++) {
    const std::string p = KeysPath(0, q);
    if (FileExists(p)) continue;
    if (Part(key) == q) {
      PwriteAll(p, &key, 8);
    } else {
      int fd = ::open(p.c_str(), O_CREAT | O_WRONLY, 0644);
      if (fd < 0) Die("cannot create " + p);
      ::close(fd);
    }
  }
}

// Expand layer k, dedup canonical children with Part == q, dump the set to
// layer k+1 part q.
void EnumPass(int k, int q) {
  LayerReader in;
  in.Open(k);
  KeySet set;
  set.Init(in.total / g_parts + 1024, g_ram_budget * 2 / 3);
  std::atomic<uint64_t> edges{0}, progress{0};

  {
    Monitor mon(&progress, in.chunks.size(),
                "enum layer " + std::to_string(k) + " part " + std::to_string(q));
    in.Run(
        [&](const uint64_t* buf, uint64_t n, uint64_t) {
          std::vector<Move> moves;
          for (uint64_t i = 0; i < n; i++) {
            if (set.oom.load(std::memory_order_relaxed)) return;
            Position p = Decode(buf[i]);
            moves.clear();
            p.GenerateMoves(&moves);
            edges.fetch_add(moves.size(), std::memory_order_relaxed);
            for (Move m : moves) {
              const uint64_t ck = ChildKey(p, m);
              if (Part(ck) == q && !set.Insert(ck)) return;
            }
          }
        },
        &progress);
  }
  if (set.oom.load())
    Die("enum dedup set exceeded the RAM budget; rerun with larger --parts");

  const std::string dst = KeysPath(k + 1, q);
  const std::string tmp = dst + ".tmp";
  ::unlink(tmp.c_str());
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) Die("cannot write " + tmp);
  std::vector<uint64_t> out(1 << 16);
  size_t fill = 0;
  for (const auto& s : set.sh)
    for (uint64_t v : s.slots) {
      if (!v) continue;
      out[fill++] = v;
      if (fill == out.size()) {
        if (std::fwrite(out.data(), 8, fill, f) != fill)
          Die("write failed on " + tmp + " (disk full?)");
        fill = 0;
      }
    }
  if (fill && std::fwrite(out.data(), 8, fill, f) != fill)
    Die("write failed on " + tmp + " (disk full?)");
  if (std::fflush(f) != 0) Die("flush failed on " + tmp + " (disk full?)");
  if (::fsync(fileno(f)) != 0) Die("fsync " + tmp);
  std::fclose(f);
  AtomicRename(tmp, dst);
  if (FileSize(dst) != set.Count() * 8)
    Die("short write on " + dst + " (disk full?)");

  Log("enum layer %d part %d: %" PRIu64 " new positions (%" PRIu64
      " edges scanned)",
      k, q, set.Count(), edges.load());
  Touch(MarkerPath("enum." + std::to_string(k) + "." + std::to_string(q)));
}

// ---------- retrograde ----------

// Load layer `layer` part q (keys + 2-bit verdicts) into a probe set.
void BuildProbe(int layer, int q, KeySet* probe) {
  const std::string kp = KeysPath(layer, q);
  const std::string bp = BitsPath(layer, q);
  const uint64_t n = FileSize(kp) / 8;
  int kfd = ::open(kp.c_str(), O_RDONLY);
  int bfd = ::open(bp.c_str(), O_RDONLY);
  if (kfd < 0 || bfd < 0) Die("BuildProbe: missing " + kp + " or " + bp);

  std::atomic<uint64_t> cur{0};
  RunParallel([&](int) {
    std::vector<uint64_t> kb(kChunkKeys);
    std::vector<uint8_t> bb(kChunkKeys / 4 + 1);
    for (;;) {
      const uint64_t off = cur.fetch_add(kChunkKeys);
      if (off >= n) break;
      const uint64_t cnt = std::min(kChunkKeys, n - off);
      uint64_t got = 0;
      while (got < cnt * 8) {
        const ssize_t r = ::pread(kfd, reinterpret_cast<char*>(kb.data()) + got,
                                  cnt * 8 - got, off_t(off * 8 + got));
        if (r <= 0) Die("pread keys");
        got += uint64_t(r);
      }
      const uint64_t nbytes = (cnt + 3) / 4;
      got = 0;
      while (got < nbytes) {
        const ssize_t r = ::pread(bfd, reinterpret_cast<char*>(bb.data()) + got,
                                  nbytes - got, off_t(off / 4 + got));
        if (r <= 0) Die("pread bits");
        got += uint64_t(r);
      }
      for (uint64_t i = 0; i < cnt; i++) {
        const uint8_t v = uint8_t((bb[i / 4] >> ((i % 4) * 2)) & 3);
        if (v == 0) Die("BuildProbe: zero verdict in " + bp);
        if (!probe->Insert(kb[i] | (uint64_t{v} << kVerdictShift)))
          Die("BuildProbe: probe table overflow (raise --ram-gb or --parts)");
      }
    }
  });
  ::close(kfd);
  ::close(bfd);
}

// One retrograde pass: parents of layer k against children partition q.
// On completion the pass bit is folded into *pass_mask and the whole
// [mask | state] file is rewritten atomically, so a crash either keeps the
// previous checkpoint or the new one — a pass is never double-counted.
void RetroPass(int k, int q, uint16_t* state, uint64_t* pass_mask) {
  const uint64_t nchild = FileSize(KeysPath(k + 1, q)) / 8;
  if (16 * nchild > g_ram_budget * 2 / 3)
    Die("probe partition too large (raise --parts)");
  KeySet probe;
  probe.Init(nchild + 1024, g_ram_budget * 2 / 3);
  BuildProbe(k + 1, q, &probe);
  Log("retro layer %d part %d: probe built (%" PRIu64 " children)", k, q,
      probe.Count());

  LayerReader in;
  in.Open(k);
  std::atomic<uint64_t> progress{0};
  {
    Monitor mon(&progress, in.chunks.size(),
                "retro layer " + std::to_string(k) + " part " + std::to_string(q));
    in.Run(
        [&](const uint64_t* buf, uint64_t n, uint64_t gidx) {
          std::vector<Move> moves;
          for (uint64_t i = 0; i < n; i++) {
            const uint16_t s = state[gidx + i];
            if (s & kWinFlag) continue;  // already proven W
            Position p = Decode(buf[i]);
            moves.clear();
            p.GenerateMoves(&moves);
            uint16_t w = 0;
            bool found_loss = false;
            for (Move m : moves) {
              const uint64_t ck = ChildKey(p, m);
              if (Part(ck) != q) continue;
              const uint8_t v = Probe(probe, ck);
              if (v == 0)
                Die("retro: child missing from probe table (table corrupt?)");
              if (v == 2) {
                found_loss = true;
                break;  // position is W; no further counting needed
              }
              if (w == kWCountMask) Die("retro: wcount overflow");
              w++;
            }
            state[gidx + i] = found_loss ? uint16_t(s | kWinFlag) : uint16_t(s + w);
          }
        },
        &progress);
  }

  *pass_mask |= uint64_t{1} << q;
  const std::string dst = StatePath(k);
  const std::string tmp = dst + ".tmp";
  {
    int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) Die("cannot create " + tmp);
    uint64_t put = 0;
    while (put < 8) {  // header: completed-pass bitmask
      const ssize_t r = ::pwrite(fd, reinterpret_cast<const char*>(pass_mask) + put,
                                 8 - put, off_t(put));
      if (r <= 0) Die("pwrite header failed on " + tmp);
      put += uint64_t(r);
    }
    const uint64_t bytes = in.total * 2;
    std::atomic<uint64_t> cur{0};
    RunParallel([&](int) {
      constexpr uint64_t kStep = uint64_t{1} << 28;
      for (;;) {
        const uint64_t off = cur.fetch_add(kStep);
        if (off >= bytes) break;
        const uint64_t n = std::min(kStep, bytes - off);
        uint64_t done = 0;
        while (done < n) {
          const ssize_t r =
              ::pwrite(fd, reinterpret_cast<const char*>(state) + off + done,
                       n - done, off_t(8 + off + done));
          if (r <= 0) Die("pwrite failed on " + tmp);
          done += uint64_t(r);
        }
      }
    });
    ::fsync(fd);
    ::close(fd);
  }
  AtomicRename(tmp, dst);
  Log("retro layer %d part %d: pass done, state checkpointed", k, q);
}

// Finalize layer k: emit 2-bit verdict files, delete the state file.
void Finalize(int k, std::vector<uint16_t>* state, uint64_t* nw_out,
              uint64_t* nl_out) {
  LayerReader in;
  in.Open(k);
  std::atomic<uint64_t> nw{0}, nl{0};

  for (int q = 0; q < g_parts; q++) {
    const uint64_t n = FileSize(KeysPath(k, q)) / 8;
    const std::string bp = BitsPath(k, q);
    ::unlink((bp + ".tmp").c_str());
    int bfd = ::open((bp + ".tmp").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (bfd < 0) Die("cannot create " + bp + ".tmp");
    // Byte j of the bits file covers keys [4j, 4j+4).  Workers pwrite
    // disjoint ranges: chunk offsets are multiples of kChunkKeys, which is
    // divisible by 4.
    std::atomic<uint64_t> cur{0};
    RunParallel([&](int) {
      std::vector<uint64_t> kb(kChunkKeys);
      std::vector<uint8_t> bb(kChunkKeys / 4);
      std::vector<Move> moves;
      int kfd = ::open(KeysPath(k, q).c_str(), O_RDONLY);
      if (kfd < 0) Die("open keys in finalize");
      for (;;) {
        const uint64_t off = cur.fetch_add(kChunkKeys);
        if (off >= n) break;
        const uint64_t cnt = std::min(kChunkKeys, n - off);
        uint64_t got = 0;
        while (got < cnt * 8) {
          const ssize_t r =
              ::pread(kfd, reinterpret_cast<char*>(kb.data()) + got, cnt * 8 - got,
                      off_t(off * 8 + got));
          if (r <= 0) Die("pread keys in finalize");
          got += uint64_t(r);
        }
        const uint64_t nbytes = (cnt + 3) / 4;
        std::memset(bb.data(), 0, nbytes);
        for (uint64_t i = 0; i < cnt; i++) {
          const uint16_t s = (*state)[in.base[q] + off + i];
          uint8_t v;
          if (s & kWinFlag) {
            v = 1;
          } else {
            Position p = Decode(kb[i]);
            moves.clear();
            p.GenerateMoves(&moves);
            v = ((s & kWCountMask) == moves.size()) ? 2 : 0;
            if (v == 0)
              Die("finalize: unresolved position at layer " + std::to_string(k) +
                  " part " + std::to_string(q) + " index " +
                  std::to_string(off + i) + " (wcount " +
                  std::to_string(s & kWCountMask) + " != outdeg " +
                  std::to_string(moves.size()) + ")");
          }
          bb[i / 4] |= uint8_t(v << ((i % 4) * 2));
          (v == 1 ? nw : nl).fetch_add(1, std::memory_order_relaxed);
        }
        uint64_t put = 0;
        while (put < nbytes) {
          const ssize_t r = ::pwrite(bfd, reinterpret_cast<const char*>(bb.data()) + put,
                                     nbytes - put, off_t(off / 4 + put));
          if (r <= 0) Die("pwrite bits in finalize");
          put += uint64_t(r);
        }
      }
      ::close(kfd);
    });
    ::fsync(bfd);
    ::close(bfd);
    AtomicRename(bp + ".tmp", bp);
  }

  ::unlink(StatePath(k).c_str());
  Touch(MarkerPath("final." + std::to_string(k)));
  Log("final layer %d: %" PRIu64 " positions, %" PRIu64 " W, %" PRIu64 " L", k,
      nw.load() + nl.load(), nw.load(), nl.load());
  *nw_out = nw.load();
  *nl_out = nl.load();
}

// ---------- verify ----------

// Sample up to `samples` positions per layer and re-derive each stored
// verdict from the child tables.  Returns the number of mismatches.
uint64_t Verify(uint64_t samples) {
  int d = -1;
  for (int k = 0;; k++) {
    if (LayerSize(k) > 0)
      d = k;
    else
      break;
  }
  uint64_t bad = 0;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  auto next_rand = [&]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };

  for (int k = 0; k <= d; k++) {
    const uint64_t lk = LayerSize(k);
    std::vector<uint64_t> idx(std::min(samples, lk));
    for (uint64_t& i : idx) i = next_rand() % lk;
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());

    // Fetch sampled keys and their stored verdicts.
    std::vector<uint64_t> keys(idx.size(), 0);
    std::vector<uint8_t> stored(idx.size(), 0);
    uint64_t qb = 0;
    for (int q = 0; q < g_parts; q++) {
      const uint64_t nq = FileSize(KeysPath(k, q)) / 8;
      int kfd = ::open(KeysPath(k, q).c_str(), O_RDONLY);
      int bfd = ::open(BitsPath(k, q).c_str(), O_RDONLY);
      if (kfd < 0 || bfd < 0) Die("verify: missing files for layer " + std::to_string(k));
      for (size_t i = 0; i < idx.size(); i++) {
        if (idx[i] < qb || idx[i] >= qb + nq) continue;
        const uint64_t local = idx[i] - qb;
        if (::pread(kfd, &keys[i], 8, off_t(local * 8)) != 8) Die("pread verify");
        uint8_t byte;
        if (::pread(bfd, &byte, 1, off_t(local / 4)) != 1) Die("pread verify bits");
        stored[i] = uint8_t((byte >> ((local % 4) * 2)) & 3);
      }
      ::close(kfd);
      ::close(bfd);
      qb += nq;
    }
    for (uint8_t v : stored)
      if (v == 0) Die("verify: zero stored verdict (incomplete table?)");

    std::vector<uint8_t> seen_l(keys.size(), 0), seen_not_w(keys.size(), 0);
    if (k < d)  // layer d has no children (its table is all-L by construction)
      for (int q = 0; q < g_parts; q++) {
        KeySet probe;
        probe.Init(FileSize(KeysPath(k + 1, q)) / 8 + 1024, g_ram_budget * 2 / 3);
        BuildProbe(k + 1, q, &probe);
        RunParallel([&](int t) {
          std::vector<Move> moves;
          const size_t per = (keys.size() + g_threads - 1) / g_threads;
          const size_t lo = size_t(t) * per, hi = std::min(keys.size(), lo + per);
          for (size_t i = lo; i < hi; i++) {
            Position p = Decode(keys[i]);
            moves.clear();
            p.GenerateMoves(&moves);
            for (Move m : moves) {
              const uint64_t ck = ChildKey(p, m);
              if (Part(ck) != q) continue;
              const uint8_t v = Probe(probe, ck);
              if (v == 0) Die("verify: child missing from table");
              if (v == 2) seen_l[i] = 1;
              if (v != 1) seen_not_w[i] = 1;
            }
          }
        });
      }

    uint64_t layer_bad = 0;
    for (size_t i = 0; i < keys.size(); i++) {
      const uint8_t derived = seen_l[i] ? 1 : (seen_not_w[i] ? 0 : 2);
      if (derived == 0 || derived != stored[i]) {
        layer_bad++;
        if (layer_bad <= 3) {
          Position p = Decode(keys[i]);
          std::fprintf(stderr, "verify mismatch layer %d:\n%sstored=%d derived=%d\n", k,
                       p.ToString().c_str(), stored[i], derived);
        }
      }
    }
    Log("verify layer %d: %zu sampled, %" PRIu64 " mismatches", k, keys.size(),
        layer_bad);
    bad += layer_bad;
  }
  return bad;
}

// ---------- driver ----------

int DeepestLayer() {
  int d = -1;
  for (int k = 0; k <= g_nsq; k++) {
    if (LayerSize(k) > 0)
      d = k;
    else
      break;
  }
  return d;
}

uint8_t RootVerdict() {
  const uint64_t root = Encode(Position::Standard(g_w, g_h).Canonical());
  const int q = Part(root);
  if (LayerSize(0) != 1) Die("root layer corrupt");
  int bfd = ::open(BitsPath(0, q).c_str(), O_RDONLY);
  uint8_t byte = 0;
  if (bfd < 0 || ::pread(bfd, &byte, 1, 0) != 1) Die("cannot read root verdict");
  ::close(bfd);
  return byte & 3;
}

}  // namespace
}  // namespace amazons

int main(int argc, char** argv) {
  using namespace amazons;
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: wdlretro WxH DIR [--parts P] [--threads T] [--ram-gb GB]\n"
                 "                        [--expect w|l] [--verify N]\n");
    return 1;
  }
  if (std::sscanf(argv[1], "%dx%d", &g_w, &g_h) != 2) Die("bad WxH");
  g_nsq = g_w * g_h;
  if (g_nsq < 4 || g_nsq > 30) Die("need 4 <= w*h <= 30 (base-4 key limit)");
  g_dir = argv[2];
  g_parts = 4;
  g_threads = std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
  g_ram_budget = 600ull << 30;
  std::string expect;
  uint64_t verify_n = 0;
  for (int i = 3; i < argc; i++) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (++i >= argc) Die(std::string("missing value for ") + name);
      return std::string(argv[i]);
    };
    if (a == "--parts")
      g_parts = std::stoi(need("--parts"));
    else if (a == "--threads")
      g_threads = std::stoi(need("--threads"));
    else if (a == "--ram-gb")
      g_ram_budget = uint64_t(std::stoll(need("--ram-gb"))) << 30;
    else if (a == "--expect")
      expect = need("--expect");
    else if (a == "--verify")
      verify_n = std::stoull(need("--verify"));
    else
      Die("unknown option " + a);
  }
  if (g_parts < 1 || (g_parts & (g_parts - 1)) != 0 || g_parts > 64)
    Die("--parts must be a power of two in [1, 64]");

  g_t0 = Clock::now();
  ::mkdir(g_dir.c_str(), 0755);
  ::mkdir((g_dir + "/done").c_str(), 0755);
  CheckMeta();
  Log("wdlretro %dx%d dir=%s parts=%d threads=%d ram=%" PRIu64 "GB", g_w, g_h,
      g_dir.c_str(), g_parts, g_threads, g_ram_budget >> 30);
  BuildSymTables();

  // ---- forward enumeration (resumable per pass) ----
  WriteRoot();
  for (int k = 0; k <= g_nsq; k++) {
    if (LayerSize(k) == 0) break;
    for (int q = 0; q < g_parts; q++) {
      if (FileExists(MarkerPath("enum." + std::to_string(k) + "." + std::to_string(q))))
        continue;
      EnumPass(k, q);
    }
    Log("layer %d: %" PRIu64 " positions", k, LayerSize(k));
    if (LayerSize(k + 1) == 0) break;  // layer k is the deepest
  }
  const int d = DeepestLayer();
  Log("enumeration complete: deepest layer = %d, total positions = %" PRIu64, d, [&] {
    uint64_t n = 0;
    for (int k = 0; k <= d; k++) n += LayerSize(k);
    return n;
  }());

  // ---- backward retrograde (resumable per pass) ----
  for (int k = d; k >= 0; k--) {
    if (FileExists(MarkerPath("final." + std::to_string(k)))) continue;
    const uint64_t lk = LayerSize(k);
    if (2 * lk > g_ram_budget / 3)
      Die("state array too large (raise --parts)");
    std::vector<uint16_t> state(lk, 0);
    uint64_t pass_mask = 0;
    const std::string sp = StatePath(k);
    if (FileExists(sp)) {
      if (FileSize(sp) != 8 + lk * 2)
        Die("state file size mismatch for layer " + std::to_string(k));
      int fd = ::open(sp.c_str(), O_RDONLY);
      if (fd < 0 || ::pread(fd, &pass_mask, 8, 0) != 8) Die("read state header");
      uint64_t got = 0;  // body follows the 8-byte header
      while (got < lk * 2) {
        const ssize_t r = ::pread(fd, reinterpret_cast<char*>(state.data()) + got,
                                  lk * 2 - got, off_t(8 + got));
        if (r <= 0) Die("read state body");
        got += uint64_t(r);
      }
      ::close(fd);
      Log("retro layer %d: resumed state from checkpoint (mask %02" PRIx64 ")", k,
          pass_mask);
    }
    if (k < d)
      for (int q = 0; q < g_parts; q++) {
        if (pass_mask & (uint64_t{1} << q)) continue;
        RetroPass(k, q, state.data(), &pass_mask);
      }
    uint64_t nw, nl;
    Finalize(k, &state, &nw, &nl);
  }
  Log("table complete (per-layer W/L counts are in the finalize logs above)");

  // ---- root verdict ----
  const uint8_t rv = RootVerdict();
  Log("ROOT VERDICT: %s", rv == 1 ? "W (first player wins)" : "L (second player wins)");

  if (verify_n) {
    const uint64_t bad = Verify(verify_n);
    if (bad) {
      Log("verify: %" PRIu64 " MISMATCHES", bad);
      return 3;
    }
    Log("verify: all sampled positions consistent");
  }

  if (!expect.empty()) {
    const uint8_t e = (expect == "w" || expect == "W") ? 1 : 2;
    if (rv != e) {
      Log("EXPECTATION FAILED: wanted %s, got %s", expect.c_str(), rv == 1 ? "w" : "l");
      return 4;
    }
    Log("expectation %s satisfied", expect.c_str());
  }
  Log("done");
  return 0;
}
