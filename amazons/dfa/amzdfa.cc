/*
  amzdfa — symbolic (DFA-based) strong solver for small Amazons boards.

  Exploration prototype.  It is our own engine and shares no code with the
  rest of this project, so it can serve as an independent check of it.

  Model
  -----
  A position is a string of one symbol per cell, written from the point of
  view of the side to move:  0 = empty, 1 = mover's queen, 2 = opponent's
  queen, 3 = arrow.  Every move burns exactly one square, so layer k (exactly
  k arrows) only moves to layer k+1, and the side to move needs no symbol.

  With P_k = all placements of q+q queens and k arrows:

      W_k = pre(L_{k+1})       (some move reaches a position lost for the
                                opponent, who is the mover there)
      L_k = P_k \ W_k

  One preimage per layer, no fixpoint.  Positions without a legal move are
  in no preimage, so they fall into L_k by themselves.  Solving all of P_k
  rather than the reachable part needs no forward pass and gives the value
  of every placement, including every possible starting setup.

  Sets are minimal layered DFAs.  A DFA node has 4 children, which are 1-based
  indices into the next level, so 32 bits suffice per child (0 = reject; on
  the last level, 1 = accept).  Each layer is its own DFA, so only two layers
  need to be alive at once.

  The preimage is one subset construction over the entire move relation:

    * The relation is a small automaton over per-cell *requirements*
      (free, from, to, arrow, must-be-empty, from-and-arrow).  It is the
      minimal DFA of the requirement strings of all (from, to, arrow) triples.
    * A result state at level i is a sorted set of (relation state, S-node)
      pairs.  Reading a parent symbol moves every pair along every compatible
      relation arc and the matching S transition.  The union over all moves is
      therefore implicit and no pairwise DFA unions are built.
    * Level by level: expand all states of a level, deduplicating the next
      level in lock-sharded tables and recording the children; then replay
      bottom-up, hash-consing, which yields the minimal DFA.  Memory follows
      the widest level.

  Usage: amzdfa solve WxH [options]   (see Usage() below)
*/

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using Clock = std::chrono::steady_clock;
Clock::time_point g_t0;
int g_threads = 1;
double g_mem_limit_gib = 0;

double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "amzdfa: FATAL: %s\n", msg.c_str());
  std::exit(1);
}

double RssGiB() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long vm = 0, rss = 0;
  const int got = std::fscanf(f, "%lu %lu", &vm, &rss);
  std::fclose(f);
  if (got != 2) return 0;
  return double(rss) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0 * 1024.0);
}

