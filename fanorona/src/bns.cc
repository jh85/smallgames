/*
  Fanorona — BNS (Branch Number Search) weak-solve solver.

  See bns.h for the algorithm description and references. Positions are
  passed by value (Pos is two uint64 + bool), so there is no
  DoMove/UndoMove machinery: the child Pos generated into the per-ply
  frame is handed straight to the recursion.
*/

#include "bns.h"

#include <algorithm>
#include <cassert>

namespace fan {

// =====================================================================
// Entry point
// =====================================================================

BnsSolver::BnsSolver(const Geom& g, size_t tt_mb) : geom_(g), tt_mb_(tt_mb) {}

void BnsSolver::set_arith_pn(bool v) { arith_pn_ = v; }
void BnsSolver::set_max_ply(int p) { max_ply_ = p; }
void BnsSolver::set_db_probe(std::function<int(const Pos&)> f) {
  db_probe_ = std::move(f);
}
void BnsSolver::set_nodes_limit(uint64_t n) { nodes_limit_ = n; }

int BnsSolver::Solve(const Pos& root) {
  stats_ = Stats();
  pv_.clear();
  path_hashes_.clear();
  overrides_.clear();
  override_version_ = 0;
  limit_hit_ = false;
  resource_taint_seen_ = false;

  tt_.Resize(tt_mb_);
  tt_.NewSearch();
  if (frames_size_ != static_cast<size_t>(max_ply_) + 2) {
    frames_size_ = static_cast<size_t>(max_ply_) + 2;
    frames_ = std::make_unique<Frame[]>(frames_size_);
  }

  const uint64_t root_hash = HashPos(root);
  path_hashes_.push_back(root_hash);

  if (arith_pn_) {
    SearchImpl<true, true>(root, bns::kInf, bns::kInf, 0);
  } else {
    SearchImpl<true, false>(root, bns::kInf, bns::kInf, 0);
  }

  path_hashes_.pop_back();

  stats_.tt_probes = tt_.probes();
  stats_.tt_hits = tt_.hits();
  stats_.tt_stores = tt_.stores();

  // Read the root verdict from the table only. Derived root verdicts
  // always land there: path dependencies anchored at the root dissolve
  // (see the loop-head rule in SearchImpl), and resource-tainted
  // verdicts stay out of the table, leaving the root entry non-final —
  // correctly UNSOLVED. Any leftover override under the root hash
  // describes a deep revisit of the root position inside the tree, not
  // the root's own value, and must be ignored here.
  bns::ChildView root_view{1, 1};
  if (BnsTTEntry* e = tt_.Probe(root_hash, 0)) {
    root_view = {e->abn, e->obn};
  }

  if (root_view.abn == 0) {
    BuildPv(root);
    return 1;  // Win proved for the side to move.
  }
  if (root_view.obn == 0) {
    return -1;  // Disproved: the attacker cannot force a win.
  }
  return 0;  // Unsolved (limits or resource-tainted disproof).
}

// =====================================================================
// Route-dependent verdicts
// =====================================================================

void BnsSolver::RecordVerdict(uint64_t hash, int ply, bns::ChildView v,
                              bool tainted, int anchor_ply, TaintKind kind,
                              BnsTTEntry* cached_entry) {
  if (tainted) {
    if (kind == TaintKind::kResource) resource_taint_seen_ = true;
    overrides_.push_back({hash, v, anchor_ply, kind});
    override_version_++;
    return;
  }
  BnsTTEntry* e = tt_.Store(hash, ply, cached_entry);
  e->abn = v.abn;
  e->obn = v.obn;
}

void BnsSolver::DropOverridesAtReturn(int ply) {
  if (overrides_.empty()) return;
  const auto end =
      std::remove_if(overrides_.begin(), overrides_.end(),
                     [ply](const PathOverride& o) {
                       return o.anchor_ply >= ply;
                     });
  const size_t erased = static_cast<size_t>(overrides_.end() - end);
  overrides_.erase(end, overrides_.end());
  if (erased) override_version_++;
}

// =====================================================================
// Core search
// =====================================================================

template <bool kOrNode, bool kPnDn>
void BnsSolver::SearchImpl(Pos pos, uint32_t abn_th, uint32_t obn_th,
                           int ply) {
  stats_.node_entries++;
  if (ply > stats_.max_ply) stats_.max_ply = ply;

  const uint64_t hash = HashPos(pos);
  bool first_visit = false;
  BnsTTEntry* own_entry = tt_.ProbeOrStore(hash, ply, &first_visit);
  const uint64_t own_tag = own_entry->tag;
  if (first_visit) stats_.first_visits++;

  Frame& f = frames_[ply];

  // ---- Terminal rules (side S = side to move) ----
  if (pos.OppCount() == 0) {
    // S has captured all opponent stones: S has won.
    RecordVerdict(hash, ply, kOrNode ? bns::MateView() : bns::NoMateView(),
                  false, 0, TaintKind::kPath, own_entry);
    return;
  }
  if (pos.OwnCount() == 0) {
    // S has no stones: S has lost.
    RecordVerdict(hash, ply, kOrNode ? bns::NoMateView() : bns::MateView(),
                  false, 0, TaintKind::kPath, own_entry);
    return;
  }

  // ---- Endgame-database probe (fresh nodes only, before movegen) ----
  if (first_visit && db_probe_) {
    const int v = db_probe_(pos);
    if (v != 0) {
      stats_.db_hits++;
      bns::ChildView view;
      if (v == 1) {       // win for the side to move
        view = kOrNode ? bns::MateView() : bns::NoMateView();
      } else if (v == 2) {  // loss for the side to move
        view = kOrNode ? bns::NoMateView() : bns::MateView();
      } else {            // draw: the attacker cannot force a win
        view = bns::NoMateView();
      }
      RecordVerdict(hash, ply, view, false, 0, TaintKind::kPath, own_entry);
      return;
    }
  }

  // ---- Move generation ----
  const size_t nn = GenMoves(geom_, pos, f.children, /*keep_dups=*/false);
  if (nn == 0) {
    // No legal moves: S has lost (stalemate rule).
    RecordVerdict(hash, ply, kOrNode ? bns::NoMateView() : bns::MateView(),
                  false, 0, TaintKind::kPath, own_entry);
    return;
  }
  const int n = static_cast<int>(nn);
  for (int i = 0; i < n; i++) {
    f.child_hash[i] = HashPos(f.children[i]);
    tt_.Prefetch(f.child_hash[i], ply + 1);
  }

  // With depth-keyed entries, a child's subtree can only write entries
  // strictly deeper than the sibling level, so sibling views are frozen
  // while the search is inside one child: after the initial pass, only
  // the child just returned needs re-probing — unless the override list
  // changed (overrides are hash-keyed and can mark siblings from below).
  int refresh_only = -1;
  uint64_t seen_override_version = ~uint64_t{0};
  for (;;) {
    // ---- Summarize current child views ----
    stats_.summaries++;
    const bool full_refresh =
        refresh_only < 0 || seen_override_version != override_version_;
    const int lo = full_refresh ? 0 : refresh_only;
    const int hi = full_refresh ? n : refresh_only + 1;
    if (overrides_.empty()) {
      // Normal route-independent fast path: one TT probe and one taint
      // byte per refreshed child.
      for (int i = lo; i < hi; i++) {
        if (BnsTTEntry* e = tt_.Probe(f.child_hash[i], ply + 1)) {
          f.views[i] = {e->abn, e->obn};
        } else {
          f.views[i] = {1, 1};
        }
        f.tainted[i] = false;
      }
    } else {
      for (int i = lo; i < hi; i++) {
        SourcedView sv = LookupChild(f.child_hash[i], ply + 1);
        f.views[i] = sv.view;
        f.tainted[i] = sv.tainted;
        if (sv.tainted) {
          f.anchor[i] = static_cast<int16_t>(sv.anchor_ply);
          f.kind[i] = static_cast<uint8_t>(sv.kind);
        }
      }
    }
    seen_override_version = override_version_;
    const bns::Summary s = bns::Summarize<kOrNode, kPnDn>(f.views, n);

    if (s.terminal()) {
      if (overrides_.empty()) {
        RecordVerdict(hash, ply, {s.abn, s.obn}, false, 0, TaintKind::kPath,
                      own_entry);
        return;
      }
      // Taint bookkeeping: a verdict determined by route-dependent child
      // verdicts is itself route-dependent and must stay out of the TT —
      // EXCEPT dependencies anchored at this very node. A cycle back to
      // this position exists on every route through it, so at the
      // anchoring node the path-dependence dissolves and the verdict is
      // unconditional (the loop head owns its loops). Resource
      // dependencies (depth cap) never dissolve.
      bool tainted = false;
      int anchor = 0;
      bool resource = false;
      auto absorb = [&](int i) {
        if (!f.tainted[i]) return;
        const TaintKind k = static_cast<TaintKind>(f.kind[i]);
        const int a = f.anchor[i];
        if (k == TaintKind::kResource) {
          resource = true;
          tainted = true;
          anchor = std::max(anchor, a);
        } else if (a < ply) {
          tainted = true;
          anchor = std::max(anchor, a);
        }
        // k == kPath && a == ply: self-anchored — intrinsic, no taint.
      };
      if (s.best >= 0) {
        // Decided by a single child (OR: proving child, AND: escaping
        // child).
        absorb(s.best);
      } else {
        // Decided by all children jointly. The combined verdict dies as
        // soon as any contributing override dies: anchor = max.
        for (int i = 0; i < n; i++) absorb(i);
      }
      RecordVerdict(hash, ply, {s.abn, s.obn}, tainted, anchor,
                    resource ? TaintKind::kResource : TaintKind::kPath,
                    own_entry);
      return;
    }

    // ---- Store unresolved node values ----
    {
      // Recursive descendants can evict this slot under real table
      // pressure. In the normal no-pressure case retain direct node
      // access instead of rescanning the cluster every pass.
      if (own_entry->tag != own_tag) own_entry = tt_.Store(hash, ply);
      BnsTTEntry* e = own_entry;
      e->abn = s.abn;
      e->obn = s.obn;
    }

    if (abn_th <= s.abn || obn_th <= s.obn) return;

    if (limit_hit_ || stats_.node_entries >= nodes_limit_) {
      limit_hit_ = true;
      return;
    }

    // ---- Descend into the selected child ----
    uint32_t child_abn_th, child_obn_th;
    bns::ChildThresholds<kOrNode>(s, f.views[s.best], abn_th, obn_th,
                                  &child_abn_th, &child_obn_th);

    const Pos child = f.children[s.best];
    const uint64_t chash = f.child_hash[s.best];

    // Route-terminal checks for the child: depth cap, then a full-path
    // cycle scan (Fanorona draws are infinite plays; a repetition is a
    // failure for the attacker on this route).
    bool handled = false;

    if (ply + 1 >= max_ply_) {
      stats_.depth_hits++;
      RecordVerdict(chash, ply + 1, bns::NoMateView(), true, ply,
                    TaintKind::kResource);
      handled = true;
    }

    if (!handled) {
      for (int idx = static_cast<int>(path_hashes_.size()) - 1; idx >= 0;
           idx--) {
        if (path_hashes_[idx] == chash) {
          stats_.rep_hits++;
          RecordVerdict(chash, ply + 1, bns::NoMateView(), true, idx,
                        TaintKind::kPath);
          handled = true;
          break;
        }
      }
    }

    if (handled) {
      refresh_only = s.best;
      continue;
    }

    path_hashes_.push_back(chash);
    SearchImpl<!kOrNode, kPnDn>(child, child_abn_th, child_obn_th, ply + 1);
    path_hashes_.pop_back();
    DropOverridesAtReturn(ply + 1);
    refresh_only = s.best;

    if (limit_hit_) return;
  }
}

// =====================================================================
// PV extraction
// =====================================================================

// TT walk, best effort for display only: at OR nodes follow any proved
// child; at AND nodes follow any child (proved preferred); stop at a
// terminal or when the chain breaks (evicted entries, cycles).
void BnsSolver::BuildPv(const Pos& root) {
  pv_.clear();
  Pos cur = root;
  std::vector<uint64_t> seen;
  seen.push_back(HashPos(root));
  for (int ply = 0; ply < max_ply_; ++ply) {
    pv_.push_back(cur);
    if (cur.OppCount() == 0 || cur.OwnCount() == 0) return;
    Pos children[kMaxMoves];
    const size_t n = GenMoves(geom_, cur, children, /*keep_dups=*/false);
    if (n == 0) return;
    const bool or_node = (ply % 2 == 0);
    int pick = -1;
    for (size_t i = 0; i < n; i++) {
      const uint64_t h = HashPos(children[i]);
      bool on_path = false;
      for (uint64_t ph : seen) {
        if (ph == h) {
          on_path = true;
          break;
        }
      }
      if (on_path) continue;
      BnsTTEntry* e = tt_.Probe(h, ply + 1);
      const bool proved = e && e->abn == 0;
      if (or_node) {
        if (proved) {
          pick = static_cast<int>(i);
          break;
        }
      } else {
        if (pick < 0) pick = static_cast<int>(i);  // any child
        if (proved) {
          pick = static_cast<int>(i);
          break;
        }
      }
    }
    if (pick < 0) return;
    cur = children[pick];
    seen.push_back(HashPos(cur));
  }
}

// Explicit instantiations.
#define INSTANTIATE_SEARCH(OR_NODE, PN_DN)                            \
  template void BnsSolver::SearchImpl<OR_NODE, PN_DN>(Pos, uint32_t, \
                                                      uint32_t, int)
INSTANTIATE_SEARCH(true, false);
INSTANTIATE_SEARCH(false, false);
INSTANTIATE_SEARCH(true, true);
INSTANTIATE_SEARCH(false, true);
#undef INSTANTIATE_SEARCH

}  // namespace fan
