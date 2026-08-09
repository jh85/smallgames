// Stage-1 tests: board graphs, mills, symmetry groups, canonicalization, minMillEvents.
#include "../src/board.hpp"
#include <cassert>
#include <cstdio>
#include <random>
#include <set>

static int edgeCount(const Board& bd) {
  int e = 0;
  for (int p = 0; p < bd.m; ++p) e += __builtin_popcount(bd.adj[p]);
  return e / 2;
}

int main() {
  int games[] = {3, 5, 6, 7, 9, 11, 12, 16};
  for (int g : games) {
    GameSpec sp = gameSpec(g, /*sevenVariant*/ 0);
    Board bd = buildBoard(sp);
    printf("[game %2d] board %c: points=%d edges=%d mills=%zu syms=%zu autos=%d\n", g,
           sp.boardType, bd.m, edgeCount(bd), bd.millMask.size(), bd.perms.size(),
           countGraphAutomorphisms(bd));
    assert(edgeCount(bd) == sp.expEdges);
    assert((int)bd.perms.size() == sp.expSyms);
    if (sp.expMills >= 0) assert((int)bd.millMask.size() == sp.expMills);
    // group closure + inverses
    std::set<std::array<u8, 32>> pset(bd.perms.begin(), bd.perms.end());
    for (auto& a : bd.perms)
      for (auto& b : bd.perms) {
        std::array<u8, 32> c{};
        for (int p = 0; p < bd.m; ++p) c[p] = a[b[p]];
        assert(pset.count(c));
      }
    // permuteMask correctness vs naive
    std::mt19937 rng(g);
    for (int it = 0; it < 2000; ++it) {
      u32 w = rng() & ((bd.m == 32) ? 0xFFFFFFFFu : ((1u << bd.m) - 1));
      int s = (int)(rng() % bd.perms.size());
      u32 naive = 0;
      for (int p = 0; p < bd.m; ++p)
        if (w >> p & 1) naive |= 1u << bd.perms[s][p];
      assert(bd.permuteMask(s, w) == naive);
    }
    // canonicalization idempotent + invariant across symmetry
    for (int it = 0; it < 2000; ++it) {
      u32 all = (bd.m == 32) ? 0xFFFFFFFFu : ((1u << bd.m) - 1);
      u32 w = rng() & all, b = rng() & all & ~w;
      u32 cw = w, cb = b;
      bd.canonicalize(cw, cb);
      assert(bd.isCanonical(cw, cb));
      u32 cw2 = cw, cb2 = cb;
      bd.canonicalize(cw2, cb2);
      assert(cw2 == cw && cb2 == cb);
      int s = (int)(rng() % bd.perms.size());
      u32 tw = bd.permuteMask(s, w), tb = bd.permuteMask(s, b);
      bd.canonicalize(tw, tb);
      assert(tw == cw && tb == cb);
    }
    // minMillEvents basics
    assert(bd.minMillEvents(0) == 0);
    for (u32 mk : bd.millMask) assert(bd.minMillEvents(mk) == 1);
    // two disjoint mills need 2; two mills sharing a point need 1
    for (size_t i = 0; i < bd.millMask.size(); ++i)
      for (size_t j = i + 1; j < bd.millMask.size(); ++j) {
        u32 u = bd.millMask[i] | bd.millMask[j];
        int e = bd.minMillEvents(u);
        // adding stones can complete additional mills; recompute expected via full check
        int full = 0;
        for (u32 mk : bd.millMask)
          if ((u & mk) == mk) ++full;
        if (full == 2)
          assert(e == ((bd.millMask[i] & bd.millMask[j]) ? 1 : 2));
      }
    bd.writeConfigJson("config/morris" + std::to_string(g) + ".json");
  }
  // 12MM mill list diagnostic (prompt requires printing it)
  Board b12 = buildBoard(gameSpec(12));
  printf("[game 12] mill list (%zu):\n", b12.millPts.size());
  for (auto& t : b12.millPts)
    printf("  %s-%s-%s\n", b12.names[t[0]].c_str(), b12.names[t[1]].c_str(),
           b12.names[t[2]].c_str());
  printf("ALL BOARD TESTS OK\n");
  return 0;
}
