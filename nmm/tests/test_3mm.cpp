// Stage-5 test: strongly solve Three Men's Morris twice — through the full ZDD pipeline
// and by an independent flat solver with its own hand-written rules — and compare the
// value of every reachable state. Also validates the mill-group equivalence claim used
// for 3-3 flying subsets on the 12MM board.
#include "../src/board.hpp"
#include "../src/moves.hpp"
#include "../src/solve.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <thread>
#include <vector>

// ---------- independent 3MM implementation (no shared code paths) ----------
namespace bf {
static const int MILLS[8][3] = {{0, 1, 2}, {2, 3, 4}, {4, 5, 6}, {6, 7, 0},
                                {1, 8, 5}, {3, 8, 7}, {0, 8, 4}, {2, 8, 6}};
static int ADJ[9];
static void init() {
  for (int i = 0; i < 8; ++i) ADJ[i] = (1 << ((i + 1) & 7)) | (1 << ((i + 7) & 7)) | (1 << 8);
  ADJ[8] = 255;
}
static bool inMill(int mask, int p) {
  for (auto& m : MILLS) {
    if (p != m[0] && p != m[1] && p != m[2]) continue;
    if ((mask >> m[0] & 1) && (mask >> m[1] & 1) && (mask >> m[2] & 1)) return true;
  }
  return false;
}
static u32 id(int w, int b, int wh, int bh, int stm) {
  return (u32)((((w << 9 | b) << 2 | wh) << 2 | bh) << 1 | stm);
}
static void succs(int w, int b, int wh, int bh, int stm, std::vector<u32>& out) {
  out.clear();
  int own = stm ? b : w, opp = stm ? w : b, hand = stm ? bh : wh;
  int empty = 511 & ~(own | opp);
  auto push = [&](int nown, int nopp, int nh) {
    int w2 = stm ? nopp : nown, b2 = stm ? nown : nopp;
    out.push_back(id(w2, b2, stm ? wh : nh, stm ? nh : bh, stm ^ 1));
  };
  auto place = [&](int nown, int nh) {
    // capture on mill
    bool mill = false;
    for (int p = 0; p < 9; ++p)
      if ((nown >> p & 1) && !(own >> p & 1) && inMill(nown, p)) mill = true;
    if (mill && opp) {
      int cand = 0;
      for (int p = 0; p < 9; ++p)
        if ((opp >> p & 1) && !inMill(opp, p)) cand |= 1 << p;
      if (!cand) cand = opp;
      for (int p = 0; p < 9; ++p)
        if (cand >> p & 1) push(nown, opp & ~(1 << p), nh);
    } else
      push(nown, opp, nh);
  };
  if (hand > 0) {
    for (int p = 0; p < 9; ++p)
      if (empty >> p & 1) place(own | 1 << p, hand - 1);
  } else {
    for (int s = 0; s < 9; ++s)
      if (own >> s & 1)
        for (int d = 0; d < 9; ++d)
          if ((ADJ[s] >> d & 1) && (empty >> d & 1)) {
            int base = own & ~(1 << s);
            // reuse place's mill logic with "own before move" = base
            int savedOwn = own;
            (void)savedOwn;
            int nown = base | 1 << d;
            bool mill = false;
            for (auto& m : MILLS)
              if (d == m[0] || d == m[1] || d == m[2])
                if ((nown >> m[0] & 1) && (nown >> m[1] & 1) && (nown >> m[2] & 1)) mill = true;
            if (mill && opp) {
              int cand = 0;
              for (int p = 0; p < 9; ++p)
                if ((opp >> p & 1) && !inMill(opp, p)) cand |= 1 << p;
              if (!cand) cand = opp;
              for (int p = 0; p < 9; ++p)
                if (cand >> p & 1) push(nown, opp & ~(1 << p), hand);
            } else
              push(nown, opp, hand);
          }
  }
}
}  // namespace bf

