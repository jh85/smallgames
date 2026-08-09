// zddbench.cpp -- Benchmark: does a ZDD representation of the solved WDL
// classes beat the dense 2-bit table in memory or query time?
//
// Represents each value class (win/loss/draw/illegal) of a solved game as a
// ZDD over 2N boolean variables (var 2c = "player 1 piece on cell c",
// var 2c+1 = "player 2 piece on cell c"); a state is the 2p-element set of
// its true variables. Reports node counts, memory, build time, and
// membership-query throughput vs. the dense table.
//
// Usage: zddbench <m> <p> <run_dir> [--areas-sym] [--no-lr]
#include "cc_core.hpp"

#include <chrono>
#include <random>
#include <unordered_map>
#include <vector>

using namespace cc;
using Clock = std::chrono::steady_clock;

struct ZDD {
  // node 0 = empty family (false), node 1 = unit family (true terminal)
  struct Node { u32 var, lo, hi; };
  std::vector<Node> nodes;
  std::unordered_map<u64, u32> unique;
  std::unordered_map<u64, u32> unionMemo;

  ZDD() {
    nodes.push_back({UINT32_MAX, 0, 0});
    nodes.push_back({UINT32_MAX, 1, 1});
  }
  u32 mk(u32 var, u32 lo, u32 hi) {
    if (hi == 0) return lo;  // ZDD zero-suppression rule
    u64 key = (u64)var << 40 ^ (u64)lo << 20 ^ hi;
    // (var < 2^8..; lo,hi < 2^20 assumed too small -> use proper packing)
    key = ((u64)var << 56) | ((u64)lo << 28) | hi;
    auto it = unique.find(key);
    if (it != unique.end()) return it->second;
    u32 id = (u32)nodes.size();
    nodes.push_back({var, lo, hi});
    unique.emplace(key, id);
    return id;
  }
  // family containing exactly one set, given by sorted-descending var list
  u32 single(const int* vars, int k) {
    u32 r = 1;
    for (int i = 0; i < k; i++) r = mk(vars[i], 0, r);  // vars ascending
    return r;
  }
  u32 zunion(u32 a, u32 b) {
    if (a == b || b == 0) return a;
    if (a == 0) return b;
    if (a > b) std::swap(a, b);
    u64 key = ((u64)a << 32) | b;
    auto it = unionMemo.find(key);
    if (it != unionMemo.end()) return it->second;
    const Node na = nodes[a];  // copy: recursive mk() may reallocate `nodes`
    const Node nb = nodes[b];
    u32 r;
    if (a == 1) {  // unit family: {emptyset}
      r = mk(nb.var, zunion(1, nb.lo), nb.hi);
    } else if (b == 1) {
      r = mk(na.var, zunion(1, na.lo), na.hi);
    } else if (na.var > nb.var) {  // a's top var is higher: a branches first?
      // order: nodes store var; treat HIGHER var as closer to root
      r = mk(na.var, zunion(na.lo, b), na.hi);
    } else if (na.var < nb.var) {
      r = mk(nb.var, zunion(a, nb.lo), nb.hi);
    } else {
      r = mk(na.var, zunion(na.lo, nb.lo), zunion(na.hi, nb.hi));
    }
    unionMemo.emplace(key, r);
    return r;
  }
  bool member(u32 root, const int* vars, int k) const {
    // vars sorted descending (matching root-to-leaf order)
    int i = 0;
    u32 n = root;
    while (n > 1) {
      const Node& nd = nodes[n];
      if (i < k && (u32)vars[i] == nd.var) {
        n = nd.hi;
        i++;
      } else if (i < k && (u32)vars[i] > nd.var) {
        return false;  // required var not on path
      } else {
        n = nd.lo;
      }
    }
    return n == 1 && i == k;
  }
  // count nodes reachable from root
  u64 countNodes(u32 root) const {
    std::vector<u32> stack{root};
    std::unordered_map<u32, bool> seen;
    u64 c = 0;
    while (!stack.empty()) {
      u32 n = stack.back();
      stack.pop_back();
      if (n <= 1 || seen[n]) continue;
      seen[n] = true;
      c++;
      stack.push_back(nodes[n].lo);
      stack.push_back(nodes[n].hi);
    }
    return c;
  }
};

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <m> <p> <run_dir> [--areas-sym] [--no-lr]\n",
            argv[0]);
    return 2;
  }
  int m = atoi(argv[1]), p = atoi(argv[2]);
  std::string dir = argv[3];
  Game::AreaMode am = Game::FIRSTK;
  bool useLR = true;
  for (int i = 4; i < argc; i++) {
    if (std::string(argv[i]) == "--areas-sym") am = Game::SYMMETRIC;
    if (std::string(argv[i]) == "--no-lr") useLR = false;
  }
  Game g(m, p, am);
  Index ix(g, useLR);
  Table table;
  table.alloc(ix.totalStored);
  if (!table.loadFrom(dir + "/table.bin")) {
    fprintf(stderr, "cannot load table\n");
    return 1;
  }
  printf("game %dx%d p=%d: %llu stored states, dense = %.1f MB (2 bits/state)\n",
         m, m, p, (unsigned long long)ix.totalStored,
         ix.totalStored / 4.0 / 1e6);

  ZDD z;
  u32 roots[4] = {0, 0, 0, 0};
  u64 classCount[4] = {0, 0, 0, 0};
  auto t0 = Clock::now();
  // Insert every stored state into its class ZDD. Chunked union (batch tree
  // reduction) to keep unions balanced.
  std::vector<u32> pend[4];
  for (u32 blk = 0; blk < ix.storedBlocks.size(); blk++) {
    u32 r1 = ix.storedBlocks[blk];
    u64 a = ix.rk.unrankK(r1, g.p, g.N);
    int comp[49], nc = 0;
    for (int c = 0; c < g.N; c++)
      if (!(a >> c & 1)) comp[nc++] = c;
    int sel[8];
    for (int i = 0; i < g.p; i++) sel[i] = i;
    i64 idx = ix.base[r1];
    while (true) {
      u64 b = 0;
      for (int i = 0; i < g.p; i++) b |= bit(comp[sel[i]]);
      u8 v = table.get(idx);
      // variable list, ascending var id (mk() builds bottom-up)
      int vars[16], nv = 0;
      for (int c = 0; c < g.N; c++) {
        if (a >> c & 1) vars[nv++] = 2 * c;
        else if (b >> c & 1) vars[nv++] = 2 * c + 1;
      }
      u32 s = z.single(vars, nv);
      pend[v].push_back(s);
      classCount[v]++;
      if (pend[v].size() >= 1024) {
        u32 u = 0;
        for (u32 x : pend[v]) u = z.zunion(u, x);
        roots[v] = z.zunion(roots[v], u);
        pend[v].clear();
        if (z.unionMemo.size() > 40'000'000) z.unionMemo.clear();
      }
      idx++;
      int i = 0;
      while (i < g.p) {
        sel[i]++;
        int lim = (i + 1 < g.p) ? sel[i + 1] : nc;
        if (sel[i] < lim) break;
        i++;
      }
      if (i == g.p) break;
      for (int j = 0; j < i; j++) sel[j] = j;
    }
  }
  for (int v = 0; v < 4; v++) {
    u32 u = 0;
    for (u32 x : pend[v]) u = z.zunion(u, x);
    roots[v] = z.zunion(roots[v], u);
  }
  double buildS = std::chrono::duration<double>(Clock::now() - t0).count();

  u64 totalNodes = z.nodes.size();
  printf("ZDD build: %.1f s, %llu total unique nodes\n", buildS,
         (unsigned long long)totalNodes);
  u64 classNodes[4];
  for (int v = 0; v < 4; v++) {
    classNodes[v] = z.countNodes(roots[v]);
    printf("  class %-12s: %12llu states, %10llu ZDD nodes (%.2f bytes/state"
           " at 12 B/node)\n",
           valueName(v), (unsigned long long)classCount[v],
           (unsigned long long)classNodes[v],
           classCount[v] ? 12.0 * classNodes[v] / classCount[v] : 0.0);
  }
  u64 sumNodes = classNodes[0] + classNodes[1] + classNodes[2] + classNodes[3];
  double zddMB = sumNodes * 12.0 / 1e6;       // packed array, no hash table
  double denseMB = ix.totalStored / 4.0 / 1e6;
  printf("ZDD (3 classes suffice; 4 counted): %.1f MB vs dense %.1f MB "
         "-> ratio %.1fx\n",
         zddMB, denseMB, zddMB / denseMB);

  // membership query benchmark (dense vs ZDD)
  std::mt19937_64 rng(42);
  const int Q = 200000;
  std::vector<std::pair<u64, u64>> qs;
  for (int i = 0; i < Q; i++) {
    u64 a = 0, b = 0;
    while (__builtin_popcountll(a) < p) a |= bit(rng() % g.N);
    while (__builtin_popcountll(b) < p) {
      int c = rng() % g.N;
      if (!(a >> c & 1)) b |= bit(c);
    }
    qs.push_back({a, b});
  }
  t0 = Clock::now();
  u64 acc = 0;
  for (auto& [a, b] : qs) acc += table.get(ix.indexOf(a, b));
  double denseQ = std::chrono::duration<double>(Clock::now() - t0).count();
  t0 = Clock::now();
  u64 acc2 = 0;
  for (auto& [a, b] : qs) {
    int vars[16], nv = 0;
    for (int c = g.N - 1; c >= 0; c--) {  // descending for member()
      if (a >> c & 1) vars[nv++] = 2 * c;
      else if (b >> c & 1) vars[nv++] = 2 * c + 1;
    }
    for (int v = 0; v < 4; v++)
      if (z.member(roots[v], vars, nv)) { acc2 += v; break; }
  }
  double zddQ = std::chrono::duration<double>(Clock::now() - t0).count();
  printf("query: dense %.0f ns/lookup, ZDD %.0f ns/lookup (checksums %llu/%llu)\n",
         denseQ / Q * 1e9, zddQ / Q * 1e9, (unsigned long long)acc,
         (unsigned long long)acc2);
  return 0;
}