double g_peak_rss = 0;
void MemCheck() {
  const double rss = RssGiB();
  g_peak_rss = std::max(g_peak_rss, rss);
  if (g_mem_limit_gib > 0 && rss > g_mem_limit_gib) {
    std::printf("MEMORY LIMIT: rss %.1f GiB > limit %.1f GiB, stopping\n", rss, g_mem_limit_gib);
    std::fflush(stdout);
    std::exit(3);
  }
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

// Runs fn(begin, end) over [0,n) in chunks.
template <class F>
void ParallelChunks(size_t n, size_t chunk, F&& fn) {
  if (n == 0) return;
  const size_t chunks = (n + chunk - 1) / chunk;
  ParallelFor(chunks, [&](size_t c) { fn(c * chunk, std::min(n, (c + 1) * chunk)); });
}

inline u64 Mix(u64 x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

inline u64 HashSpan(const u64* p, u32 len) {
  u64 h = 0x9E3779B97F4A7C15ULL * (u64(len) + 1);
  for (u32 i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 32;
  }
  return Mix(h);
}

struct SpinGuard {
  std::atomic_flag& f;
  explicit SpinGuard(std::atomic_flag& flag) : f(flag) {
    while (f.test_and_set(std::memory_order_acquire)) {
#if defined(__x86_64__)
      __builtin_ia32_pause();
#endif
    }
  }
  ~SpinGuard() { f.clear(std::memory_order_release); }
};

// ---------------------------------------------------------------- geometry

struct Geometry {
  int w = 0, h = 0, n = 0;
  std::vector<int> order;  // string position -> cell (cell = y * w + x)
  std::vector<int> pos;    // cell -> string position
  std::string order_name;
};

std::vector<int> MakeOrder(int w, int h, const std::string& name) {
  std::vector<int> o;
  const int n = w * h;
  auto cell = [&](int x, int y) { return y * w + x; };
  if (name == "row") {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) o.push_back(cell(x, y));
  } else if (name == "col") {
    for (int x = 0; x < w; ++x)
      for (int y = 0; y < h; ++y) o.push_back(cell(x, y));
  } else if (name == "snake") {
    for (int y = 0; y < h; ++y)
      for (int i = 0; i < w; ++i) o.push_back(cell(y % 2 ? w - 1 - i : i, y));
  } else if (name == "colsnake") {
    for (int x = 0; x < w; ++x)
      for (int i = 0; i < h; ++i) o.push_back(cell(x, x % 2 ? h - 1 - i : i));
  } else if (name == "diag") {
    for (int d = 0; d < w + h - 1; ++d)
      for (int x = 0; x < w; ++x) {
        const int y = d - x;
        if (y >= 0 && y < h) o.push_back(cell(x, y));
      }
  } else if (name == "rowsoutin") {  // rows 0, h-1, 1, h-2, ...
    for (int i = 0; i < h; ++i) {
      const int y = (i % 2 == 0) ? i / 2 : h - 1 - i / 2;
      for (int x = 0; x < w; ++x) o.push_back(cell(x, y));
    }
  } else if (name == "rowsmidout") {  // middle rows first
    for (int i = h - 1; i >= 0; --i) {
      const int y = (i % 2 == 0) ? i / 2 : h - 1 - i / 2;
      for (int x = 0; x < w; ++x) o.push_back(cell(x, y));
    }
  } else if (name == "spiralin" || name == "spiralout") {
    int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
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
  } else if (name == "ringrows") {  // outer ring by rows, then the inner board
    int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
    while (x0 <= x1 && y0 <= y1) {
      for (int x = x0; x <= x1; ++x) o.push_back(cell(x, y0));
      for (int y = y0 + 1; y < y1; ++y) {
        o.push_back(cell(x0, y));
        if (x1 > x0) o.push_back(cell(x1, y));
      }
      if (y1 > y0)
        for (int x = x0; x <= x1; ++x) o.push_back(cell(x, y1));
      ++x0, --x1, ++y0, --y1;
    }
  } else {
    // Explicit comma-separated cell list.
    size_t p = 0;
    while (p < name.size()) {
      size_t e = name.find(',', p);
      if (e == std::string::npos) e = name.size();
      o.push_back(std::atoi(name.substr(p, e - p).c_str()));
      p = e + 1;
    }
  }
  std::vector<char> seen(size_t(n), 0);
  for (int c : o) {
    if (c < 0 || c >= n || seen[size_t(c)]) Die("bad cell order: " + name);
    seen[size_t(c)] = 1;
  }
  if (int(o.size()) != n) Die("cell order does not cover the board: " + name);
  return o;
}

Geometry MakeGeometry(int w, int h, const std::string& order) {
  Geometry g;
  g.w = w;
  g.h = h;
  g.n = w * h;
  g.order = MakeOrder(w, h, order);
  g.pos.resize(size_t(g.n));
  for (int i = 0; i < g.n; ++i) g.pos[size_t(g.order[size_t(i)])] = i;
  g.order_name = order;
  return g;
}

const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
const int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

inline u8 Swap12(u8 s) { return s == 1 ? 2 : (s == 2 ? 1 : s); }

// ------------------------------------------------------------------- DFA

struct Node {
  u32 c[4];
};
static_assert(sizeof(Node) == 16);

inline bool operator==(const Node& a, const Node& b) { return std::memcmp(&a, &b, sizeof(Node)) == 0; }

struct Dfa {
  int n = 0;
  u32 root = 0;                     // 0 = empty language
  std::vector<std::vector<Node>> lv;  // lv[i][id-1]; last-level children: 1 = accept

  u64 Nodes() const {
    u64 t = 0;
    for (const auto& l : lv) t += l.size();
    return t;
  }
  u64 MaxWidth() const {
    u64 t = 0;
    for (const auto& l : lv) t = std::max<u64>(t, l.size());
    return t;
  }
  bool Contains(const u8* s) const {
    u32 id = root;
    for (int i = 0; i < n; ++i) {
      if (id == 0) return false;
      id = lv[size_t(i)][id - 1].c[s[i]];
    }
    return id != 0;
  }
};

u64 CountStrings(const Dfa& d) {
  if (d.root == 0) return 0;
  std::vector<u64> below, cur;
  for (int i = d.n - 1; i >= 0; --i) {
    const auto& l = d.lv[size_t(i)];
    cur.assign(l.size(), 0);
    const bool last = (i == d.n - 1);
    ParallelChunks(l.size(), 1 << 16, [&](size_t b, size_t e) {
      for (size_t j = b; j < e; ++j) {
        u64 t = 0;
        for (int c = 0; c < 4; ++c) {
          const u32 ch = l[j].c[c];
          if (ch) t += last ? 1 : below[ch - 1];
        }
        cur[j] = t;
      }
    });
    below.swap(cur);
  }
  return below[d.root - 1];
}

// ------------------------------------------------- level-by-level builder

// Distinct subsets of one level, lock-sharded.  A subset is a sequence of
// 64-bit items in an arena that never moves.
class SubsetTable {
 public:
  static constexpr int kShardBits = 12;
  static constexpr size_t kShards = size_t(1) << kShardBits;
  static constexpr size_t kChunkWords = size_t(1) << 18;
  static constexpr int kLocalBits = 28;

  struct Shard {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<std::unique_ptr<u64[]>> chunks;
    size_t used = kChunkWords;
    std::vector<u64> hash;
    std::vector<u64> loc;  // chunk << 36 | offset << 18 | length
    std::vector<u32> slots;

    u32 Count() const { return u32(hash.size()); }
    const u64* Data(u32 local) const {
      const u64 l = loc[local];
      return chunks[l >> 36].get() + ((l >> 18) & (kChunkWords - 1));
    }
    u32 Len(u32 local) const { return u32(loc[local] & (kChunkWords - 1)); }
    void Grow() {
      const size_t cap = slots.empty() ? 64 : slots.size() * 2;
      std::vector<u32> grown(cap, 0);
      const size_t mask = cap - 1;
      for (u32 i = 0; i < Count(); ++i) {
        size_t j = hash[i] & mask;
        while (grown[j]) j = (j + 1) & mask;
        grown[j] = i + 1;
      }
      slots.swap(grown);
    }
    size_t Bytes() const {
      return chunks.size() * kChunkWords * 8 + hash.capacity() * 16 + slots.capacity() * 4;
    }
  };

  SubsetTable() : shards_(new Shard[kShards]) {}

  // Returns (shard << kLocalBits) | local.
  u64 Insert(const u64* p, u32 len) {
    const u64 h = HashSpan(p, len);
    const size_t si = h >> (64 - kShardBits);
    Shard& s = shards_[si];
    SpinGuard guard(s.lock);
    if ((size_t(s.Count()) + 1) * 3 > s.slots.size() * 2) s.Grow();
    const size_t mask = s.slots.size() - 1;
    size_t j = h & mask;
    while (const u32 v = s.slots[j]) {
      const u32 local = v - 1;
      if (s.hash[local] == h && s.Len(local) == len && std::memcmp(s.Data(local), p, len * sizeof(u64)) == 0) {
        return (u64(si) << kLocalBits) | local;
      }
      j = (j + 1) & mask;
    }
    if (s.used + len > kChunkWords) {
      s.chunks.emplace_back(new u64[kChunkWords]);
      s.used = 0;
    }
    std::memcpy(s.chunks.back().get() + s.used, p, len * sizeof(u64));
    const u32 local = s.Count();
    if (local + 1 >= (u32(1) << kLocalBits)) Die("subset shard overflow");
    s.loc.push_back((u64(s.chunks.size() - 1) << 36) | (u64(s.used) << 18) | u64(len));
    s.hash.push_back(h);
    s.slots[j] = local + 1;
    s.used += len;
    return (u64(si) << kLocalBits) | local;
  }

  Shard& shard(size_t i) { return shards_[i]; }
  const Shard& shard(size_t i) const { return shards_[i]; }
  u64 Total() const {
    u64 t = 0;
    for (size_t i = 0; i < kShards; ++i) t += shards_[i].Count();
    return t;
  }

 private:
  std::unique_ptr<Shard[]> shards_;
};

// Interning table for the bottom-up replay.
class NodeTable {
 public:
  static constexpr int kShardBits = 10;
  static constexpr size_t kShards = size_t(1) << kShardBits;

  struct Shard {
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    std::vector<Node> nodes;
    std::vector<u32> slots;
    void Grow() {
      const size_t cap = slots.empty() ? 64 : slots.size() * 2;
      std::vector<u32> grown(cap, 0);
      const size_t mask = cap - 1;
      for (u32 i = 0; i < nodes.size(); ++i) {
        size_t j = Key(nodes[i]) & mask;
        while (grown[j]) j = (j + 1) & mask;
        grown[j] = i + 1;
      }
      slots.swap(grown);
    }
  };

  static u64 Key(const Node& n) {
    u64 a, b;
    std::memcpy(&a, &n.c[0], 8);
    std::memcpy(&b, &n.c[2], 8);
    return Mix(a * 0x9E3779B97F4A7C15ULL + Mix(b));
  }

  NodeTable() : shards_(new Shard[kShards]) {}

  // Returns (shard << 32) | local.
  u64 Intern(const Node& n) {
    const u64 k = Key(n);
    const size_t si = k >> (64 - kShardBits);
    Shard& s = shards_[si];
    SpinGuard guard(s.lock);
    if ((s.nodes.size() + 1) * 3 > s.slots.size() * 2) s.Grow();
    const size_t mask = s.slots.size() - 1;
    size_t j = k & mask;
    while (const u32 v = s.slots[j]) {
      if (s.nodes[v - 1] == n) return (u64(si) << 32) | (v - 1);
      j = (j + 1) & mask;
    }
    s.nodes.push_back(n);
    s.slots[j] = u32(s.nodes.size());
    return (u64(si) << 32) | u64(s.nodes.size() - 1);
  }

  // Concatenates shards; returns the base index of each shard.
  std::vector<u64> Flatten(std::vector<Node>* out) {
    std::vector<u64> base(kShards + 1, 0);
    for (size_t i = 0; i < kShards; ++i) base[i + 1] = base[i] + shards_[i].nodes.size();
    if (base[kShards] >= (u64(1) << 32) - 2) Die("level wider than 2^32 nodes");
    out->resize(base[kShards]);
    ParallelFor(kShards, [&](size_t i) {
      std::copy(shards_[i].nodes.begin(), shards_[i].nodes.end(), out->begin() + std::ptrdiff_t(base[i]));
      std::vector<Node>().swap(shards_[i].nodes);
      std::vector<u32>().swap(shards_[i].slots);
    });
    return base;
  }

 private:
  std::unique_ptr<Shard[]> shards_;
};

// A record of 4 children, 40 bits each: 0 = reject, 1 = accept,
// otherwise 2 + ((shard << 28) | local) in the next level's table.
struct Rec {
  u8 b[20];
  void Set(int i, u64 v) { std::memcpy(b + 5 * i, &v, 5); }
  u64 Get(int i) const {
    u64 v = 0;
    std::memcpy(&v, b + 5 * i, 5);
    return v;
  }
};
static_assert(sizeof(Rec) == 20);

struct BuildStats {
  u64 states = 0;      // subset states over all levels
  u64 items = 0;       // items over all subsets
  u64 max_len = 0;     // largest subset
  u64 max_width = 0;   // widest level of subset states
  double expand_s = 0, replay_s = 0;
  double peak_frontier_gib = 0;
  std::vector<u64> width;
};

// Expander: void(int level, const u64* subset, u32 len, std::vector<u64>* kids),
// filling kids[0..3] with canonical subsets (empty = reject).
template <class Expander>
Dfa BuildLevelwise(int n, const std::vector<u64>& start, const Expander& expand, BuildStats* stats) {
  Dfa out;
  out.n = n;
  out.lv.resize(size_t(n));
  if (start.empty()) return out;

  const double t0 = Now();
  std::vector<std::vector<std::vector<Rec>>> recs(static_cast<size_t>(n));
  auto current = std::make_unique<SubsetTable>();
  current->Insert(start.data(), u32(start.size()));
  stats->width.assign(size_t(n), 0);

  for (int i = 0; i < n; ++i) {
    auto next = std::make_unique<SubsetTable>();
    auto& level_recs = recs[size_t(i)];
    level_recs.resize(SubsetTable::kShards);
    std::atomic<u64> items{0}, max_len{0};
    ParallelFor(SubsetTable::kShards, [&](size_t si) {
      SubsetTable::Shard& shard = current->shard(si);
      const u32 count = shard.Count();
      auto& rec = level_recs[si];
      rec.resize(count);
      thread_local std::vector<u64> kids[4];
      u64 local_items = 0, local_max = 0;
      for (u32 local = 0; local < count; ++local) {
        const u32 len = shard.Len(local);
        local_items += len;
        local_max = std::max<u64>(local_max, len);
        expand(i, shard.Data(local), len, kids);
        for (int c = 0; c < 4; ++c) {
          u64 v = 0;
          if (!kids[c].empty()) {
            v = (i == n - 1) ? 1 : 2 + next->Insert(kids[c].data(), u32(kids[c].size()));
          }
          rec[local].Set(c, v);
        }
      }
      items.fetch_add(local_items, std::memory_order_relaxed);
      u64 seen = max_len.load(std::memory_order_relaxed);
      while (local_max > seen && !max_len.compare_exchange_weak(seen, local_max)) {
      }
      // Free this shard's subsets as soon as they are expanded.
      std::vector<std::unique_ptr<u64[]>>().swap(shard.chunks);
      std::vector<u32>().swap(shard.slots);
    });
    const u64 width = current->Total();
    stats->width[size_t(i)] = width;
    stats->states += width;
    stats->items += items.load();
    stats->max_len = std::max(stats->max_len, max_len.load());
    stats->max_width = std::max(stats->max_width, width);
    size_t frontier = 0;
    for (size_t s = 0; s < SubsetTable::kShards; ++s) frontier += next->shard(s).Bytes();
    stats->peak_frontier_gib = std::max(stats->peak_frontier_gib, double(frontier) / (1ULL << 30));
    current = std::move(next);
    MemCheck();
  }
  current.reset();
  stats->expand_s += Now() - t0;

  const double t1 = Now();
  std::vector<std::vector<u32>> canon_next;  // per shard, level i+1
  for (int i = n - 1; i >= 0; --i) {
    auto& level_recs = recs[size_t(i)];
    std::vector<std::vector<u64>> tmp(SubsetTable::kShards);
    NodeTable table;
    ParallelFor(SubsetTable::kShards, [&](size_t si) {
      auto& rec = level_recs[si];
      auto& t = tmp[si];
      t.resize(rec.size());
      for (size_t local = 0; local < rec.size(); ++local) {
        Node node;
        bool any = false;
        for (int c = 0; c < 4; ++c) {
          const u64 v = rec[local].Get(c);
          u32 id = 0;
          if (v == 1) {
            id = 1;
          } else if (v >= 2) {
            const u64 e = v - 2;
            id = canon_next[e >> SubsetTable::kLocalBits][e & ((u64(1) << SubsetTable::kLocalBits) - 1)];
          }
          node.c[c] = id;
          any = any || id != 0;
        }
        t[local] = any ? table.Intern(node) + 1 : 0;
      }
      std::vector<Rec>().swap(rec);
    });
    std::vector<std::vector<Rec>>().swap(level_recs);
    const std::vector<u64> base = table.Flatten(&out.lv[size_t(i)]);
    std::vector<std::vector<u32>> canon(SubsetTable::kShards);
    ParallelFor(SubsetTable::kShards, [&](size_t si) {
      auto& t = tmp[si];
      auto& c = canon[si];
      c.resize(t.size());
      for (size_t j = 0; j < t.size(); ++j) {
        if (t[j] == 0) {
          c[j] = 0;
        } else {
          const u64 e = t[j] - 1;
          c[j] = u32(base[e >> 32] + (e & 0xFFFFFFFFULL) + 1);
        }
      }
      std::vector<u64>().swap(t);
    });
    canon_next.swap(canon);
    MemCheck();
  }
  // Level 0 has a single state: the start subset.
  for (const auto& c : canon_next)
    if (!c.empty()) out.root = c[0];
  stats->replay_s += Now() - t1;
  return out;
}

// ------------------------------------------------------- move relation

enum Req : u8 { kFree = 0, kFrom = 1, kTo = 2, kShot = 3, kEmpty = 4, kFromShot = 5, kReqs = 6 };

struct Arc {
  u16 next;
  u8 in;
};

struct Relation {
  int n = 0;
  u32 root = 0;
  u64 moves = 0;
  u64 states = 0;
  u64 max_states = 0;
  // dir 0 = preimage (out = parent symbol, in = child symbol in the child's
  // own frame); dir 1 = image (out = child symbol in the child's frame, in =
  // parent symbol).  off[dir][i][state * 4 + out] ... indexes arcs[dir][i].
  std::vector<u32> off[2][64];
  std::vector<Arc> arcs[2][64];
};

// (parent symbol, child symbol in the parent's frame) for each requirement.
void ReqPairs(int r, std::vector<std::pair<u8, u8>>* out) {
  out->clear();
  switch (r) {
    case kFree:
      for (u8 s = 0; s < 4; ++s) out->push_back({s, s});
      break;
    case kFrom:
      out->push_back({1, 0});
      break;
    case kTo:
      out->push_back({0, 1});
      break;
    case kShot:
      out->push_back({0, 3});
      break;
    case kEmpty:
      out->push_back({0, 0});
      break;
    case kFromShot:
      out->push_back({1, 3});
      break;
    default:
      Die("bad requirement");
  }
}

// With from_filter >= 0, only moves of a queen standing on that cell.
Relation BuildRelation(const Geometry& g, int from_filter = -1) {
  // Requirement strings of all (from, to, shot) triples, in string order.
  std::vector<std::vector<u8>> moves;
  const int w = g.w, h = g.h, n = g.n;
  auto cell = [&](int x, int y) { return y * w + x; };
  for (int fy = 0; fy < h; ++fy)
    for (int fx = 0; fx < w; ++fx) {
      const int f = cell(fx, fy);
      if (from_filter >= 0 && f != from_filter) continue;
      for (int d1 = 0; d1 < 8; ++d1) {
        std::vector<int> ray1;
        for (int tx = fx + kDx[d1], ty = fy + kDy[d1]; tx >= 0 && tx < w && ty >= 0 && ty < h;
             tx += kDx[d1], ty += kDy[d1]) {
          const int t = cell(tx, ty);
          for (int d2 = 0; d2 < 8; ++d2) {
            std::vector<int> ray2;
            for (int ax = tx + kDx[d2], ay = ty + kDy[d2]; ax >= 0 && ax < w && ay >= 0 && ay < h;
                 ax += kDx[d2], ay += kDy[d2]) {
              const int a = cell(ax, ay);
              std::vector<u8> req(static_cast<size_t>(n), kFree);
              for (int c : ray1) req[size_t(g.pos[size_t(c)])] = kEmpty;
              for (int c : ray2) req[size_t(g.pos[size_t(c)])] = kEmpty;
              req[size_t(g.pos[size_t(t)])] = kTo;
              req[size_t(g.pos[size_t(a)])] = kShot;
              req[size_t(g.pos[size_t(f)])] = (a == f) ? kFromShot : kFrom;
              moves.push_back(std::move(req));
              ray2.push_back(a);
            }
          }
          ray1.push_back(t);
        }
      }
    }

  // Minimal layered DFA of the requirement strings.
  std::vector<std::map<std::array<int, kReqs>, int>> intern(static_cast<size_t>(n));
  std::function<int(int, const std::vector<int>&)> build = [&](int i, const std::vector<int>& group) -> int {
    if (i == n) return 0;
    std::array<std::vector<int>, kReqs> parts;
    for (int m : group) parts[moves[size_t(m)][size_t(i)]].push_back(m);
    std::array<int, kReqs> key;
    for (int r = 0; r < kReqs; ++r) key[size_t(r)] = parts[size_t(r)].empty() ? -1 : build(i + 1, parts[size_t(r)]);
    auto& table = intern[size_t(i)];
    const auto found = table.find(key);
    if (found != table.end()) return found->second;
    const int id = int(table.size());
    table.emplace(key, id);
    return id;
  };
  std::vector<int> all(moves.size());
  for (size_t i = 0; i < moves.size(); ++i) all[i] = int(i);
  Relation R;
  R.n = n;
  R.moves = moves.size();
  R.root = u32(build(0, all));

  std::vector<std::pair<u8, u8>> pairs;
  for (int i = 0; i < n; ++i) {
    const auto& table = intern[size_t(i)];
    const size_t states = table.size();
    R.states += states;
    R.max_states = std::max<u64>(R.max_states, states);
    if (states >= 65535) Die("relation automaton too wide for 16-bit states");
    for (int dir = 0; dir < 2; ++dir) {
      std::vector<std::vector<Arc>> per(states * 4);
      for (const auto& [key, id] : table) {
        for (int r = 0; r < kReqs; ++r) {
          if (key[size_t(r)] < 0) continue;
          ReqPairs(r, &pairs);
          for (const auto& [a, b] : pairs) {
            const int out = dir == 0 ? a : Swap12(b);
            const u8 in = dir == 0 ? Swap12(b) : a;
            per[size_t(id) * 4 + size_t(out)].push_back(Arc{u16(key[size_t(r)]), in});
          }
        }
      }
      auto& off = R.off[dir][i];
      auto& arcs = R.arcs[dir][i];
      off.assign(states * 4 + 1, 0);
      for (size_t s = 0; s < per.size(); ++s) {
        off[s] = u32(arcs.size());
        arcs.insert(arcs.end(), per[s].begin(), per[s].end());
      }
      off[states * 4] = u32(arcs.size());
    }
  }
  return R;
}

constexpr u64 kNodeMask = (u64(1) << 48) - 1;

// Image (dir 1) or preimage (dir 0) of S under the whole move relation.
Dfa ApplyRelation(const Dfa& S, const Relation& R, int dir, BuildStats* stats) {
  if (S.root == 0) {
    Dfa e;
    e.n = S.n;
    e.lv.resize(size_t(S.n));
    return e;
  }
  const std::vector<u64> start{(u64(R.root) << 48) | S.root};
  auto expand = [&](int i, const u64* p, u32 len, std::vector<u64>* kids) {
    const Node* nodes = S.lv[size_t(i)].data();
    for (u32 j = 0; j < len; ++j) __builtin_prefetch(nodes + (p[j] & kNodeMask) - 1);
    for (int c = 0; c < 4; ++c) kids[c].clear();
    const u32* off = R.off[dir][i].data();
    const Arc* arcs = R.arcs[dir][i].data();
    for (u32 j = 0; j < len; ++j) {
      const u32 tau = u32(p[j] >> 48);
      const Node& node = nodes[(p[j] & kNodeMask) - 1];
      for (u32 o = 0; o < 4; ++o) {
        for (u32 a = off[tau * 4 + o]; a < off[tau * 4 + o + 1]; ++a) {
          const u32 c = node.c[arcs[a].in];
          if (c) kids[o].push_back((u64(arcs[a].next) << 48) | c);
        }
      }
    }
    for (int c = 0; c < 4; ++c) {
      if (kids[c].size() > 1) {
        std::sort(kids[c].begin(), kids[c].end());
        kids[c].erase(std::unique(kids[c].begin(), kids[c].end()), kids[c].end());
      }
    }
  };
  return BuildLevelwise(S.n, start, expand, stats);
}

Dfa Intersect(const Dfa& A, const Dfa& B, BuildStats* stats) {
  if (A.root == 0 || B.root == 0) {
    Dfa e;
    e.n = A.n;
    e.lv.resize(size_t(A.n));
    return e;
  }
  const std::vector<u64> start{A.root, B.root};
  auto expand = [&](int i, const u64* p, u32, std::vector<u64>* kids) {
    const Node& na = A.lv[size_t(i)][p[0] - 1];
    const Node& nb = B.lv[size_t(i)][p[1] - 1];
    for (int c = 0; c < 4; ++c) {
      kids[c].clear();
      if (na.c[c] && nb.c[c]) {
        kids[c].push_back(na.c[c]);
        kids[c].push_back(nb.c[c]);
      }
    }
  };
  return BuildLevelwise(A.n, start, expand, stats);
}

// Level-wise union of minimal DFAs: a state is a sorted set of (DFA, node).
Dfa UnionAll(const std::vector<Dfa>& parts, int n, BuildStats* stats) {
  std::vector<u64> start;
  for (size_t d = 0; d < parts.size(); ++d)
    if (parts[d].root) start.push_back((u64(d) << 48) | parts[d].root);
  if (start.empty()) {
    Dfa e;
    e.n = n;
    e.lv.resize(size_t(n));
    return e;
  }
  auto expand = [&](int i, const u64* p, u32 len, std::vector<u64>* kids) {
    for (int c = 0; c < 4; ++c) kids[c].clear();
    for (u32 j = 0; j < len; ++j) {
      const size_t d = size_t(p[j] >> 48);
      const Node& node = parts[d].lv[size_t(i)][(p[j] & kNodeMask) - 1];
      for (int c = 0; c < 4; ++c)
        if (node.c[c]) kids[c].push_back((u64(d) << 48) | node.c[c]);
    }
  };
  return BuildLevelwise(n, start, expand, stats);
}

// The same preimage in two stages: one DFA per moving-queen cell, minimized,
// then a union.  Unions then work on canonical nodes, so different ways of
// writing one language cannot multiply states.
Dfa SplitPreimage(const Dfa& S, const std::vector<Relation>& rels, BuildStats* stats, u64* union_states) {
  std::vector<Dfa> parts;
  parts.reserve(rels.size());
  for (const Relation& r : rels) {
    parts.push_back(ApplyRelation(S, r, 0, stats));
    MemCheck();
  }
  BuildStats us;
  Dfa out = UnionAll(parts, S.n, &us);
  *union_states = us.states;
  stats->expand_s += us.expand_s;
  stats->replay_s += us.replay_s;
  stats->states += us.states;
  stats->items += us.items;
  return out;
}

// ------------------------------------------------------------ complement

inline u16 PackCtx(int w, int b, int a) { return u16((w << 9) | (b << 6) | a); }
inline int CtxW(u16 c) { return c >> 9; }
inline int CtxB(u16 c) { return (c >> 6) & 7; }
inline int CtxA(u16 c) { return c & 63; }
constexpr size_t kCtxMax = size_t(1) << 12;

inline bool CtxAllows(u16 ctx, int sym, int remaining) {
  const int w = CtxW(ctx), b = CtxB(ctx), a = CtxA(ctx);
  switch (sym) {
    case 0:
      return w + b + a <= remaining - 1;
    case 1:
      return w > 0;
    case 2:
      return b > 0;
    default:
      return a > 0;
  }
}

inline u16 CtxAfter(u16 ctx, int sym) {
  switch (sym) {
    case 1:
      return u16(ctx - (1 << 9));
    case 2:
      return u16(ctx - (1 << 6));
    case 3:
      return u16(ctx - 1);
    default:
      return ctx;
  }
}

// P_k \ A, where every string of A has q mover queens, q opponent queens and
// k arrows.  Linear: complements map one-to-one onto the nodes of A, plus the
// universe nodes U(level, remaining counts).
Dfa Complement(const Dfa& A, int n, int q, int k) {
  const u16 root_ctx = PackCtx(q, q, k);
  if (q + q + k > n) Die("layer does not fit on the board");
  std::vector<std::vector<u16>> ctx(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) ctx[size_t(i)].assign(A.lv[size_t(i)].size(), 0);
  std::vector<std::vector<u8>> need(static_cast<size_t>(n) + 1, std::vector<u8>(kCtxMax, 0));
  if (A.root) {
    ctx[0][A.root - 1] = root_ctx;
  } else {
    need[0][root_ctx] = 1;
  }
  for (int i = 0; i < n; ++i) {
    const auto& l = A.lv[size_t(i)];
    const int remaining = n - i;
    ParallelChunks(l.size(), 1 << 14, [&](size_t b, size_t e) {
      for (size_t j = b; j < e; ++j) {
        const u16 cx = ctx[size_t(i)][j];
        for (int c = 0; c < 4; ++c) {
          if (!CtxAllows(cx, c, remaining)) continue;
          const u16 after = CtxAfter(cx, c);
          const u32 ch = l[j].c[c];
          if (ch == 0) {
            std::atomic_ref<u8>(need[size_t(i) + 1][after]).store(1, std::memory_order_relaxed);
          } else if (i + 1 < n) {
            std::atomic_ref<u16>(ctx[size_t(i) + 1][ch - 1]).store(after, std::memory_order_relaxed);
          }
        }
      }
    });
    for (size_t cx = 0; cx < kCtxMax; ++cx) {
      if (!need[size_t(i)][cx]) continue;
      for (int c = 0; c < 4; ++c)
        if (CtxAllows(u16(cx), c, remaining)) need[size_t(i) + 1][CtxAfter(u16(cx), c)] = 1;
    }
  }

  Dfa out;
  out.n = n;
  out.lv.resize(size_t(n));
  std::vector<u32> uid_next(kCtxMax, 0), map_next;
  constexpr size_t kChunk = size_t(1) << 16;
  for (int i = n - 1; i >= 0; --i) {
    const auto& l = A.lv[size_t(i)];
    const int remaining = n - i;
    const bool last = i == n - 1;
    auto& res = out.lv[size_t(i)];
    std::vector<u32> uid(kCtxMax, 0);
    for (size_t cx = 0; cx < kCtxMax; ++cx) {
      if (!need[size_t(i)][cx]) continue;
      Node node{};
      for (int c = 0; c < 4; ++c) {
        if (CtxAllows(u16(cx), c, remaining)) node.c[c] = last ? 1 : uid_next[CtxAfter(u16(cx), c)];
      }
      res.push_back(node);
      uid[cx] = u32(res.size());
    }
    const u64 base_u = res.size();
    std::vector<u32> map(l.size(), 0);
    const size_t chunks = (l.size() + kChunk - 1) / kChunk;
    std::vector<std::vector<Node>> parts(chunks);
    ParallelFor(chunks, [&](size_t chunk) {
      const size_t b = chunk * kChunk, e = std::min(l.size(), b + kChunk);
      auto& part = parts[chunk];
      for (size_t j = b; j < e; ++j) {
        const u16 cx = ctx[size_t(i)][j];
        Node node{};
        bool any = false;
        for (int c = 0; c < 4; ++c) {
          if (!CtxAllows(cx, c, remaining)) continue;
          const u32 ch = l[j].c[c];
          u32 id;
          if (last) {
            id = ch ? 0 : 1;
          } else {
            id = ch ? map_next[ch - 1] : uid_next[CtxAfter(cx, c)];
          }
          node.c[c] = id;
          any = any || id != 0;
        }
        if (any) {
          part.push_back(node);
          map[j] = u32(part.size());
        }
      }
    });
    std::vector<u64> base(chunks + 1, base_u);
    for (size_t c = 0; c < chunks; ++c) base[c + 1] = base[c] + parts[c].size();
    res.resize(base[chunks]);
    ParallelFor(chunks, [&](size_t chunk) {
      std::copy(parts[chunk].begin(), parts[chunk].end(), res.begin() + std::ptrdiff_t(base[chunk]));
      const size_t b = chunk * kChunk, e = std::min(l.size(), b + kChunk);
      for (size_t j = b; j < e; ++j)
        if (map[j]) map[j] = u32(base[chunk] + map[j]);
      std::vector<Node>().swap(parts[chunk]);
    });
    map_next.swap(map);
    uid_next.swap(uid);
    if (i == 0) out.root = A.root ? map_next[A.root - 1] : uid_next[root_ctx];
  }
  return out;
}

// ---------------------------------------------------- explicit reference

// True iff the position (grid[cell] in the mover's frame) has a legal move to
// a string in `child_lost` (child's frame).  This deliberately does not use
// the relation automaton: it walks rays on an explicit grid.
bool ExplicitWin(const Geometry& g, u8* grid, const Dfa& child_lost) {
  std::vector<u8> s(static_cast<size_t>(g.n));
  auto probe = [&]() {
    for (int i = 0; i < g.n; ++i) s[size_t(i)] = Swap12(grid[g.order[size_t(i)]]);
    return child_lost.Contains(s.data());
  };
  for (int f = 0; f < g.n; ++f) {
    if (grid[f] != 1) continue;
    const int fx = f % g.w, fy = f / g.w;
    grid[f] = 0;
    for (int d1 = 0; d1 < 8; ++d1) {
      for (int x = fx + kDx[d1], y = fy + kDy[d1]; x >= 0 && x < g.w && y >= 0 && y < g.h;
           x += kDx[d1], y += kDy[d1]) {
        const int t = y * g.w + x;
        if (grid[t] != 0) break;
        grid[t] = 1;
        for (int d2 = 0; d2 < 8; ++d2) {
          for (int ax = x + kDx[d2], ay = y + kDy[d2]; ax >= 0 && ax < g.w && ay >= 0 && ay < g.h;
               ax += kDx[d2], ay += kDy[d2]) {
            const int a = ay * g.w + ax;
            if (grid[a] != 0) break;
            grid[a] = 3;
            const bool hit = probe();
            grid[a] = 0;
            if (hit) {
              grid[t] = 0;
              grid[f] = 1;
              return true;
            }
          }
        }
        grid[t] = 0;
      }
    }
    grid[f] = 1;
  }
  return false;
}

// Relabels N uniformly random members of P_k from their successors with the
// explicit rules and compares with the DFAs.
u64 SampleCheck(const Geometry& g, int q, int k, const Dfa& W, const Dfa& L, const Dfa& child_lost, u64 samples) {
  std::atomic<u64> bad{0};
  const size_t threads = size_t(std::max(1, g_threads));
  ParallelFor(threads, [&](size_t t) {
    std::mt19937_64 rng(0x5EEDULL * (u64(k) + 1) + t);
    std::vector<u8> grid;
    grid.reserve(size_t(g.n));
    for (int i = 0; i < q; ++i) grid.push_back(1);
    for (int i = 0; i < q; ++i) grid.push_back(2);
    for (int i = 0; i < k; ++i) grid.push_back(3);
    grid.resize(size_t(g.n), 0);
    std::vector<u8> s(static_cast<size_t>(g.n));
    const u64 todo = samples / threads + 1;
    for (u64 it = 0; it < todo; ++it) {
      std::shuffle(grid.begin(), grid.end(), rng);
      for (int i = 0; i < g.n; ++i) s[size_t(i)] = grid[size_t(g.order[size_t(i)])];
      const bool in_w = W.Contains(s.data());
      const bool in_l = L.Contains(s.data());
      const bool truth = ExplicitWin(g, grid.data(), child_lost);
      if (in_w == in_l || in_w != truth) bad.fetch_add(1);
    }
  });
  return bad.load();
}

// Checks every member of P_k.
u64 ExhaustiveCheck(const Geometry& g, int q, int k, const Dfa& W, const Dfa& L, const Dfa& child_lost, u64* checked) {
  std::atomic<u64> bad{0}, seen{0};
  const int split = std::min(g.n, 5);
  size_t tasks = 1;
  for (int i = 0; i < split; ++i) tasks *= 4;
  ParallelFor(tasks, [&](size_t task) {
    std::vector<u8> grid(static_cast<size_t>(g.n), 0), s(static_cast<size_t>(g.n), 0);
    int qm = q, qo = q, ar = k;
    size_t code = task;
    for (int i = 0; i < split; ++i) {
      const u8 sym = u8(code & 3);
      code >>= 2;
      s[size_t(i)] = sym;
      if (sym == 1) --qm;
      if (sym == 2) --qo;
      if (sym == 3) --ar;
    }
    if (qm < 0 || qo < 0 || ar < 0) return;
    u64 local_seen = 0, local_bad = 0;
    std::function<void(int, int, int, int)> rec = [&](int i, int m, int o, int a) {
      if (m + o + a > g.n - i) return;
      if (i == g.n) {
        for (int j = 0; j < g.n; ++j) grid[size_t(g.order[size_t(j)])] = s[size_t(j)];
        const bool in_w = W.Contains(s.data());
        const bool in_l = L.Contains(s.data());
        const bool truth = ExplicitWin(g, grid.data(), child_lost);
        ++local_seen;
        if (in_w == in_l || in_w != truth) ++local_bad;
        return;
      }
      s[size_t(i)] = 0;
      rec(i + 1, m, o, a);
      if (m) {
        s[size_t(i)] = 1;
        rec(i + 1, m - 1, o, a);
      }
      if (o) {
        s[size_t(i)] = 2;
        rec(i + 1, m, o - 1, a);
      }
      if (a) {
        s[size_t(i)] = 3;
        rec(i + 1, m, o, a - 1);
      }
    };
    rec(split, qm, qo, ar);
    seen.fetch_add(local_seen);
    bad.fetch_add(local_bad);
  });
  *checked = seen.load();
  return bad.load();
}

// ------------------------------------------------------------ persistence

void SaveDfa(const Dfa& d, const std::string& path) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) Die("cannot create " + tmp);
  const u64 header[3] = {0x31414644'5A4D41ULL, u64(d.n), u64(d.root)};
  std::fwrite(header, sizeof(u64), 3, f);
  for (const auto& l : d.lv) {
    const u64 size = l.size();
    std::fwrite(&size, sizeof(u64), 1, f);
    if (size && std::fwrite(l.data(), sizeof(Node), size, f) != size) Die("short write: " + tmp);
  }
  if (std::fclose(f) != 0) Die("close failed: " + tmp);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) Die("rename failed: " + path);
}