int main() {
  bf::init();
  // ---------- brute force ----------
  const u32 NS = 1u << 23;
  std::vector<u8> val(NS, V_UNK), valid(NS, 0);
  std::vector<u32> sbuf;
  for (int w = 0; w < 512; ++w)
    for (int b = 0; b < 512; ++b) {
      if (w & b) continue;
      for (int wh = 0; wh <= 3; ++wh)
        for (int bh = 0; bh <= 3; ++bh)
          for (int stm = 0; stm < 2; ++stm) valid[bf::id(w, b, wh, bh, stm)] = 1;
    }
  bool changed = true;
  while (changed) {
    changed = false;
    for (u32 s = 0; s < NS; ++s) {
      if (!valid[s] || val[s] == V_WIN || val[s] == V_LOSS || val[s] == V_DRAW) continue;
      int stm = s & 1, bh = (s >> 1) & 3, wh = (s >> 3) & 3;
      int b = (s >> 5) & 511, w = (s >> 14) & 511;
      int own = stm ? __builtin_popcount(b) : __builtin_popcount(w);
      int hand = stm ? bh : wh;
      u32 nv;
      if (hand == 0 && wh + bh == 0 && own < 3) nv = V_LOSS;
      else {
        bf::succs(w, b, wh, bh, stm, sbuf);
        if (sbuf.empty()) nv = V_LOSS;
        else {
          bool anyUnk = false, anyDraw = false, win = false;
          for (u32 t : sbuf) {
            u32 v = val[t];
            if (v == V_LOSS) { win = true; break; }
            if (v == V_UNK) anyUnk = true;
            else if (v == V_DRAW) anyDraw = true;
          }
          nv = win ? V_WIN : anyUnk ? V_UNK : anyDraw ? V_DRAW : V_LOSS;
        }
      }
      if (nv != V_UNK && nv != val[s]) { val[s] = (u8)nv; changed = true; }
    }
  }
  for (u32 s = 0; s < NS; ++s)
    if (valid[s] && val[s] == V_UNK) val[s] = V_DRAW;

  // BFS reachable set from the initial state
  std::vector<u8> reach(NS, 0);
  std::vector<u32> queue{bf::id(0, 0, 3, 3, 0)};
  reach[queue[0]] = 1;
  for (size_t qi = 0; qi < queue.size(); ++qi) {
    u32 s = queue[qi];
    int stm = s & 1, bh = (s >> 1) & 3, wh = (s >> 3) & 3;
    int b = (s >> 5) & 511, w = (s >> 14) & 511;
    int own = stm ? __builtin_popcount(b) : __builtin_popcount(w);
    if (wh + bh == 0 && own < 3) continue;    // terminal: no expansion
    bf::succs(w, b, wh, bh, stm, sbuf);
    for (u32 t : sbuf)
      if (!reach[t]) { reach[t] = 1; queue.push_back(t); }
  }
  u32 init = bf::id(0, 0, 3, 3, 0);
  printf("[bf] reachable states: %zu, initial value: %s\n", queue.size(),
         val[init] == V_WIN ? "WIN" : val[init] == V_LOSS ? "LOSS" : "DRAW");

  // ---------- pipeline ----------
  int threads = (int)std::thread::hardware_concurrency();
  Board bd = buildBoard(gameSpec(3));
  Zdd1 z23, zp;
  z23.build(bd.m, 3, 3);
  zp.build(bd.m, 0, 3);
  Forest2 f23, fpp;
  sweepAndBuild(bd, z23, 3, 3, true, true, &f23, threads);
  sweepAndBuild(bd, zp, 0, 3, false, true, &fpp, threads);
  Solver sv;
  sv.bd = &bd; sv.z23 = &z23; sv.f23 = &f23; sv.zp = &zp; sv.fp = &fpp;
  sv.N = 3; sv.threads = threads; sv.dir = "/tmp/m3_tables"; sv.quiet = true;
  sv.keepAllPlacement = true;
  sv.solvePhase23();
  sv.solvePlacement();
  const char* names[] = {"UNK", "LOSS", "DRAW", "WIN"};
  printf("[pipeline] initial value: %s, out-of-index children: %llu\n",
         names[sv.initialValue], (unsigned long long)sv.outOfIndexChildren);
  assert(sv.initialValue == val[init]);

  // ---------- compare every reachable state ----------
  u64 compared = 0, mism = 0;
  for (u32 s : queue) {
    int stm = s & 1, bh = (s >> 1) & 3, wh = (s >> 3) & 3;
    int b = (s >> 5) & 511, w = (s >> 14) & 511;
    u32 pv;
    if (wh + bh == 0) pv = sv.lookup23((u32)w, (u32)b, stm);
    else pv = sv.lookupPlacement((u32)w, (u32)b, wh, bh);
    ++compared;
    if (pv != val[s]) {
      if (++mism < 6)
        printf("MISMATCH state w=%o b=%o wh=%d bh=%d stm=%d bf=%s mine=%s\n", w, b, wh,
               bh, stm, names[val[s]], names[pv]);
    }
  }
  printf("[compare] %llu reachable states compared, %llu mismatches\n",
         (unsigned long long)compared, (unsigned long long)mism);
  assert(mism == 0);

  // ---------- mill-group value-preservation structural test (12MM board, 3-3) ----------
  {
    Board b12 = buildBoard(gameSpec(12));
    printf("[millgroup] 12MM: board group %zu, mill group %zu\n", b12.perms.size(),
           b12.millPerms.size());
    std::mt19937 rng(42);
    std::vector<Succ> s1, s2;
    for (int it = 0; it < 5000; ++it) {
      // random 3-3 configuration
      u32 w = 0, b = 0;
      while (__builtin_popcount(w) < 3) w |= 1u << (rng() % 24);
      while (__builtin_popcount(b) < 3) {
        u32 bit = 1u << (rng() % 24);
        if (!(w & bit)) b |= bit;
      }
      int sIdx = 1 + (int)(rng() % (b12.millPerms.size() - 1));
      u32 tw = b12.permuteMaskMill(sIdx, w), tb = b12.permuteMaskMill(sIdx, b);
      for (int stm = 0; stm < 2; ++stm) {
        s1.clear(); s2.clear();
        genSuccessors(b12, w, b, 0, 0, stm, s1);
        genSuccessors(b12, tw, tb, 0, 0, stm, s2);
        // canonical multisets of successors must agree (under the mill group)
        // capture successors are terminal (opponent reduced to 2 -> mover wins): only
        // their count must correspond; non-capture successors stay 3-3 and must map to
        // identical mill-group-canonical forms.
        std::vector<u64> c1, c2;
        for (auto& sc : s1) {
          if (sc.capture) { c1.push_back(0xC0FFEEull << 32); continue; }
          u32 cw = sc.w, cb = sc.b;
          b12.canonicalizeAuto(cw, cb);
          c1.push_back(b12.code(cw, cb));
        }
        for (auto& sc : s2) {
          if (sc.capture) { c2.push_back(0xC0FFEEull << 32); continue; }
          u32 cw = sc.w, cb = sc.b;
          b12.canonicalizeAuto(cw, cb);
          c2.push_back(b12.code(cw, cb));
        }
        std::sort(c1.begin(), c1.end());
        std::sort(c2.begin(), c2.end());
        assert(c1 == c2);
      }
    }
    printf("[millgroup] successor-structure isomorphism verified on 5000 random 3-3 pairs\n");
  }
  printf("ALL 3MM TESTS OK\n");
  return 0;
}
