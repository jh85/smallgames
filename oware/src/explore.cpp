// Exploration tool for small seed counts: full-score retrograde bounds, size of the
// cycle-dependent gap, and exact fresh-history values by brute-force search.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include "oware.hpp"
using namespace oware;

static Index IX;
static std::vector<std::vector<uint8_t>> LO, HI;
static std::vector<std::vector<int8_t>> EX;  // exact fresh value, -1 unknown

static int N;
static uint64_t nodes;
static std::unordered_set<uint64_t> path;

// Exact value (seeds the mover still collects) of board b in layer n, given the
// set of states already visited since the last capture (path), by exhaustive search.
static int search(const Board& b, int n, uint64_t idx, int alpha, int beta) {
  ++nodes;
  int m = legalMask(b);
  if (!m) return b.rowSum(0);
  int best = -1;
  // captures first
  Board c;
  struct Mv { int cap; Board c; };
  Mv mv[ROW]; int nm = 0;
  for (int i = 0; i < ROW; ++i) if (m >> i & 1) { mv[nm].cap = play(b, i, mv[nm].c); ++nm; }
  for (int q = 0; q < nm; ++q) if (mv[q].cap) {
    uint64_t ci = IX.rank(mv[q].c, n - mv[q].cap);
    int v = n - EX[n - mv[q].cap][ci];
    best = std::max(best, v);
  }
  for (int q = 0; q < nm && best < beta; ++q) if (!mv[q].cap) {
    uint64_t ci = IX.rank(mv[q].c, n);
    int v;
    if (path.count(ci)) v = n - mv[q].c.rowSum(0);  // repetition: sweep by ownership
    else if (LO[n][ci] == HI[n][ci]) {
      // NOTE: not valid in general (history!) - only used when flag set
      v = -100;
    } else v = -100;
    if (v == -100) {
      path.insert(ci);
      v = n - search(mv[q].c, n, ci, n - beta, n - std::max(alpha, best));
      path.erase(ci);
    }
    best = std::max(best, v);
  }
  (void)c; (void)idx;
  return best;
}

int main(int argc, char** argv) {
  N = argc > 1 ? atoi(argv[1]) : 10;
  int NX = argc > 2 ? atoi(argv[2]) : 0;  // layers up to NX get exact brute-force values
  LO.resize(N + 1); HI.resize(N + 1); EX.resize(N + 1);
  for (int n = 0; n <= N; ++n) {
    uint64_t sz = IX.layerSize(n);
    auto &lo = LO[n], &hi = HI[n];
    lo.assign(sz, 0); hi.assign(sz, uint8_t(n));
    int sweeps = 0;
    for (bool ch = true; ch; ++sweeps) {
      ch = false;
      for (uint64_t x = 0; x < sz; ++x) {
        if (lo[x] == hi[x]) continue;
        Board b = IX.unrank(x, n), c;
        int m = legalMask(b), l, h;
        if (!m) l = h = b.rowSum(0);
        else {
          l = 0; h = 0;
          for (int i = 0; i < ROW; ++i) if (m >> i & 1) {
            int cap = play(b, i, c);
            uint64_t ci = IX.rank(c, n - cap);
            // bounds use exact lower layers when available, else lo/hi
            int cl = LO[n - cap][ci], chh = HI[n - cap][ci];
            if (cap && n - cap <= NX) cl = chh = EX[n - cap][ci];
            l = std::max(l, n - chh); h = std::max(h, n - cl);
          }
        }
        if (l > lo[x]) { lo[x] = uint8_t(l); ch = true; }
        if (h < hi[x]) { hi[x] = uint8_t(h); ch = true; }
      }
    }
    uint64_t gap = 0, gapsum = 0;
    for (uint64_t x = 0; x < sz; ++x) if (lo[x] != hi[x]) { ++gap; gapsum += hi[x] - lo[x]; }
    printf("n=%2d size=%12llu sweeps=%3d gap=%10llu (%.4f%%) meanwidth=%.2f\n", n,
           (unsigned long long)sz, sweeps, (unsigned long long)gap, 100.0 * gap / sz,
           gap ? double(gapsum) / gap : 0.0);
    fflush(stdout);
    if (n <= NX) {
      EX[n].assign(sz, -1);
      uint64_t inside = 0, outside = 0, eqown = 0, maxnodes = 0;
      for (uint64_t x = 0; x < sz; ++x) {
        if (lo[x] == hi[x]) { EX[n][x] = lo[x]; continue; }
        Board b = IX.unrank(x, n);
        path.clear(); path.insert(x); nodes = 0;
        int v = search(b, n, x, -1, n + 1);
        maxnodes = std::max(maxnodes, nodes);
        EX[n][x] = int8_t(v);
        if (v >= lo[x] && v <= hi[x]) ++inside; else ++outside;
        if (v == b.rowSum(0)) ++eqown;
      }
      printf("      exact: inside=%llu outside=%llu value==ownrow:%llu maxnodes=%llu\n",
             (unsigned long long)inside, (unsigned long long)outside,
             (unsigned long long)eqown, (unsigned long long)maxnodes);
    }
  }
}
