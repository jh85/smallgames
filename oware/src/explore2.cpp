// Exploration: sound guarantees gM (mover) / gN (non-mover) per board, first by plain
// attractor retrograde, then tightened by the repetition-aware (co-Buchi) fixpoint.
// Brute-force exact fresh-history values verify soundness on tiny layers.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include "oware.hpp"
using namespace oware;

static Index IX;
static std::vector<std::vector<uint8_t>> GM, GN;
static std::vector<std::vector<int8_t>> EX;
static int NX;
static uint64_t nodes, nodeLimit;
static std::unordered_set<uint64_t> path;

static int search(const Board& b, int n, int alpha, int beta) {
  if (++nodes > nodeLimit) throw 1;
  int m = legalMask(b);
  if (!m) return b.rowSum(0);
  int best = -1;
  struct Mv { int cap; Board c; } mv[6];
  int nm = 0;
  for (int i = 0; i < ROW; ++i) if (m >> i & 1) { mv[nm].cap = play(b, i, mv[nm].c); ++nm; }
  for (int q = 0; q < nm; ++q) if (mv[q].cap)
    best = std::max(best, n - EX[n - mv[q].cap][IX.rank(mv[q].c, n - mv[q].cap)]);
  for (int q = 0; q < nm && best < beta; ++q) if (!mv[q].cap) {
    uint64_t ci = IX.rank(mv[q].c, n);
    int v;
    if (path.count(ci)) v = n - mv[q].c.rowSum(0);
    else {
      path.insert(ci);
      v = n - search(mv[q].c, n, n - beta, n - std::max(alpha, best));
      path.erase(ci);
    }
    best = std::max(best, v);
  }
  return best;
}

struct Succ { int cap; uint64_t idx; };

int main(int argc, char** argv) {
  int N = argc > 1 ? atoi(argv[1]) : 10;
  NX = argc > 2 ? atoi(argv[2]) : 3;
  nodeLimit = argc > 3 ? strtoull(argv[3], 0, 10) : 100000000ull;
  bool refine = argc > 4 ? atoi(argv[4]) : 1;
  GM.resize(N + 1); GN.resize(N + 1); EX.resize(N + 1);
  for (int n = 0; n <= N; ++n) {
    uint64_t sz = IX.layerSize(n);
    auto &gm = GM[n], &gn = GN[n];
    gm.assign(sz, 0); gn.assign(sz, 0);
    std::vector<uint8_t> zm, zn;
    // one evaluation: returns (M,N) guarantees of x computed from arrays (am,an) for
    // same-layer successors
    auto evalY = [&](uint64_t x, const std::vector<uint8_t>& am, const std::vector<uint8_t>& an,
                     int& outM, int& outN, const Board& b) {
      int m = legalMask(b);
      if (!m) { outM = b.rowSum(0); outN = b.rowSum(ROW); return; }
      int M = 0, Nn = 99; Board c;
      for (int i = 0; i < ROW; ++i) if (m >> i & 1) {
        int cap = play(b, i, c);
        uint64_t ci = IX.rank(c, n - cap);
        if (cap) { M = std::max(M, cap + GN[n - cap][ci]); Nn = std::min<int>(Nn, GM[n - cap][ci]); }
        else { M = std::max<int>(M, an[ci]); Nn = std::min<int>(Nn, am[ci]); }
      }
      outM = M; outN = Nn; (void)x;
    };
    auto attractor = [&]() {
      int sweeps = 0;
      for (bool ch = true; ch; ++sweeps) {
        ch = false;
        for (uint64_t x = 0; x < sz; ++x) {
          if (gm[x] + gn[x] == n) continue;
          Board b = IX.unrank(x, n);
          int M, Nn; evalY(x, gm, gn, M, Nn, b);
          if (M > gm[x]) { gm[x] = uint8_t(M); ch = true; }
          if (Nn > gn[x]) { gn[x] = uint8_t(Nn); ch = true; }
        }
      }
      return sweeps;
    };
    auto report = [&](const char* tag, int sweeps) {
      uint64_t gap = 0, gs = 0;
      for (uint64_t x = 0; x < sz; ++x) if (gm[x] + gn[x] != n) { ++gap; gs += n - gm[x] - gn[x]; }
      printf("n=%2d %-9s size=%10llu sweeps=%4d gap=%9llu (%.3f%%) meanwidth=%.2f\n", n, tag,
             (unsigned long long)sz, sweeps, (unsigned long long)gap, 100.0 * gap / sz,
             gap ? double(gs) / gap : 0.0);
      fflush(stdout);
    };
    int sw = attractor();
    report("attractor", sw);
    if (refine) {
      int total = 0, rounds = 0;
      for (bool outer = true; outer; ++rounds) {
        outer = false;
        zm.assign(sz, uint8_t(n)); zn.assign(sz, uint8_t(n));
        for (bool ch = true; ch; ++total) {
          ch = false;
          for (uint64_t x = 0; x < sz; ++x) {
            if (zm[x] == gm[x] && zn[x] == gn[x]) continue;
            Board b = IX.unrank(x, n);
            int yM, yN, sM, sN;
            evalY(x, gm, gn, yM, yN, b);   // CPre(Y)
            evalY(x, zm, zn, sM, sN, b);   // CPre(Z)
            int m = legalMask(b);
            int nm = m ? std::max(yM, std::min(b.rowSum(0), sM)) : yM;
            int nn = m ? std::max(yN, std::min(b.rowSum(ROW), sN)) : yN;
            nm = std::max<int>(nm, gm[x]); nn = std::max<int>(nn, gn[x]);
            if (nm < zm[x]) { zm[x] = uint8_t(nm); ch = true; }
            if (nn < zn[x]) { zn[x] = uint8_t(nn); ch = true; }
          }
        }
        for (uint64_t x = 0; x < sz; ++x) {
          if (zm[x] > gm[x]) { gm[x] = zm[x]; outer = true; }
          if (zn[x] > gn[x]) { gn[x] = zn[x]; outer = true; }
        }
      }
      for (uint64_t x = 0; x < sz; ++x) if (gm[x] + gn[x] > n) { printf("INCONSISTENT at %llu\n", (unsigned long long)x); return 1; }
      printf("      refine rounds=%d ", rounds);
      report("refined", total);
    }
    if (n <= NX) {
      EX[n].assign(sz, -1);
      uint64_t bad = 0, done = 0, skipped = 0, tight = 0;
      for (uint64_t x = 0; x < sz; ++x) {
        if (gm[x] + gn[x] == n) { EX[n][x] = gm[x]; continue; }
        Board b = IX.unrank(x, n);
        path.clear(); path.insert(x); nodes = 0;
        try {
          int v = search(b, n, -1, n + 1);
          EX[n][x] = int8_t(v); ++done;
          if (v < gm[x] || v > n - gn[x]) { ++bad; if (bad < 5) { printf("UNSOUND x=%llu v=%d gm=%d gn=%d board:", (unsigned long long)x, v, gm[x], gn[x]); for (int j=0;j<12;++j) printf(" %d", b.p[j]); printf("\n"); } }
          if (v == gm[x] || v == n - gn[x]) ++tight;
        } catch (int) { ++skipped; EX[n][x] = int8_t(gm[x]); }
      }
      printf("      exact check: searched=%llu unsound=%llu skipped=%llu atbound=%llu\n",
             (unsigned long long)done, (unsigned long long)bad, (unsigned long long)skipped,
             (unsigned long long)tight);
    }
  }
}
