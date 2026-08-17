#include "retro.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <stdexcept>
#include <thread>

namespace fan {

namespace {

constexpr int kChunkShift = 20;  // 2^20 states per work chunk
constexpr size_t kChunkSize = size_t{1} << kChunkShift;

void* MapAnon(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) throw std::runtime_error("mmap anonymous failed");
  return p;
}

// Runs fn(chunk_begin, chunk_end) over [0, total) on n threads.
void ParallelChunks(uint64_t total, int nthreads,
                    const std::function<void(uint64_t, uint64_t)>& fn) {
  const uint64_t nchunks = (total + kChunkSize - 1) / kChunkSize;
  std::atomic<uint64_t> next{0};
  auto worker = [&] {
    for (;;) {
      const uint64_t c = next.fetch_add(1);
      if (c >= nchunks) return;
      const uint64_t lo = c << kChunkShift;
      const uint64_t hi = std::min<uint64_t>(lo + kChunkSize, total);
      fn(lo, hi);
    }
  };
  std::vector<std::thread> pool;
  for (int i = 0; i < nthreads; i++) pool.emplace_back(worker);
  for (auto& t : pool) t.join();
}

}  // namespace

// ---------------------------------------------------------------------------
// ValueStore
// ---------------------------------------------------------------------------

bool ValueStore::Register(int layer_id, const std::string& path,
                          uint64_t total) {
  const uint64_t bytes = (total + 3) / 4;
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_size) != bytes) {
    close(fd);
    return false;
  }
  void* p = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return false;
  madvise(p, bytes, MADV_RANDOM);
  regions_[layer_id] = Region{static_cast<const uint8_t*>(p), bytes};
  return true;
}

bool ValueStore::Has(int layer_id) const {
  return regions_.find(layer_id) != regions_.end();
}

int ValueStore::Get(int layer_id, uint64_t idx) const {
  const Region& r = regions_.at(layer_id);
  return (r.data[idx >> 2] >> ((idx & 3) * 2)) & 3;
}

// ---------------------------------------------------------------------------
// RetroBuilder
// ---------------------------------------------------------------------------

RetroBuilder::RetroBuilder(const Geom& g, const Indexer& ix,
                           const std::string& out_dir, int threads)
    : g_(g), ix_(ix), out_dir_(out_dir), threads_(threads) {}

std::string RetroBuilder::LayerPath(const Layer& l) const {
  char buf[64];
  snprintf(buf, sizeof buf, "%s/values/%d_%d.wdl", out_dir_.c_str(), l.a, l.b);
  return buf;
}

int RetroBuilder::Probe(const Pos& stored) {
  uint64_t w, b;
  Pos wp = stored;
  wp.white_to_move = true;
  ix_.Fold(wp, &w, &b);
  const int lid = ix_.LayerId(__builtin_popcountll(w), __builtin_popcountll(b));
  if (!values_.Has(lid)) return kUnknown;
  return values_.Get(lid, ix_.IndexOf(ix_.layer(lid), w, b));
}

bool RetroBuilder::BuildAll(int max_stones) {
  // Processing order: increasing total stones.
  std::vector<int> order;
  for (int i = 0; i < ix_.num_layers(); i++) {
    const Layer& l = ix_.layer(i);
    if (l.a + l.b <= max_stones) order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&](int x, int y) {
    const Layer &lx = ix_.layer(x), &ly = ix_.layer(y);
    if (lx.a + lx.b != ly.a + ly.b) return lx.a + lx.b < ly.a + ly.b;
    return lx.b < ly.b;
  });

  const std::string vdir = out_dir_ + "/values";
  if (system(("mkdir -p " + vdir).c_str()) != 0) return false;

  FILE* report = fopen((out_dir_ + "/report.txt").c_str(), "a");
  uint64_t done_states = 0;
  for (size_t oi = 0; oi < order.size(); oi++) {
    const int lid = order[oi];
    const Layer& l = ix_.layer(lid);
    const std::string path = LayerPath(l);
    if (values_.Register(lid, path, l.total)) {
      fprintf(stderr, "[skip] {%d,%d} already built\n", l.a, l.b);
      continue;
    }
    LayerStats st;
    const auto t0 = std::chrono::steady_clock::now();
    if (!BuildLayer(l, lid, &st)) return false;
    st.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    done_states += st.reps;
    fprintf(stderr,
            "[done] {%d,%d} total=%llu reps=%llu W=%llu D=%llu L=%llu "
            "(%zu/%zu layers, %.1fs)\n",
            l.a, l.b, (unsigned long long)st.total,
            (unsigned long long)st.reps, (unsigned long long)st.wins,
            (unsigned long long)st.draws, (unsigned long long)st.losses,
            oi + 1, order.size(), st.seconds);
    if (report) {
      fprintf(report, "{%d,%d} total=%llu reps=%llu W=%llu D=%llu L=%llu %.1fs\n",
              l.a, l.b, (unsigned long long)st.total,
              (unsigned long long)st.reps, (unsigned long long)st.wins,
              (unsigned long long)st.draws, (unsigned long long)st.losses,
              st.seconds);
      fflush(report);
    }
  }
  if (report) fclose(report);
  return true;
}

