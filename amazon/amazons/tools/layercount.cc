/*
  amazons — layer-size measurement for the standard 5x5 WDL-table
  feasibility question.

  Counts the exact number of distinct canonical positions reachable from
  the standard 5x5 starting position, layer by layer (layer k = positions
  reached after k moves = k burned squares; the game ends by layer 17
  since the start has 17 empty squares).  These layer sizes decide whether
  a full retrograde WDL table fits in this machine's RAM: the peak layer
  must fit as an array of 8-byte keys plus outcome bits.

  Method: breadth-first expansion with global dedup.  A position is
  canonicalized (8 symmetries + color swap, stm normalized to white) and
  encoded as a base-4 integer over 25 squares (< 4^25, fits in 50 bits).
  Dedup uses 256 independently-locked shards of open-addressed hash
  tables; expansion is parallel over the current layer.

  Aborts with a clear report if the RAM budget is exceeded — that is
  itself the feasibility answer.
*/
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "board.h"

namespace amazons {
namespace {

int kW = 5, kH = 5, kSq = 25;

// Base-4 encoding of a canonical position: 0=empty 1=white 2=black
// 3=burned.  Queens always exist, so key 0 never occurs (0 = empty slot).
uint64_t Encode(const Position& c) {
  uint64_t k = 0, p = 1;
  for (int sq = 0; sq < kSq; sq++, p *= 4) {
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
  p.w = kW;
  p.h = kH;
  p.stm = kWhite;
  for (int sq = 0; sq < kSq; sq++, k /= 4) {
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

uint64_t KeyHash(uint64_t k) {
  k *= 0x9E3779B97F4A7C15ull;
  return k ^ (k >> 29);
}

struct Shard {
  std::vector<uint64_t> slots;
  size_t mask = 0;
  size_t count = 0;
  std::mutex mu;

  void Reserve(size_t n) {
    size_t cap = 1 << 16;
    while (cap < n * 2) cap *= 2;  // load <= 0.5 after reserve
    slots.assign(cap, 0);
    mask = cap - 1;
    count = 0;
  }

  // Returns false and sets *oom if a grow would exceed the byte budget.
  bool Insert(uint64_t k, std::atomic<bool>* oom) {
    if (__builtin_expect((count + 1) * 2 > slots.size(), 0)) {
      if (slots.size() * sizeof(uint64_t) * 2 >
          (size_t{1} << 30) * 4) {  // one shard > 4GB -> too big
        oom->store(true);
        return false;
      }
      std::vector<uint64_t> ns(slots.size() * 2, 0);
      const size_t nm = ns.size() - 1;
      for (uint64_t v : slots) {
        if (!v) continue;
        size_t i = KeyHash(v) & nm;
        while (ns[i]) i = (i + 1) & nm;
        ns[i] = v;
      }
      slots.swap(ns);
      mask = nm;
    }
    size_t i = KeyHash(k) & mask;
    while (slots[i]) {
      if (slots[i] == k) return true;
      i = (i + 1) & mask;
    }
    slots[i] = k;
    count++;
    return true;
  }
};

size_t RssBytes() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  long pages = 0, resident = 0;
  std::fscanf(f, "%ld %ld", &pages, &resident);
  std::fclose(f);
  return static_cast<size_t>(resident) * 4096;
}

}  // namespace
}  // namespace amazons

int main(int argc, char** argv) {
  using namespace amazons;
  const size_t budget_gb = argc > 1 ? std::atoll(argv[1]) : 100;
  if (argc > 3) {  // optional board override: layercount [budget_gb] W H
    kW = std::atoi(argv[2]);
    kH = std::atoi(argv[3]);
    kSq = kW * kH;
  }
  const int nthreads =
      std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
  std::printf("layercount %dx%d: %d threads, RAM budget %zu GB\n", kW,
              kH, nthreads, budget_gb);

  const Position root = Position::Standard(kW, kH).Canonical();
  std::vector<uint64_t> cur = {Encode(root)};
  uint64_t total = 0;
  const auto t0 = std::chrono::steady_clock::now();

  for (int layer = 0; layer <= kSq - 8; layer++) {
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    total += cur.size();
    std::printf(
        "layer %2d: %12zu positions (cumulative %" PRIu64 ", rss %.1f GB, "
        "%.0fs)\n",
        layer, cur.size(), total, RssBytes() / 1e9, secs);
    std::fflush(stdout);
    if (layer == kSq - 8) break;

    constexpr int kShards = 256;
    std::vector<Shard> shards(kShards);
    const size_t per_shard = cur.size() * 2 / kShards + 1024;
    for (auto& s : shards) s.Reserve(per_shard);

    std::atomic<size_t> next_idx{0};
    std::atomic<bool> oom{false};
    auto worker = [&]() {
      std::vector<Move> moves;
      while (!oom.load(std::memory_order_relaxed)) {
        const size_t begin = next_idx.fetch_add(4096);
        if (begin >= cur.size()) break;
        const size_t end = std::min(begin + 4096, cur.size());
        for (size_t i = begin; i < end; i++) {
          Position p = Decode(cur[i]);
          p.GenerateMoves(&moves);
          for (Move m : moves) {
            Position c = p;
            c.DoMove(m);
            const uint64_t k = Encode(c.Canonical());
            Shard& sh = shards[KeyHash(k) & (kShards - 1)];
            sh.mu.lock();
            const bool ok = sh.Insert(k, &oom);
            sh.mu.unlock();
            if (!ok) break;
          }
          if (oom.load(std::memory_order_relaxed)) break;
        }
      }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < nthreads; t++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    if (oom.load()) {
      std::printf(
          "ABORT: a shard exceeded its budget while expanding layer %d -> "
          "%d. Peak layer does not fit in RAM; the full table needs an "
          "external-memory build.\n",
          layer, layer + 1);
      return 2;
    }

    std::vector<uint64_t> nxt;
    size_t n = 0;
    for (const auto& s : shards) n += s.count;
    nxt.reserve(n);
    for (auto& s : shards) {
      for (uint64_t v : s.slots)
        if (v) nxt.push_back(v);
      s.slots.clear();
      s.slots.shrink_to_fit();
    }
    cur.swap(nxt);
    if (cur.size() * sizeof(uint64_t) > (size_t{1} << 30) * budget_gb) {
      std::printf("ABORT: layer exceeds RAM budget.\n");
      return 2;
    }
  }
  std::printf("DONE: total reachable canonical positions = %" PRIu64 "\n",
              total);
  return 0;
}
