// Minimal multi-valued layered DFA over the pits of an Oware layer, built by
// enumerating every board in a chosen pit order and hash-consing the decision
// tree bottom-up.  Shared by dfacomp (table compression) and wtraj (DFA sizes
// along a retrograde iteration).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../src/oware.hpp"

namespace mvdfa {
using namespace oware;

static constexpr uint32_t REJECT = 0xffffffffu;
static constexpr uint32_t TERM_BASE = 0x80000000u;  // TERM_BASE + terminal code

// A layer's nodes: node = span of child ids (length = remaining seeds + 1).
struct NodeTable {
  std::vector<uint32_t> kids;
  std::vector<uint64_t> off;
  std::vector<uint16_t> len;
  struct SpanHash {
    const NodeTable* t;
    size_t operator()(uint32_t id) const {
      uint64_t h = 1469598103934665603ull;
      const uint32_t* p = &t->kids[t->off[id]];
      for (int i = 0; i < t->len[id]; ++i) { h ^= p[i]; h *= 1099511628211ull; h ^= h >> 29; }
      return size_t(h ^ t->len[id]);
    }
  };
  struct SpanEq {
    const NodeTable* t;
    bool operator()(uint32_t a, uint32_t b) const {
      return t->len[a] == t->len[b] && std::memcmp(&t->kids[t->off[a]], &t->kids[t->off[b]], t->len[a] * 4) == 0;
    }
  };
  using Map = std::unordered_map<uint32_t, uint32_t, SpanHash, SpanEq>;
  Map* map = nullptr;
  NodeTable() { map = new Map(16, SpanHash{this}, SpanEq{this}); }
  ~NodeTable() { delete map; }
  NodeTable(const NodeTable&) = delete;
  uint32_t intern(const uint32_t* span, int L) {
    bool allRej = true;
    for (int i = 0; i < L; ++i) if (span[i] != REJECT) { allRej = false; break; }
    if (allRej) return REJECT;
    uint32_t id = uint32_t(off.size());
    off.push_back(kids.size());
    len.push_back(uint16_t(L));
    kids.insert(kids.end(), span, span + L);
    auto it = map->find(id);
    if (it != map->end()) { kids.resize(off.back()); off.pop_back(); len.pop_back(); return it->second; }
    map->emplace(id, id);
    return id;
  }
  size_t size() const { return off.size(); }
  uint64_t edges() const { uint64_t e = 0; for (uint32_t k : kids) e += k != REJECT; return e; }
};

struct MVDFA {
  std::vector<NodeTable> G;
  uint32_t root = REJECT;
  int n = 0;
  int order[PITS];
  MVDFA() : G(PITS) {}
  uint64_t nodes() const { uint64_t s = 0; for (auto& g : G) s += g.size(); return s; }
  uint64_t edges() const { uint64_t s = 0; for (auto& g : G) s += g.edges(); return s; }
  // Terminal code of a board, or REJECT.
  uint32_t lookup(const Board& b) const {
    uint32_t st = root; int rem = n;
    for (int pos = 0; pos < PITS && st != REJECT && st < TERM_BASE; ++pos) {
      int c = b.p[order[pos]];
      st = c <= rem ? G[pos].kids[G[pos].off[st] + c] : REJECT;
      rem -= c;
    }
    return st;
  }
};

struct Stats { uint64_t nodes = 0, edges = 0; };

// value(board, rank) -> terminal code (>= 0) or -1 for "not in the set".
// Boards outside the indexed set (no empty opponent pit) are never passed in.
template <class F>
struct Builder {
  const Index& IX;
  int n;
  const int* order;
  F value;
  std::atomic<bool> abort{false};
  uint64_t maxNodes;

  uint32_t build(int pos, int rem, Board& b, std::vector<NodeTable>& tabs) {
    uint32_t span[MAX_SEEDS + 1];
    if (pos == PITS - 1) {
      b.p[order[pos]] = uint8_t(rem);
      bool indexed = false;
      for (int j = ROW; j < PITS; ++j) indexed |= b.p[j] == 0;
      for (int c = 0; c <= rem; ++c) span[c] = REJECT;
      if (indexed) {
        int v = value(b, IX.rank(b, n));
        if (v >= 0) span[rem] = TERM_BASE + uint32_t(v);
      }
      return tabs[pos].intern(span, rem + 1);
    }
    for (int c = 0; c <= rem; ++c) {
      b.p[order[pos]] = uint8_t(c);
      span[c] = build(pos + 1, rem - c, b, tabs);
    }
    return tabs[pos].intern(span, rem + 1);
  }