bool LoadDfa(const std::string& path, Dfa* d) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  u64 header[3];
  if (std::fread(header, sizeof(u64), 3, f) != 3 || header[0] != 0x31414644'5A4D41ULL) Die("bad DFA file: " + path);
  d->n = int(header[1]);
  d->root = u32(header[2]);
  d->lv.assign(size_t(d->n), {});
  for (auto& l : d->lv) {
    u64 size = 0;
    if (std::fread(&size, sizeof(u64), 1, f) != 1) Die("truncated DFA file: " + path);
    l.resize(size);
    if (size && std::fread(l.data(), sizeof(Node), size, f) != size) Die("truncated DFA file: " + path);
  }
  std::fclose(f);
  return true;
}

// ----------------------------------------------------------------- driver

std::vector<u8> StartString(const Geometry& g) {
  std::vector<u8> grid(static_cast<size_t>(g.n), 0), s(static_cast<size_t>(g.n), 0);
  const int w = g.w, h = g.h;
  auto set = [&](int x, int y, u8 v) { grid[size_t(y * w + x)] = v; };
  set(1, 0, 1);
  set(w - 2, 0, 1);
  set(0, 1, 1);
  set(w - 1, 1, 1);
  set(0, h - 2, 2);
  set(w - 1, h - 2, 2);
  set(1, h - 1, 2);
  set(w - 2, h - 1, 2);
  for (int i = 0; i < g.n; ++i) s[size_t(i)] = grid[size_t(g.order[size_t(i)])];
  return s;
}

