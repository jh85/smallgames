// Stage-3 tests: reproduce paper Tables 4-7 (3/5/6/7 Men's Morris) per-subset counts,
// determine the 7MM center-mill variant, and validate Sel rank/unrank on built ZDD2s.
#include "../src/board.hpp"
#include "../src/zdd1.hpp"
#include "../src/zdd2.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

struct Row { int x, y; u64 c; };

static bool checkTables(const char* name, const SweepResult& r, const Row* rows, int n,
                        u64 total) {
  bool ok = true;
  u64 sum = 0;
  for (int i = 0; i < n; ++i) {
    u64 mine = rows[i].x == rows[i].y ? r.tally[rows[i].x][rows[i].y]
                                      : r.tally[rows[i].x][rows[i].y] +
                                            r.tally[rows[i].y][rows[i].x];
    sum += mine;
    if (mine != rows[i].c) {
      printf("  [%s] subset %d-%d: mine=%llu paper=%llu MISMATCH\n", name, rows[i].x,
             rows[i].y, (unsigned long long)mine, (unsigned long long)rows[i].c);
      ok = false;
    }
  }
  printf("  [%s] total mine=%llu paper=%llu %s\n", name, (unsigned long long)sum,
         (unsigned long long)total, sum == total && ok ? "MATCH (all rows)" : "MISMATCH");
  return ok && sum == total;
}

int main() {
  int threads = (int)std::thread::hardware_concurrency();
  {  // 3MM
    Board bd = buildBoard(gameSpec(3));
    Zdd1 z;
    z.build(bd.m, 3, 3);
    SweepResult r = sweepAndBuild(bd, z, 3, 3, true, false, nullptr, threads);
    Row rows[] = {{3, 3, 183}};
    assert(checkTables("3MM", r, rows, 1, 183));
  }
  {  // 5MM
    Board bd = buildBoard(gameSpec(5));
    Zdd1 z;
    z.build(bd.m, 3, 5);
    SweepResult r = sweepAndBuild(bd, z, 3, 5, true, false, nullptr, threads);
    Row rows[] = {{3, 3, 10112}, {4, 3, 50380}, {4, 4, 56748},
                  {5, 3, 89218}, {5, 4, 170484}, {5, 5, 93851}};
    assert(checkTables("5MM", r, rows, 6, 470793));
  }
  {  // 6MM
    Board bd = buildBoard(gameSpec(6));
    Zdd1 z;
    z.build(bd.m, 3, 6);
    SweepResult r = sweepAndBuild(bd, z, 3, 6, true, false, nullptr, threads);
    // Paper Table 6 prints 5-3 = 89,218 (identical to its 5MM value), but then its rows
    // sum to 1,106,334, not the published total 1,107,656. The deficit (1,322) matches
    // our 5-3 excess exactly: corrected value 90,540 (looser filter at N=6).
    Row rows[] = {{3, 3, 10112},  {4, 3, 50380},  {4, 4, 56748},  {5, 3, 90540},
                  {5, 4, 180840}, {5, 5, 126516}, {6, 3, 118884}, {6, 4, 199328},
                  {6, 5, 216774}, {6, 6, 57534}};
    assert(checkTables("6MM", r, rows, 10, 1107656));
  }
  {  // 7MM: mill set = variant 1 (all consecutive collinear triples through the center),
     // determined empirically: every row of Table 7 matches EXCEPT 3-3, where the paper
     // prints 30,494. Our 31,360 is the exact orbit count of all 247,520 raw 3-3 configs
     // under the full 8-op group (mill-filter provably inert there); the paper's kept set
     // arithmetically contains no symmetry-invariant configurations (247,520-243,952 =
     // 3,568 ~ all symmetric ones), consistent with a strict-inequality uniqueness
     // artifact in their 7MM run. Documented as an unresolved source anomaly.
    Row rows[] = {{3, 3, 31360},   {4, 3, 171652},  {4, 4, 213938},  {5, 3, 342440},
                  {5, 4, 769116},  {5, 5, 614712},  {6, 3, 513180},  {6, 4, 1024920},
                  {6, 5, 1434088}, {6, 6, 710427},  {7, 3, 573968},  {7, 4, 942112},
                  {7, 5, 992180},  {7, 6, 643338},  {7, 7, 65910}};
    Board bd = buildBoard(gameSpec(7, 1));
    Zdd1 z;
    z.build(bd.m, 3, 7);
    SweepResult r = sweepAndBuild(bd, z, 3, 7, true, false, nullptr, threads);
    assert(checkTables("7MM(v1, 3-3 corrected-to-ours)", r, rows, 15, 9043341));
  }
  {  // ZDD2 build + Sel round trip on 6MM (small but nontrivial)
    Board bd = buildBoard(gameSpec(6));
    Zdd1 z;
    z.build(bd.m, 3, 6);
    Forest2 F;
    SweepResult r = sweepAndBuild(bd, z, 3, 6, true, true, &F, threads);
    assert(F.globalCount == 1107656);
    u64 sum = 0;
    for (int w = 3; w <= 6; ++w)
      for (int b = 3; b <= 6; ++b) {
        assert(F.subsetCount[F.sidx(w, b)] == r.tally[w][b]);
        sum += F.subsetCount[F.sidx(w, b)];
      }
    assert(sum == F.globalCount);
    std::mt19937_64 rng(6);
    for (int w = 3; w <= 6; ++w)
      for (int b = 3; b <= 6; ++b) {
        Sel s = F.sel(w, b);
        for (int it = 0; it < 2000 && s.count; ++it) {
          u64 k = rng() % s.count;
          u64 r1 = s.selUnrank(k);
          assert(s.contains(r1));
          assert(s.selRank(r1) == k);
          u32 wm, bm;
          z.unrank(r1, wm, bm);
          assert(__builtin_popcount(wm) == w && __builtin_popcount(bm) == b);
          assert(bd.isCanonical(wm, bm));
        }
      }
    // membership completeness: every canonical+filtered config is contained
    Zdd1Iter itr;
    itr.initAt(z, 0);
    u64 nContained = 0;
    for (u64 i = 0; i < z.total; ++i) {
      if (F.sel(__builtin_popcount(itr.w), __builtin_popcount(itr.b)).contains(i))
        ++nContained;
      if (i + 1 < z.total) itr.next();
    }
    assert(nContained == F.globalCount);
    F.save("/tmp/zdd2_test.bin");
    Forest2 F2;
    assert(F2.load("/tmp/zdd2_test.bin", bd, z) && F2.globalCount == F.globalCount);
    printf("[6MM] ZDD2: nodes=%llu globalReachable=%llu sel round-trips OK\n",
           (unsigned long long)F.pool.nnodes.load(),
           (unsigned long long)F.pool.reachableNodes(F.globalRoot));
  }
  printf("ALL COUNT TESTS OK\n");
  return 0;
}
