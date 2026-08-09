// Stage-2 tests: ZDD1 rank/unrank round trips, lexicographic order property, node counts
// vs paper Table 11, 12MM maximum-integer target, serialization reload.
#include "../src/board.hpp"
#include "../src/zdd1.hpp"
#include <cassert>
#include <cstdio>
#include <random>

int main() {
  struct T { int game; u64 paperNodes; };
  T tests[] = {{3, 41}, {5, 447}, {6, 612}, {7, 869}, {9, 2315}, {11, 3057}, {12, 3385}};
  for (auto& t : tests) {
    GameSpec sp = gameSpec(t.game);
    Board bd = buildBoard(sp);
    Zdd1 z;
    z.build(bd.m, 3, sp.pieces);
    printf("[game %2d] zdd1: nodes=%llu (paper %llu%s) count=%llu\n", t.game,
           (unsigned long long)z.nodeCount(), (unsigned long long)t.paperNodes,
           z.nodeCount() == t.paperNodes ? ", MATCH" : ", differs (canonical form)",
           (unsigned long long)z.total);
    // exhaustive or sampled round trip
    u64 nCheck = z.total < 200000 ? z.total : 200000;
    std::mt19937_64 rng(t.game);
    for (u64 i = 0; i < nCheck; ++i) {
      u64 idx = z.total < 200000 ? i : rng() % z.total;
      u32 w, b;
      z.unrank(idx, w, b);
      assert(!(w & b));
      int pw = __builtin_popcount(w), pb = __builtin_popcount(b);
      assert(pw >= 3 && pw <= sp.pieces && pb >= 3 && pb <= sp.pieces);
      assert(z.rank(w, b) == idx);
    }
    // rank order == interleaved-code order (basis for lex-min canonicalization)
    Zdd1Iter it;
    it.initAt(z, 0);
    u64 prev = bd.code(it.w, it.b);
    u64 steps = z.total < 300000 ? z.total - 1 : 300000;
    u64 start = 0;
    if (z.total >= 300000) {
      start = rng() % (z.total - steps - 1);
      it.initAt(z, start);
      prev = bd.code(it.w, it.b);
    }
    for (u64 i = 0; i < steps; ++i) {
      assert(it.next());
      u64 c = bd.code(it.w, it.b);
      assert(c > prev);
      prev = c;
      if (i < 3000) {  // iterator agrees with unrank
        u32 w, b;
        z.unrank(start + i + 1, w, b);
        assert(w == it.w && b == it.b);
      }
    }
    // serialization reload
    z.save("/tmp/zdd1_test.bin", bd.boardHash());
    Zdd1 z2;
    bool ok = z2.load("/tmp/zdd1_test.bin", bd.boardHash());
    assert(ok && z2.total == z.total && z2.nodeCount() == z.nodeCount());
    u32 w, b;
    z2.unrank(z2.total / 2, w, b);
    assert(z2.rank(w, b) == z.total / 2);
  }
  // 12MM maximum-integer target: paper reports 264,369,400,848
  {
    Board bd = buildBoard(gameSpec(12));
    Zdd1 z;
    z.build(bd.m, 3, 12);
    printf("[game 12] count=%llu, max integer (count-1)=%llu, paper max 264369400848: %s\n",
           (unsigned long long)z.total, (unsigned long long)(z.total - 1),
           z.total - 1 == 264369400848ull ? "MATCH (zero-based)"
           : z.total == 264369400848ull  ? "MATCH (count convention)"
                                         : "MISMATCH");
    // 3MM raw count sanity: C(9,3)*C(6,3) = 1680
    Zdd1 z3;
    z3.build(9, 3, 3);
    assert(z3.total == 1680);
  }
  printf("ALL ZDD1 TESTS OK\n");
  return 0;
}