bool RetroBuilder::BuildLayer(const Layer& l, int layer_id, LayerStats* stats) {
  const uint64_t total = l.total;
  stats->a = l.a;
  stats->b = l.b;
  stats->total = total;

  uint8_t* cnt = static_cast<uint8_t*>(MapAnon(total));
  // processed bitmap + chunk queue flags
  const uint64_t nchunks = (total + kChunkSize - 1) / kChunkSize;
  uint64_t* processed =
      static_cast<uint64_t*>(MapAnon((total + 63) / 64 * 8));
  std::vector<std::atomic<uint8_t>> chunk_flag(nchunks);
  for (auto& f : chunk_flag) f.store(0, std::memory_order_relaxed);

  // Overflow counters (>250 children), sharded.
  constexpr int kShards = 256;
  std::vector<std::unordered_map<uint64_t, uint32_t>> ovf(kShards);
  std::vector<std::mutex> ovf_mu(kShards);

  std::atomic<uint64_t> reps{0}, seeds{0}, noverflow{0};

  // ---------------- init pass ----------------
  ParallelChunks(total, threads_, [&](uint64_t lo, uint64_t hi) {
    bool chunk_has_seed = false;
    Pos children[kMaxMoves];
    for (uint64_t idx = lo; idx < hi; idx++) {
      uint64_t w, b;
      ix_.BoardOf(l, idx, &w, &b);
      if (l.sym && !ix_.IsRep(w, b)) continue;
      reps.fetch_add(1, std::memory_order_relaxed);
      const int wc = __builtin_popcountll(w);
      const int bc = __builtin_popcountll(b);
      uint8_t res = 0;
      if (wc == 0) {
        res = kCLoss;  // side to move has no stones
      } else if (bc == 0) {
        res = kCWin;  // opponent has no stones
      } else {
        const size_t n = GenMoves(g_, Pos{w, b, true}, children);
        if (n == 0) {
          res = kCLoss;  // stalemate = loss
        } else {
          uint32_t count = 0;
          // Same-layer child indices, deduplicated after canonicalization:
          // in D4 mode two distinct actual children can share a canonical
          // representative, and the propagation side updates each canonical
          // parent once per resolved child.
          uint64_t same[kMaxMoves];
          int nsame = 0;
          for (size_t i = 0; i < n; i++) {
            uint64_t cw, cb;
            ix_.Fold(children[i], &cw, &cb);
            const int cwc = __builtin_popcountll(cw);
            const int cbc = __builtin_popcountll(cb);
            const int clid = ix_.LayerId(cwc, cbc);
            if (clid == layer_id) {
              same[nsame++] = ix_.IndexOf(l, cw, cb);
              continue;
            }
            const int v =
                values_.Get(clid, ix_.IndexOf(ix_.layer(clid), cw, cb));
            if (v == kLoss) {
              res = kCWin;  // child loses for the side to move there
              break;
            }
            if (v == kDraw) count++;  // never decremented: parent not a loss
            // kWin: not counted
          }
          if (res == 0) {
            std::sort(same, same + nsame);
            count += static_cast<uint32_t>(
                std::unique(same, same + nsame) - same);
            if (count == 0) {
              res = kCLoss;
            } else if (count <= 250) {
              cnt[idx] = static_cast<uint8_t>(count);
            } else {
              cnt[idx] = kOverflow;
              {
                std::lock_guard<std::mutex> lk(ovf_mu[idx & (kShards - 1)]);
                ovf[idx & (kShards - 1)][idx] = count;
              }
              noverflow.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
      }
      if (res) {
        cnt[idx] = res;
        chunk_has_seed = true;
        seeds.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (chunk_has_seed) chunk_flag[lo >> kChunkShift].store(1);
  });

  // ---------------- work queue ----------------
  std::deque<uint32_t> queue;
  std::mutex qmu;
  std::atomic<int> in_flight{0};
  for (uint64_t c = 0; c < nchunks; c++)
    if (chunk_flag[c].load()) queue.push_back(static_cast<uint32_t>(c));

  auto queue_chunk = [&](uint64_t idx) {
    const uint32_t c = static_cast<uint32_t>(idx >> kChunkShift);
    uint8_t expected = 0;
    if (chunk_flag[c].compare_exchange_strong(expected, 1)) {
      std::lock_guard<std::mutex> lk(qmu);
      queue.push_back(c);
    }
  };

  // ---------------- propagation ----------------
  auto worker = [&] {
    for (;;) {
      uint32_t chunk;
      bool popped = false;
      {
        std::lock_guard<std::mutex> lk(qmu);
        if (!queue.empty()) {
          chunk = queue.front();
          queue.pop_front();
          in_flight.fetch_add(1);
          popped = true;
        }
      }
      if (!popped) {
        if (in_flight.load(std::memory_order_acquire) == 0) return;
        std::this_thread::yield();
        continue;
      }
      chunk_flag[chunk].store(0, std::memory_order_release);
      const uint64_t lo = static_cast<uint64_t>(chunk) << kChunkShift;
      const uint64_t hi = std::min<uint64_t>(lo + kChunkSize, total);
      for (uint64_t idx = lo; idx < hi; idx++) {
        const uint8_t xv = cnt[idx];
        if (xv != kCWin && xv != kCLoss) continue;
        uint64_t& pword = processed[idx >> 6];
        const uint64_t pbit = 1ull << (idx & 63);
        if (__atomic_fetch_or(&pword, pbit, __ATOMIC_ACQ_REL) & pbit) continue;

        uint64_t w, b;
        ix_.BoardOf(l, idx, &w, &b);
        if (w == 0 || b == 0) continue;  // terminals: no paika predecessors
        const uint64_t occ = w | b;
        // Reverse paika: black (the side that just moved) steps a stone
        // from u back to an adjacent empty v. In D4 mode distinct raw
        // predecessors can share a canonical parent, so collect and dedup.
        uint64_t parents[512];
        int nparents = 0;
        uint64_t bb = b;
        while (bb) {
          const int u = __builtin_ctzll(bb);
          bb &= bb - 1;
          uint64_t tg = g_.adj[u] & ~occ;
          while (tg) {
            const int v = __builtin_ctzll(tg);
            tg &= tg - 1;
            const uint64_t pb = (b & ~(1ull << u)) | (1ull << v);
            const Pos pred{w, pb, false};
            if (HasCapture(g_, pred)) continue;  // paika would be illegal
            uint64_t sw, sb;
            ix_.Fold(pred, &sw, &sb);
            parents[nparents++] = ix_.IndexOf(l, sw, sb);
          }
        }
        std::sort(parents, parents + nparents);
        nparents = static_cast<int>(
            std::unique(parents, parents + nparents) - parents);
        for (int pi = 0; pi < nparents; pi++) {
          const uint64_t pidx = parents[pi];
            if (xv == kCLoss) {
              // Child is a loss for the side to move => parent wins.
              uint8_t cur = __atomic_load_n(&cnt[pidx], __ATOMIC_ACQUIRE);
              while (cur <= kOverflow) {
                if (__atomic_compare_exchange_n(&cnt[pidx], &cur, kCWin,
                                                /*weak=*/false,
                                                __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE)) {
                  queue_chunk(pidx);
                  break;
                }
              }
            } else {
              // Child is a win for the side to move: one fewer escape.
              for (;;) {
                uint8_t cur = __atomic_load_n(&cnt[pidx], __ATOMIC_ACQUIRE);
                if (cur >= kCWin) break;  // resolved meanwhile
                if (cur == kOverflow) {
                  auto& map = ovf[pidx & (kShards - 1)];
                  std::lock_guard<std::mutex> lk(ovf_mu[pidx & (kShards - 1)]);
                  auto it = map.find(pidx);
                  if (it == map.end()) break;  // resolved meanwhile
                  if (--it->second == 0) {
                    map.erase(it);
                    uint8_t exp = kOverflow;
                    if (__atomic_compare_exchange_n(&cnt[pidx], &exp, kCLoss,
                                                    false, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_ACQUIRE)) {
                      queue_chunk(pidx);
                    }
                  }
                  break;
                }
                if (cur == 0) break;  // should not happen
                if (cur == 1) {
                  uint8_t exp = 1;
                  if (__atomic_compare_exchange_n(&cnt[pidx], &exp, kCLoss,
                                                  false, __ATOMIC_ACQ_REL,
                                                  __ATOMIC_ACQUIRE)) {
                    queue_chunk(pidx);
                  }
                  break;
                }
                uint8_t exp = cur;
                if (__atomic_compare_exchange_n(&cnt[pidx], &exp,
                                                static_cast<uint8_t>(cur - 1),
                                                false, __ATOMIC_ACQ_REL,
                                                __ATOMIC_ACQUIRE))
                  break;
              }
            }
          }
      }
      in_flight.fetch_sub(1, std::memory_order_release);
    }
  };

  {
    std::vector<std::thread> pool;
    for (int i = 0; i < threads_; i++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }

  // ---------------- finalize: write 2-bit value file ----------------
  const std::string path = LayerPath(l);
  const std::string tmp = path + ".tmp";
  const uint64_t bytes = (total + 3) / 4;
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  uint8_t* out = static_cast<uint8_t*>(MapAnon(bytes));
  std::atomic<uint64_t> nwins{0}, nloss{0}, ndraw{0};
  ParallelChunks(total, threads_, [&](uint64_t lo, uint64_t hi) {
    uint64_t lw = 0, ll = 0, ld = 0;
    for (uint64_t idx = lo; idx < hi; idx++) {
      const uint8_t c = cnt[idx];
      uint8_t v = kUnknown;
      if (c == kCWin) {
        v = kWin;
        lw++;
      } else if (c == kCLoss) {
        v = kLoss;
        ll++;
      } else {
        if (l.sym) {
          uint64_t w, b;
          ix_.BoardOf(l, idx, &w, &b);
          if (ix_.IsRep(w, b)) {
            v = kDraw;
            ld++;
          }
        } else {
          v = kDraw;
          ld++;
        }
      }
      out[idx >> 2] |= v << ((idx & 3) * 2);
    }
    nwins += lw;
    nloss += ll;
    ndraw += ld;
  });
  size_t written = fwrite(out, 1, bytes, f);
  fflush(f);
  fsync(fileno(f));
  fclose(f);
  munmap(out, bytes);
  if (written != bytes) {
    unlink(tmp.c_str());
    return false;
  }
  if (rename(tmp.c_str(), path.c_str()) != 0) return false;

  munmap(cnt, total);
  munmap(processed, (total + 63) / 64 * 8);

  if (!values_.Register(layer_id, path, total)) return false;

  stats->reps = reps.load();
  stats->wins = nwins.load();
  stats->losses = nloss.load();
  stats->draws = ndraw.load();
  if (noverflow.load() > 0)
    fprintf(stderr, "  [info] {%d,%d}: %llu overflow counters\n", l.a, l.b,
            (unsigned long long)noverflow.load());
  (void)seeds;
  return true;
}

int ProbePosition(const Geom& g, const Indexer& ix, ValueStore& vs,
                  const Pos& actual) {
  (void)g;
  uint64_t w, b;
  ix.Fold(actual, &w, &b);
  const int lid = ix.LayerId(__builtin_popcountll(w), __builtin_popcountll(b));
  if (!vs.Has(lid)) return kUnknown;
  return vs.Get(lid, ix.IndexOf(ix.layer(lid), w, b));
}

}  // namespace fan
