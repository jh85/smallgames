// solve.hpp — retrograde WDL solver over the two-ZDD index.
//
// Phase 2/3: partitions (W,B) solved in ascending W+B order (captures only descend);
// within a partition, synchronous double-buffered value iteration to the fixed point,
// then unresolved states become DRAW (infinite-play semantics).
// Placement: layers H = wh+bh solved 1..2N; each layer's hand pair is forced
// (H even -> (H/2,H/2) white to move; H odd -> ((H-1)/2,(H+1)/2) black to move), so a
// partition is (wb, bb, H) and one acyclic sweep suffices.
//
// Value encoding (2 bits): 00 unknown (only during construction), 01 LOSS, 10 DRAW,
// 11 WIN — from the perspective of the side to move.
//
// Out-of-index children: the placement index is canonical-only (unfiltered), so a
// pseudo-unreachable placement state may generate a phase-2/3 child that the paper filter
// excludes (provably unreachable). Such children are valued DRAW by convention and
// counted; reachable states never encounter them (every reachable configuration passes
// the filter, and filtered movement states are closed under moves).
//
// Merged phases (Lasker, spec.mergedPhases): the state is (w, b, wh, bh, stm) with stm
// EXPLICIT (hands no longer determine it) and hands free to diverge. Partition key
// (wh, bh, W, B): placements strictly decrease wh+bh; captures strictly decrease W+B at
// fixed hands; capture-free movement stays inside the key — so cycles are confined to a
// single partition, and the ph23 double-buffered iteration kernel applies per partition.
// Order: wh+bh ascending (layer s reads only layers < s and same-layer lower W+B), then
// W+B ascending. Index: unfiltered placement forest (the paper filter is UNSOUND here:
// with an empty opponent board and a nonempty opponent hand, a mill fires without a
// capture, breaking the one-capture-per-mill-event accounting). Board group only —
// the 3-3 mill-group shortcut needs both players flying forever, false with hands.
// Terminals on TOTALS: a side with board+hand < 3 has lost; no-move-loses only fires
// with an empty hand (a hand-bearing player always has an empty point: 2N < m).
#pragma once
#include "board.hpp"
#include "moves.hpp"
#include "zdd1.hpp"
#include "zdd2.hpp"
#include <map>
#include <string>
#include <vector>

enum : u32 { V_UNK = 0, V_LOSS = 1, V_DRAW = 2, V_WIN = 3 };

struct WdlTable {
  std::vector<u8> bits;   // 2 bits per state
  u64 n = 0;
  void init(u64 states) { n = states; bits.assign((states + 3) / 4, 0); }
  u32 get(u64 i) const { return (bits[i >> 2] >> ((i & 3) * 2)) & 3; }
  void set(u64 i, u32 v) {
    u8& x = bits[i >> 2];
    int sh = (int)(i & 3) * 2;
    x = (u8)((x & ~(3 << sh)) | (v << sh));
  }
};

struct Solver {
  const Board* bd = nullptr;
  const Zdd1* z23 = nullptr;
  const Forest2* f23 = nullptr;
  const Zdd1* zp = nullptr;
  const Forest2* fp = nullptr;
  int N = 0, threads = 1;
  std::string dir;
  bool quiet = false;

  // phase-2/3 tables: [sidx23], states = selCount*2 (idx*2+stm)
  std::vector<WdlTable> ph23;
  // placement tables for the previous and current layer: key (wb<<8|bb)
  std::map<int, WdlTable> plPrev, plCur;
  bool keepAllPlacement = false;                       // retain all layers (small games)
  std::map<std::array<int, 3>, WdlTable> plAll;        // (H, wb, bb) -> table
  u64 outOfIndexChildren = 0;
  u64 initialValue = V_UNK;

  int sidx23(int W, int B) const { return (W - 3) * (f23->nMax - 2) + (B - 3); }

  // value of a phase-2/3 position for its side to move (masks already canonical or not)
  u32 lookup23(u32 w, u32 b, int stm) {
    int W = __builtin_popcount(w), B = __builtin_popcount(b);
    int own = stm ? B : W;
    int opp = stm ? W : B;
    if (own < 3) return V_LOSS;
    if (opp < 3) return V_WIN;
    u32 cw = w, cb = b;
    bd->canonicalizeAuto(cw, cb);
    u64 r1 = z23->rank(cw, cb);
    Sel s = f23->sel(W, B);
    u64 k = r1 == UINT64_MAX ? UINT64_MAX : s.selRank(r1);
    if (k == UINT64_MAX) { ++outOfIndexChildren; return V_DRAW; }
    return ph23[sidx23(W, B)].get(k * 2 + stm);
  }

  u32 lookupPlacement(u32 w, u32 b, int wh, int bh) {
    int H = wh + bh;
    int wb = __builtin_popcount(w), bb = __builtin_popcount(b);
    u32 cw = w, cb = b;
    bd->canonicalize(cw, cb);
    u64 r1 = zp->rank(cw, cb);
    Sel s = fp->sel(wb, bb);
    u64 k = r1 == UINT64_MAX ? UINT64_MAX : s.selRank(r1);
    if (k == UINT64_MAX) return V_UNK;
    auto it = plAll.find({H, wb, bb});
    return it == plAll.end() ? V_UNK : it->second.get(k);
  }
  // merged-phase (Lasker) tables, all resident: [midx], states = selCount*2 (idx*2+stm)
  std::vector<WdlTable> mp;
  int midx(int wh, int bh, int W, int B) const {
    return ((wh * (N + 1) + bh) * (N + 1) + W) * (N + 1) + B;
  }
  // value of a merged-phase position for its side to move; terminal on totals
  u32 lookupMerged(u32 w, u32 b, int wh, int bh, int stm) {
    int W = __builtin_popcount(w), B = __builtin_popcount(b);
    int ownT = stm ? B + bh : W + wh, oppT = stm ? W + wh : B + bh;
    if (ownT < 3) return V_LOSS;
    if (oppT < 3) return V_WIN;
    u32 cw = w, cb = b;
    bd->canonicalize(cw, cb);   // board group only (see header comment)
    u64 r1 = zp->rank(cw, cb);
    Sel s = fp->sel(W, B);
    u64 k = r1 == UINT64_MAX ? UINT64_MAX : s.selRank(r1);
    if (k == UINT64_MAX) return V_UNK;   // cannot happen: unfiltered index
    return mp[midx(wh, bh, W, B)].get(k * 2 + stm);
  }
  void solvePhase23();
  void solvePlacement();   // requires solvePhase23 done (or loaded)
  void solveMerged();      // Lasker: full solve over (wh,bh,W,B) partitions
  void saveTables() const;
  bool loadPhase23();
  bool loadMerged();
};