Dfa SingleString(const std::vector<u8>& s) {
  Dfa d;
  d.n = int(s.size());
  d.lv.resize(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    Node node{};
    node.c[s[i]] = 1;
    d.lv[i].push_back(node);
  }
  d.root = 1;
  return d;
}

std::string WidthProfile(const Dfa& d) {
  std::string out;
  for (size_t i = 0; i < d.lv.size(); ++i) {
    if (i) out += ' ';
    out += std::to_string(d.lv[i].size());
  }
  return out;
}

u64 Binomial(int n, int k) {
  if (k < 0 || k > n) return 0;
  unsigned __int128 r = 1;
  for (int i = 1; i <= k; ++i) r = r * (unsigned __int128)(n - k + i) / (unsigned __int128)i;
  return u64(r);
}

struct Options {
  int w = 0, h = 0, q = 4;
  std::string order = "row";
  int kmin = 0;
  u64 sample = 0;
  bool exhaustive = false;
  bool widths = false;
  bool reach = false;
  std::string save_dir;
  bool keep_all = false;
  double max_seconds = 0;
  int forward_only = -1;  // >= 0: only enumerate reachable layers up to this k
  bool split_from = false;
};

void Usage() {
  std::puts(
      "usage: amzdfa solve WxH [--order NAME] [--queens Q] [--threads T] [--kmin K]\n"
      "         [--sample N] [--exhaustive] [--widths] [--reach] [--mem-gib G]\n"
      "         [--save DIR [--keep-all]] [--max-seconds S] [--forward-only K] [--split-from]\n"
      "  orders: row col snake colsnake diag rowsoutin rowsmidout spiralin spiralout ringrows\n"
      "          or an explicit comma-separated cell list\n"
      "  --kmin K     stop after layer K (0 = solve the whole game)\n"
      "  --sample N   relabel N random positions per layer with the explicit rules\n"
      "  --exhaustive relabel every position of every layer\n"
      "  --reach      also compute reachable sets and their intersection with W/L\n"
      "  --forward-only K  print reachable-set sizes up to layer K, then stop\n"
      "  --split-from one preimage per moving-queen cell, then a union\n"
      "  --save DIR   keep the last computed L layer there (resume from it);\n"
      "               with --keep-all keep every W and L layer\n");
}