  static void merge(std::vector<NodeTable>& G, std::vector<NodeTable>& L, uint32_t& root) {
    std::vector<uint32_t> prevMap;
    uint32_t span[MAX_SEEDS + 1];
    for (int pos = PITS - 1; pos >= 2; --pos) {
      NodeTable& l = L[pos];
      std::vector<uint32_t> curMap(l.size());
      for (uint32_t id = 0; id < l.size(); ++id) {
        int Ln = l.len[id];
        const uint32_t* k = &l.kids[l.off[id]];
        for (int c = 0; c < Ln; ++c) span[c] = (k[c] == REJECT || k[c] >= TERM_BASE) ? k[c] : prevMap[k[c]];
        curMap[id] = G[pos].intern(span, Ln);
      }
      prevMap.swap(curMap);
    }
    if (root != REJECT) root = prevMap[root];
  }

  // Returns false if aborted (too many nodes).
  bool run(MVDFA& out, int threads) {
    out.n = n;
    for (int i = 0; i < PITS; ++i) out.order[i] = order[i];
    struct Task { int c0, c1; };
    std::vector<Task> tasks;
    for (int c0 = 0; c0 <= n; ++c0) for (int c1 = 0; c0 + c1 <= n; ++c1) tasks.push_back({c0, c1});
    std::atomic<size_t> next{0};
    std::mutex gmu;
    std::vector<std::vector<uint32_t>> sub(n + 1, std::vector<uint32_t>(n + 1, REJECT));
    auto worker = [&]() {
      for (;;) {
        size_t i = next.fetch_add(1);
        if (i >= tasks.size() || abort.load()) return;
        Task tk = tasks[i];
        std::vector<NodeTable> L(PITS);
        Board b{};
        b.p[order[0]] = uint8_t(tk.c0); b.p[order[1]] = uint8_t(tk.c1);
        uint32_t r = build(2, n - tk.c0 - tk.c1, b, L);
        std::lock_guard<std::mutex> lk(gmu);
        merge(out.G, L, r);
        sub[tk.c0][tk.c1] = r;
        uint64_t tot = 0;
        for (int pos = 2; pos < PITS; ++pos) tot += out.G[pos].size();
        if (tot > maxNodes) abort.store(true);
      }
    };
    std::vector<std::thread> th;
    for (int i = 0; i < threads; ++i) th.emplace_back(worker);
    for (auto& t : th) t.join();
    if (abort.load()) return false;
    uint32_t span[MAX_SEEDS + 1];
    for (int c0 = 0; c0 <= n; ++c0) span[c0] = out.G[1].intern(sub[c0].data(), n - c0 + 1);
    out.root = out.G[0].intern(span, n + 1);
    return true;
  }
};

template <class F>
bool buildMV(const Index& IX, int n, const int* order, int threads, uint64_t maxNodes, F value, MVDFA& out) {
  Builder<F> b{IX, n, order, value, {}, maxNodes};
  return b.run(out, threads);
}

// Minimal binary DFA size of {board : accept[code(board)]}, derived from the MV-DFA.
inline Stats threshold(const MVDFA& D, const std::vector<char>& accept) {
  std::vector<NodeTable> H(PITS);
  std::vector<uint32_t> prevMap;
  uint32_t span[MAX_SEEDS + 1];
  for (int pos = PITS - 1; pos >= 0; --pos) {
    const NodeTable& g = D.G[pos];
    std::vector<uint32_t> curMap(g.size());
    for (uint32_t id = 0; id < g.size(); ++id) {
      int Ln = g.len[id];
      const uint32_t* k = &g.kids[g.off[id]];
      for (int c = 0; c < Ln; ++c) {
        uint32_t v = k[c];
        span[c] = v == REJECT ? REJECT : v >= TERM_BASE ? (accept[v - TERM_BASE] ? TERM_BASE : REJECT) : prevMap[v];
      }
      curMap[id] = H[pos].intern(span, Ln);
    }
    prevMap.swap(curMap);
  }
  Stats s;
  if (D.root == REJECT || prevMap[D.root] == REJECT) return s;
  for (int pos = 0; pos < PITS; ++pos) { s.nodes += H[pos].size(); s.edges += H[pos].edges(); }
  return s;
}

}  // namespace mvdfa
