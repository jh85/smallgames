/*
  amazons — fixed-size 2-way clustered transposition table, adapted from
  JHBR3/mate/bns.h (BnsTT) with one structural change: entries are keyed by
  the canonical position hash only, with no ply mixed in.  Amazons burns
  one square per move, so the game graph is a DAG — cross-depth sharing can
  never create the circular value dependencies that ply keying guards
  against in shogi, and sharing every transposition maximizes reuse
  (the paper's df-pn solver likewise shares one table across all tasks).
*/
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace amazons {

struct TTEntry {
  // Upper 48 bits: hash fingerprint. Lower 16 bits: search generation.
  uint64_t tag = 0;
  uint32_t abn = 1;  // numbers in this node's own "side to move wins" frame
  uint32_t obn = 1;

  bool final_result() const { return abn == 0 || obn == 0; }
};
static_assert(sizeof(TTEntry) == 16, "TTEntry should stay compact");

struct TTFreeDeleter {
  void operator()(void* p) const { std::free(p); }
};

class TransTable {
 public:
  static constexpr int kClusterSize = 2;

  void Resize(size_t mb) {
    size_t bytes = mb << 20;
    const size_t want = bytes / sizeof(TTEntry);
    size_t n = size_t{1} << 12;
    while (n * 2 <= want) n *= 2;
    if (n != num_entries_) {
      table_.reset(static_cast<TTEntry*>(std::calloc(n, sizeof(TTEntry))));
      num_entries_ = n;
      mask_ = (n - 1) & ~static_cast<size_t>(kClusterSize - 1);
      gen_ = 0;
    }
  }

  void NewSearch() {
    if (!table_) Resize(4);
    if (++gen_ == 0) {  // uint16 wrap: hard-clear once every 65535 searches
      std::memset(table_.get(), 0, num_entries_ * sizeof(TTEntry));
      gen_ = 1;
    }
    probes_ = hits_ = stores_ = evictions_ = 0;
  }

  // Combined lookup/insertion for the node being searched.
  TTEntry* ProbeOrStore(uint64_t hash, bool* inserted) {
    probes_++;
    TTEntry* c = &table_[hash & mask_];
    const uint64_t tag = (hash & 0xffffffffffff0000ull) | gen_;
    for (int i = 0; i < kClusterSize; i++) {
      if (c[i].tag == tag) {
        hits_++;
        *inserted = false;
        return &c[i];
      }
      // Stores fill a cluster from the first stale slot and entries are
      // never deleted within a generation, so a stale slot also terminates
      // the lookup.
      if (GenerationOf(c[i]) != gen_) {
        stores_++;
        c[i].tag = tag;
        c[i].abn = 1;
        c[i].obn = 1;
        *inserted = true;
        return &c[i];
      }
    }
    stores_++;
    evictions_++;
    c[0].tag = tag;
    c[0].abn = 1;
    c[0].obn = 1;
    *inserted = true;
    return &c[0];
  }

  TTEntry* Probe(uint64_t hash) {
    probes_++;
    TTEntry* c = &table_[hash & mask_];
    const uint64_t tag = (hash & 0xffffffffffff0000ull) | gen_;
    for (int i = 0; i < kClusterSize; i++) {
      if (c[i].tag == tag) {
        hits_++;
        return &c[i];
      }
      if (GenerationOf(c[i]) != gen_) return nullptr;
    }
    return nullptr;
  }

  void Prefetch(uint64_t hash) const {
    __builtin_prefetch(&table_[hash & mask_]);
  }

  uint64_t probes() const { return probes_; }
  uint64_t hits() const { return hits_; }
  uint64_t stores() const { return stores_; }
  uint64_t evictions() const { return evictions_; }

 private:
  static uint16_t GenerationOf(const TTEntry& entry) {
    return static_cast<uint16_t>(entry.tag);
  }

  std::unique_ptr<TTEntry[], TTFreeDeleter> table_;
  size_t num_entries_ = 0;
  size_t mask_ = 0;
  uint16_t gen_ = 0;
  uint64_t probes_ = 0, hits_ = 0, stores_ = 0, evictions_ = 0;
};

}  // namespace amazons