int Solve(const Options& opt) {
  const Geometry g = MakeGeometry(opt.w, opt.h, opt.order);
  const int n = g.n, q = opt.q;
  const int K = n - 2 * q;
  if (K < 0) Die("board too small for the queens");
  if (n > 64) Die("board too large");

  const double rel_t = Now();
  const Relation R = BuildRelation(g);
  std::printf("# board %dx%d q=%d order=%s threads=%d cells=%d layers=0..%d\n", opt.w, opt.h, q, g.order_name.c_str(),
              g_threads, n, K);
  std::printf("# relation: %" PRIu64 " moves, %" PRIu64 " automaton states (widest level %" PRIu64 "), built in %.2fs\n",
              R.moves, R.states, R.max_states, Now() - rel_t);
  std::printf(
      "#%3s %17s %17s %17s %13s %13s %11s %13s %7s %7s %8s %8s %8s %7s\n", "k", "|P_k|", "|W_k|", "|L_k|", "nodes(W)",
      "nodes(L)", "width(L)", "subsets", "pairs/s", "maxpair", "expand_s", "replay_s", "total_s", "rssGiB");
  std::fflush(stdout);

  if (opt.forward_only >= 0) {
    Dfa cur = SingleString(StartString(g));
    for (int k = 0; k <= std::min(opt.forward_only, K); ++k) {
      const double t = Now();
      BuildStats st;
      Dfa next;
      if (k < std::min(opt.forward_only, K)) next = ApplyRelation(cur, R, 1, &st);
      const u64 p = Binomial(n, q) * Binomial(n - q, q) * Binomial(n - 2 * q, k);
      const u64 count = CountStrings(cur);
      std::printf("R k=%d raw=%" PRIu64 " share_of_P=%.6f nodes=%" PRIu64 " width=%" PRIu64 " next_subsets=%" PRIu64 " rss=%.1f %.1fs\n", k,
                  count, double(count) / double(p), cur.Nodes(), cur.MaxWidth(), st.states, RssGiB(), Now() - t);
      std::fflush(stdout);
      MemCheck();
      cur = std::move(next);
    }
    return 0;
  }

  std::vector<Dfa> reach;
  if (opt.reach) {
    reach.resize(size_t(K) + 1);
    reach[0] = SingleString(StartString(g));
    for (int k = 0; k < K; ++k) {
      if (reach[size_t(k)].root == 0) {
        reach[size_t(k) + 1] = Dfa{};
        reach[size_t(k) + 1].n = n;
        reach[size_t(k) + 1].lv.resize(size_t(n));
        continue;
      }
      BuildStats st;
      const double t = Now();
      reach[size_t(k) + 1] = ApplyRelation(reach[size_t(k)], R, 1, &st);
      std::printf("# reach k=%d: |R|=%" PRIu64 " nodes=%" PRIu64 " width=%" PRIu64 " subsets=%" PRIu64 " %.1fs\n", k + 1,
                  CountStrings(reach[size_t(k) + 1]), reach[size_t(k) + 1].Nodes(), reach[size_t(k) + 1].MaxWidth(),
                  st.states, Now() - t);
      std::fflush(stdout);
    }
  }

  Dfa Lnext;
  int top = K;
  if (!opt.save_dir.empty()) {
    for (int k = opt.kmin; k <= K; ++k) {
      Dfa loaded;
      if (LoadDfa(opt.save_dir + "/L" + std::to_string(k) + ".dfa", &loaded)) {
        Lnext = std::move(loaded);
        top = k - 1;
        std::printf("# resumed from layer %d (%" PRIu64 " nodes)\n", k, Lnext.Nodes());
        break;
      }
    }
  }
  if (top == K) {
    Dfa empty;
    empty.n = n;
    empty.lv.resize(size_t(n));
    Lnext = Complement(empty, n, q, K);
    const u64 count = CountStrings(Lnext);
    const u64 expected = Binomial(n, q) * Binomial(n - q, q);
    if (count != expected) Die("top layer count mismatch");
    std::printf(" %3d %17" PRIu64 " %17d %17" PRIu64 " %13d %13" PRIu64 " %11" PRIu64 "\n", K, count, 0, count, 0, Lnext.Nodes(),
                Lnext.MaxWidth());
    std::fflush(stdout);
    if (opt.reach) {
      BuildStats st;
      const Dfa rl = Intersect(reach[size_t(K)], Lnext, &st);
      std::printf("# reachL k=%d |RL|=%" PRIu64 " nodes=%" PRIu64 "\n", K, CountStrings(rl), rl.Nodes());
    }
    --top;
  }

  std::vector<Relation> by_from;
  if (opt.split_from) {
    for (int f = 0; f < n; ++f) by_from.push_back(BuildRelation(g, f));
  }
  bool stopped = false;
  for (int k = top; k >= opt.kmin; --k) {
    const double t0 = Now();
    BuildStats st;
    u64 union_states = 0;
    Dfa W = opt.split_from ? SplitPreimage(Lnext, by_from, &st, &union_states) : ApplyRelation(Lnext, R, 0, &st);
    if (opt.split_from) std::printf("#  split k=%d: union states %" PRIu64 " of %" PRIu64 "\n", k, union_states, st.states);
    Dfa L = Complement(W, n, q, k);
    const u64 cw = CountStrings(W), cl = CountStrings(L);
    const u64 p = Binomial(n, q) * Binomial(n - q, q) * Binomial(n - 2 * q, k);
    if (cw + cl != p) Die("layer " + std::to_string(k) + ": |W|+|L| != |P|");
    MemCheck();
    std::printf(" %3d %17" PRIu64 " %17" PRIu64 " %17" PRIu64 " %13" PRIu64 " %13" PRIu64 " %11" PRIu64 " %13" PRIu64
                " %7.1f %7" PRIu64 " %8.1f %8.1f %8.1f %7.1f\n",
                k, p, cw, cl, W.Nodes(), L.Nodes(), L.MaxWidth(), st.states,
                st.states ? double(st.items) / double(st.states) : 0.0, st.max_len, st.expand_s, st.replay_s, Now() - t0,
                g_peak_rss);
    if (opt.widths) {
      std::printf("#  subset-widths k=%d:", k);
      for (u64 x : st.width) std::printf(" %" PRIu64, x);
      std::printf("\n#  W-widths k=%d: %s\n", k, WidthProfile(W).c_str());
    }
    if (opt.sample) {
      const u64 bad = SampleCheck(g, q, k, W, L, Lnext, opt.sample);
      if (bad) Die("sample check failed at layer " + std::to_string(k));
    }
    if (opt.exhaustive) {
      u64 checked = 0;
      const u64 bad = ExhaustiveCheck(g, q, k, W, L, Lnext, &checked);
      if (checked != p) Die("exhaustive check saw the wrong number of positions");
      if (bad) Die("exhaustive check failed at layer " + std::to_string(k));
      std::printf("#  exhaustive check ok: %" PRIu64 " positions\n", checked);
    }
    if (opt.reach) {
      BuildStats s1, s2;
      const Dfa rw = Intersect(reach[size_t(k)], W, &s1);
      const Dfa rl = Intersect(reach[size_t(k)], L, &s2);
      std::printf("# reach k=%d |R|=%" PRIu64 " nodes(R)=%" PRIu64 " |RW|=%" PRIu64 " nodes(RW)=%" PRIu64 " |RL|=%" PRIu64
                  " nodes(RL)=%" PRIu64 "\n",
                  k, CountStrings(reach[size_t(k)]), reach[size_t(k)].Nodes(), CountStrings(rw), rw.Nodes(),
                  CountStrings(rl), rl.Nodes());
    }
    std::fflush(stdout);
    if (!opt.save_dir.empty()) {
      SaveDfa(L, opt.save_dir + "/L" + std::to_string(k) + ".dfa");
      if (opt.keep_all) {
        SaveDfa(W, opt.save_dir + "/W" + std::to_string(k) + ".dfa");
      } else if (k + 1 <= K) {
        std::remove((opt.save_dir + "/L" + std::to_string(k + 1) + ".dfa").c_str());
      }
    }
    if (k == 0 && q == 4 && std::min(opt.w, opt.h) >= 4) {
      const std::vector<u8> s = StartString(g);
      std::printf("# root: %s\n", W.Contains(s.data()) ? "first player wins" : "second player wins");
    }
    Lnext = std::move(L);
    if (opt.max_seconds > 0 && Now() > opt.max_seconds && k > opt.kmin) {
      std::printf("# stopped after layer %d: time budget of %.0fs used\n", k, opt.max_seconds);
      stopped = true;
      break;
    }
  }
  std::printf("# done: %.1fs, peak rss %.1f GiB%s\n", Now(), g_peak_rss, stopped ? " (stopped early)" : "");
  return stopped ? 4 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  g_t0 = Clock::now();
  g_threads = int(std::thread::hardware_concurrency());
  if (argc < 3 || std::string(argv[1]) != "solve") {
    Usage();
    return 2;
  }
  Options opt;
  if (std::sscanf(argv[2], "%dx%d", &opt.w, &opt.h) != 2) {
    Usage();
    return 2;
  }
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) Die("missing value for " + a);
      return argv[++i];
    };
    if (a == "--order") {
      opt.order = next();
    } else if (a == "--queens") {
      opt.q = std::atoi(next());
    } else if (a == "--threads") {
      g_threads = std::atoi(next());
    } else if (a == "--kmin") {
      opt.kmin = std::atoi(next());
    } else if (a == "--sample") {
      opt.sample = u64(std::atoll(next()));
    } else if (a == "--exhaustive") {
      opt.exhaustive = true;
    } else if (a == "--widths") {
      opt.widths = true;
    } else if (a == "--reach") {
      opt.reach = true;
    } else if (a == "--mem-gib") {
      g_mem_limit_gib = std::atof(next());
    } else if (a == "--save") {
      opt.save_dir = next();
      ::mkdir(opt.save_dir.c_str(), 0775);
    } else if (a == "--keep-all") {
      opt.keep_all = true;
    } else if (a == "--max-seconds") {
      opt.max_seconds = std::atof(next());
    } else if (a == "--forward-only") {
      opt.forward_only = std::atoi(next());
    } else if (a == "--split-from") {
      opt.split_from = true;
    } else {
      Usage();
      return 2;
    }
  }
  return Solve(opt);
}
