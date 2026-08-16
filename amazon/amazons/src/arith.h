/*
  amazons — branch-number / proof-number arithmetic for the AND/OR solver.

  Ported from JHBR3/mate/bns.h (the pure, board-independent part), which
  implements the route-branch-number search of:
    岡部文洋「経路分枝数を用いた詰め将棋解図について」
    (Fumihiro Okabe, "Application for solving tsume shogi problem by
     route branch number")

  Instead of df-pn's proof/disproof numbers (min/sum over children), BNS
  tracks per-node AND/OR *branch numbers*: the number of unresolved
  branches attached to the currently selected route.  Sums over siblings
  are replaced by counts of unresolved siblings, which makes the numbers
  immune to the DAG double-counting that inflates proof numbers.  Amazons
  positions transpose heavily (the game graph is a DAG), so this matters
  here at least as much as in tsume shogi.

  Here "proved" = side to move wins, "disproved" = side to move loses:

    Unresolved leaf:  abn = 1,   obn = 1
    Proved win:       abn = 0,   obn = INF
    Proved loss:      abn = INF, obn = 0
    OR node  (side to move; k active children, best = min abn):
        abn = best.abn,           obn = best.obn + (k - 1)
    AND node (opponent; k active children, best = min obn):
        obn = best.obn,           abn = best.abn + (k - 1)
    Child thresholds (c2 = second-smallest relevant number):
        OR:  ABN' = min(abn(c2) + 1, ABN),   OBN' = OBN - k + 1
        AND: ABN' = ABN - k + 1,             OBN' = min(obn(c2) + 1, OBN)
    Iterate while ABN > abn && OBN > obn.
*/
#pragma once

#include <cstdint>

namespace amazons::bns {

// Saturated infinity. Real branch numbers stay far below this: they only
// grow by +1 per unresolved sibling along a route, so anything at or
// above kInf means "infinite" (proved/disproved side), never a count.
constexpr uint32_t kInf = 1u << 30;

inline uint32_t SatAdd(uint32_t a, uint32_t b) {
  uint64_t s = static_cast<uint64_t>(a) + b;
  return s >= kInf ? kInf : static_cast<uint32_t>(s);
}

// A child's numbers as seen from its parent.
struct ChildView {
  uint32_t abn = 1;
  uint32_t obn = 1;
};

// Node summary computed from children.
struct Summary {
  uint32_t abn = 1;
  uint32_t obn = 1;
  int best = -1;          // selected child index (-1 when terminal)
  uint32_t second = kInf; // second-smallest relevant number (multiset)
  uint32_t k = 0;         // number of active (unresolved) children
  bool proved = false;    // side to move wins
  bool disproved = false; // side to move loses

  bool terminal() const { return proved || disproved; }
};

// Summarize children into node values.
//
// kOrNode: side to move (existential). kPnDn: use proof/disproof-number
// arithmetic (sum over siblings) instead of branch numbers (count of
// unresolved siblings) — the control mode for algorithm comparisons;
// everything else in the solver is shared.
//
// The "relevant" number is abn at OR nodes and obn at AND nodes; the
// selected child minimizes it.  PN/DN mode uses the smaller opposite number
// as its secondary key; branch-number mode keeps generator order on ties.
template <bool kOrNode, bool kPnDn>
inline Summary Summarize(const ChildView* c, int n) {
  Summary s;
  uint32_t best_rel = kInf, best_opp = kInf;
  uint32_t second = kInf;
  uint32_t k = 0;
  uint64_t opp_sum = 0;  // kPnDn only
  int best = -1;

  for (int i = 0; i < n; i++) {
    const uint32_t rel = kOrNode ? c[i].abn : c[i].obn;
    const uint32_t opp = kOrNode ? c[i].obn : c[i].abn;

    if (rel == 0) {
      // OR: a proved child proves the node. AND: a disproved child
      // disproves the node.
      s.best = i;
      s.proved = kOrNode;
      s.disproved = !kOrNode;
      if (kOrNode) {
        s.abn = 0;
        s.obn = kInf;
      } else {
        s.abn = kInf;
        s.obn = 0;
      }
      return s;
    }
    if (kPnDn) opp_sum += opp;
    if (rel >= kInf) continue;

    k++;
    if (rel < best_rel || (kPnDn && rel == best_rel && opp < best_opp)) {
      second = best_rel;
      best_rel = rel;
      best_opp = opp;
      best = i;
    } else if (rel < second) {
      second = rel;
    }
  }

  const bool exhausted = kPnDn ? opp_sum == 0 : k == 0;
  if (exhausted) {
    // OR: every child disproved -> loss. AND: every child proved -> win.
    s.proved = !kOrNode;
    s.disproved = kOrNode;
    if (kOrNode) {
      s.abn = kInf;
      s.obn = 0;
    } else {
      s.abn = 0;
      s.obn = kInf;
    }
    return s;
  }

  const uint32_t node_rel = best_rel;
  const uint32_t node_opp =
      kPnDn ? (opp_sum >= kInf ? kInf : static_cast<uint32_t>(opp_sum))
            : SatAdd(best_opp, k - 1);

  s.abn = kOrNode ? node_rel : node_opp;
  s.obn = kOrNode ? node_opp : node_rel;
  s.best = best;
  s.second = second;
  s.k = k;
  return s;
}

// Thresholds to pass to the selected child.
//
//   OR:  ABN' = min(second + 1, ABN)
//        OBN' = OBN - (node.obn - best.obn)   [= OBN - k + 1 for BNS]
//   AND: symmetric.
//
// The subtraction form is shared by both arithmetics: for BNS the
// difference (node.opp - best.opp) is exactly k - 1; for pn/dn it is the
// sum of the other children's opposite numbers (cshogi's formula).
// Caller guarantees ABN > node.abn and OBN > node.obn (else it returns).
template <bool kOrNode>
inline void ChildThresholds(const Summary& s, const ChildView& best,
                            uint32_t abn_th, uint32_t obn_th,
                            uint32_t* child_abn_th, uint32_t* child_obn_th) {
  const uint32_t rel_th = kOrNode ? abn_th : obn_th;
  const uint32_t opp_th = kOrNode ? obn_th : abn_th;
  const uint32_t node_opp = kOrNode ? s.obn : s.abn;
  const uint32_t best_opp = kOrNode ? best.obn : best.abn;

  const uint32_t rel_out =
      rel_th < SatAdd(s.second, 1) ? rel_th : SatAdd(s.second, 1);
  const uint32_t opp_out =
      opp_th >= kInf ? kInf : opp_th - (node_opp - best_opp);

  *child_abn_th = kOrNode ? rel_out : opp_out;
  *child_obn_th = kOrNode ? opp_out : rel_out;
}

constexpr ChildView WinView() { return ChildView{0, kInf}; }
constexpr ChildView LossView() { return ChildView{kInf, 0}; }

}  // namespace amazons::bns
