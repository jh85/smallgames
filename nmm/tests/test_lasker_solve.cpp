// End-to-end validation of Solver::solveMerged on a miniature merged-phase game:
// 4 pieces per side on the 9-point 3MM board ('a'), merged phases, flying enabled.
// (4 pieces so captures reach LIVE states, hands diverge, and flying-at-3-total
// occurs mid-game.) A flat map-based fixpoint solver over the raw (w,b,wh,bh,stm)
// space — no ZDDs, no canonicalization, no partitions — must agree on EVERY state.
#include "../src/solve.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sys/stat.h>
#include <tuple>

using State = std::tuple<u32, u32, int, int, int>;   // w, b, wh, bh, stm

int main() {
  GameSpec sp{99, 'a', 1, true, false, 4, true, false, 8, 8, 16, 0, true};
  Board bd = buildBoard(sp);
  int N = sp.pieces;

  // ---- flat reference solve -------------------------------------------------
  std::map<State, u32> val;
  std::vector<State> all;
  for (u32 w = 0; w < (1u << bd.m); ++w) {
    if (__builtin_popcount(w) > N) continue;
    for (u32 b = 0; b < (1u << bd.m); ++b) {
      if (b & w) continue;
      if (__builtin_popcount(b) > N) continue;
      int W = __builtin_popcount(w), B = __builtin_popcount(b);
      for (int wh = 0; wh + W <= N; ++wh)
        for (int bh = 0; bh + B <= N; ++bh) {
          if (W + wh < 3 || B + bh < 3) continue;   // terminal subspaces: no entry
          for (int stm = 0; stm < 2; ++stm) {
            all.push_back({w, b, wh, bh, stm});
            val[{w, b, wh, bh, stm}] = V_UNK;
          }
        }
    }
  }
  auto childVal = [&](const Succ& s, int stm2) -> u32 {
    int W2 = __builtin_popcount(s.w), B2 = __builtin_popcount(s.b);
    int ownT = stm2 ? B2 + s.bh : W2 + s.wh, oppT = stm2 ? W2 + s.wh : B2 + s.bh;
    if (ownT < 3) return V_LOSS;
    if (oppT < 3) return V_WIN;
    return val[{s.w, s.b, (int)s.wh, (int)s.bh, stm2}];
  };
  std::vector<Succ> buf;
  for (bool changed = true; changed;) {
    changed = false;
    for (auto& st : all) {
      auto [w, b, wh, bh, stm] = st;
      u32 old = val[st];
      if (old != V_UNK) continue;
      buf.clear();
      genSuccessors(bd, w, b, wh, bh, stm, buf);
      u32 nv;
      if (buf.empty()) nv = V_LOSS;
      else {
        bool anyUnk = false, anyDraw = false, win = false;
        for (auto& s : buf) {
          u32 v = childVal(s, stm ^ 1);
          if (v == V_LOSS) { win = true; break; }
          if (v == V_UNK) anyUnk = true;
          else if (v == V_DRAW) anyDraw = true;
        }
        nv = win ? V_WIN : anyUnk ? V_UNK : anyDraw ? V_DRAW : V_LOSS;
      }
      if (nv != old) { val[st] = nv; changed = true; }
    }
  }
  for (auto& [st, v] : val)
    if (v == V_UNK) v = V_DRAW;

  // ---- pipeline solve (real ZDD index + partitioned value iteration) --------
  Zdd1 zp;
  zp.build(bd.m, 0, N);
  Forest2 fp;
  sweepAndBuild(bd, zp, 0, N, false, true, &fp, 4);
  Solver sv;
  sv.bd = &bd; sv.zp = &zp; sv.fp = &fp;
  sv.N = N; sv.threads = 4; sv.dir = "/tmp/test_lasker_solve"; sv.quiet = true;
  mkdir(sv.dir.c_str(), 0755);
  sv.solveMerged();

  // ---- compare every state --------------------------------------------------
  u64 checked = 0, bad = 0;
  for (auto& [st, v] : val) {
    auto [w, b, wh, bh, stm] = st;
    u32 pv = sv.lookupMerged(w, b, wh, bh, stm);
    if (pv != v && ++bad < 6)
      printf("MISMATCH w=%x b=%x h=(%d,%d) stm=%d flat=%u pipeline=%u\n", w, b, wh, bh,
             stm, v, pv);
    ++checked;
  }
  printf("test_lasker_solve: %llu states compared, %llu mismatches; initial=%u\n",
         (unsigned long long)checked, (unsigned long long)bad, (u32)sv.initialValue);
  if (bad) return 1;
  u32 flatInit = val[{0, 0, N, N, 0}];
  if (sv.initialValue != flatInit) { printf("initial-value mismatch\n"); return 1; }
  system(("rm -rf " + sv.dir).c_str());
  return 0;
}
