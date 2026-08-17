/*
  Fanorona — BNS (Branch Number Search) weak-solve solver.

  Ported from the JHBR3 Shogi checkmate solver (mate/bns.{h,cc}, GPLv3),
  which implements the route-branch-number search of:
    岡部文洋「経路分枝数を用いた詰め将棋解図について」
    (Fumihiro Okabe, "Application for solving tsume shogi problem by
     route branch number")

  Goal here: prove that the side to move at the root (the "attacker")
  forces a win, i.e. captures all opponent stones. OR node = attacker to
  move, AND node = defender to move; both expand all legal moves (there
  is no check/restriction in Fanorona). A repetition on a route is a
  failure for the attacker on that route and is kept out of the
  transposition table via path-scoped overrides.

  The bns:: branch-number arithmetic below is copied verbatim from the
  reference implementation. Search shape (recursive threshold iteration
  over a depth-keyed transposition table) follows cshogi/dlshogi's
  dfpn_inner (src/dfpn.cpp, GPLv3) as in the reference.
*/

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "fanorona.h"

namespace fan {

// =====================================================================
// Pure branch-number arithmetic (unit-testable without a board)
// =====================================================================

namespace bns {

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
  bool proved = false;    // mate proved at this node
  bool disproved = false; // no-mate proved at this node

  bool terminal() const { return proved || disproved; }
};

// Summarize children into node values.
//
// kOrNode: attacker to move. kPnDn: use proof/disproof-number arithmetic
// (sum over siblings) instead of branch numbers (count of unresolved
// siblings) — the control mode for algorithm comparisons; everything
// else in the solver is shared.
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
    if (rel < best_rel ||
        (kPnDn && rel == best_rel && opp < best_opp)) {
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
    // OR: every child disproved -> no mate. AND: every child proved
    // (every defense mated) -> mate.
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

constexpr ChildView MateView() { return ChildView{0, kInf}; }
constexpr ChildView NoMateView() { return ChildView{kInf, 0}; }

}  // namespace bns

// =====================================================================
// Transposition table
// =====================================================================

// Fixed-size, 2-way clustered table keyed by (position hash, ply from
// root). Generation-based lazy clearing.
//
// The ply is part of the key (as in cshogi/dlshogi): it layers the
// search graph so that a position's entry can never feed back into its
// own value through a transposition at a different depth — without
// this, cross-depth sharing creates circular value dependencies and the
// threshold iteration can enter a limit cycle instead of converging.
// The price is that transpositions are only shared at equal depth.
// Route-dependent results are never stored here (see PathOverride).
struct BnsTTEntry {
  // Upper 48 bits: fingerprint of (position hash, ply). Lower 16 bits:
  // search generation. Mixing ply into the fingerprint as well as the
  // cluster index avoids storing it separately. Two complete clusters fit in
  // one 64-byte cache line.
  uint64_t tag = 0;
  uint32_t abn = 1;
  uint32_t obn = 1;

  bool final_result() const { return abn == 0 || obn == 0; }
};
static_assert(sizeof(BnsTTEntry) == 16, "BnsTTEntry should stay compact");

// Deleter for calloc-backed tables (lazy zero pages from the OS, so
// allocating a large table costs no page-touching until use).
struct BnsFreeDeleter {
  void operator()(void* p) const { std::free(p); }
};

class BnsTT {
 public:
  static constexpr int kClusterSize = 2;

  void Resize(size_t mb) {
    size_t bytes = mb << 20;
    const size_t want = bytes / sizeof(BnsTTEntry);
    size_t n = size_t{1} << 12;
    while (n * 2 <= want) n *= 2;
    if (n != num_entries_) {
      table_.reset(
          static_cast<BnsTTEntry*>(std::calloc(n, sizeof(BnsTTEntry))));
      num_entries_ = n;
      mask_ = (n - 1) & ~static_cast<size_t>(kClusterSize - 1);
      gen_ = 0;
    }
  }

  void NewSearch() {
    if (!table_) Resize(4);
    if (++gen_ == 0) {  // uint16 wrap: hard-clear once every 65535 searches
      for (size_t i = 0; i < num_entries_; i++) table_[i] = BnsTTEntry{};
      gen_ = 1;
    }
    probes_ = hits_ = stores_ = 0;
  }

  // Returns the matching live entry, or nullptr.
  BnsTTEntry* Probe(uint64_t hash, int ply) {
    probes_++;
    const uint64_t mixed = Mix(hash, ply);
    BnsTTEntry* c = &table_[IndexOf(mixed)];
    const uint64_t tag = TagOf(mixed);
    for (int i = 0; i < kClusterSize; i++) {
      if (c[i].tag == tag) {
        hits_++;
        return &c[i];
      }
      // Stores fill a cluster from the first stale slot and entries are never
      // deleted within a generation. Therefore a stale slot also terminates
      // the lookup; no later slot can hold a live match.
      if (GenerationOf(c[i]) != gen_) return nullptr;
    }
    return nullptr;
  }

  // Combined lookup/insertion for the node being searched. This avoids a
  // second cluster scan on the dominant first-visit path.
  BnsTTEntry* ProbeOrStore(uint64_t hash, int ply, bool* inserted) {
    probes_++;
    const uint64_t mixed = Mix(hash, ply);
    BnsTTEntry* c = &table_[IndexOf(mixed)];
    const uint64_t tag = TagOf(mixed);
    for (int i = 0; i < kClusterSize; i++) {
      if (c[i].tag == tag) {
        hits_++;
        *inserted = false;
        return &c[i];
      }
      if (GenerationOf(c[i]) != gen_) {
        stores_++;
        c[i].tag = tag;
        c[i].abn = 1;
        c[i].obn = 1;
        *inserted = true;
        return &c[i];
      }
    }
    // Cluster full: always overwrite the first slot.
    stores_++;
    c[0].tag = tag;
    c[0].abn = 1;
    c[0].obn = 1;
    *inserted = true;
    return &c[0];
  }

  void Prefetch(uint64_t hash, int ply) const {
    __builtin_prefetch(&table_[IndexOf(Mix(hash, ply))]);
  }

  // Returns an entry to write for this key: the live match if present,
  // else a victim (empty/stale first, then the first slot).
  BnsTTEntry* Store(uint64_t hash, int ply, BnsTTEntry* cached = nullptr) {
    stores_++;
    const uint64_t mixed = Mix(hash, ply);
    const uint64_t tag = TagOf(mixed);
    if (cached && cached->tag == tag) return cached;
    BnsTTEntry* c = &table_[IndexOf(mixed)];
    for (int i = 0; i < kClusterSize; i++) {
      if (c[i].tag == tag) return &c[i];
      if (GenerationOf(c[i]) != gen_) {
        c[i].tag = tag;
        c[i].abn = 1;
        c[i].obn = 1;
        return &c[i];
      }
    }
    c[0].tag = tag;
    c[0].abn = 1;
    c[0].obn = 1;
    return &c[0];
  }

  size_t num_entries() const { return num_entries_; }
  uint64_t probes() const { return probes_; }
  uint64_t hits() const { return hits_; }
  uint64_t stores() const { return stores_; }

 private:
  static uint64_t Mix(uint64_t hash, int ply) {
    return hash + static_cast<uint64_t>(ply) * 0x9E3779B97F4A7C15ull;
  }
  size_t IndexOf(uint64_t mixed) const { return mixed & mask_; }
  uint64_t TagOf(uint64_t mixed) const {
    return (mixed & 0xffffffffffff0000ull) | gen_;
  }
  static uint16_t GenerationOf(const BnsTTEntry& entry) {
    return static_cast<uint16_t>(entry.tag);
  }

  std::unique_ptr<BnsTTEntry[], BnsFreeDeleter> table_;
  size_t num_entries_ = 0;
  size_t mask_ = 0;
  uint16_t gen_ = 0;
  uint64_t probes_ = 0, hits_ = 0, stores_ = 0;
};

// =====================================================================
// BnsSolver
// =====================================================================

class BnsSolver {
 public:
  struct Stats {
    uint64_t node_entries = 0;  // SearchImpl invocations
    uint64_t first_visits = 0;  // node entries that created the TT entry
    uint64_t summaries = 0;     // summarize passes
    uint64_t rep_hits = 0;      // repetition terminals
    uint64_t depth_hits = 0;    // depth-cap terminals
    uint64_t db_hits = 0;       // endgame-database verdicts
    int max_ply = 0;
    uint64_t tt_probes = 0, tt_hits = 0, tt_stores = 0;
  };

  explicit BnsSolver(const Geom& g, size_t tt_mb = 1024);

  void set_arith_pn(bool v);  // false = BNS branch numbers (default),
                              // true = proof/disproof numbers
  void set_max_ply(int p);
  // f(pos) -> 0 unknown, 1 win / 2 loss / 3 draw for the side to move.
  void set_db_probe(std::function<int(const Pos&)> f);
  void set_nodes_limit(uint64_t n);

  // Returns 1 if the side to move forces a win (proved), -1 if disproved
  // (attacker cannot force a win: draw or loss), 0 if unresolved (limits).
  int Solve(const Pos& root);

  uint64_t nodes() const { return stats_.node_entries; }
  std::vector<Pos> pv() const { return pv_; }  // best-effort PV after Solve
  const Stats& stats() const { return stats_; }

 private:
  // Route-dependent verdicts (repetition, depth cap) valid only while
  // the anchoring ancestor stays on the DFS path. anchor_ply is the ply
  // of that ancestor; entries are dropped when the search unwinds past
  // it. kind distinguishes path-dependence from resource limits (a
  // resource-limited "no win" must never be reported as a real disproof).
  enum class TaintKind : uint8_t { kPath, kResource };

  struct PathOverride {
    uint64_t hash;
    bns::ChildView view;
    int anchor_ply;
    TaintKind kind;
  };

  // A child's numbers plus where they came from.
  struct SourcedView {
    bns::ChildView view;
    bool tainted = false;
    int anchor_ply = 0;
    TaintKind kind = TaintKind::kPath;
  };

  // Per-ply scratch space (heap-allocated once; keeps recursion frames
  // small). Sized for the movegen worst case.
  struct Frame {
    Pos children[kMaxMoves];
    uint64_t child_hash[kMaxMoves];
    bns::ChildView views[kMaxMoves];
    int16_t anchor[kMaxMoves];
    uint8_t tainted[kMaxMoves];
    uint8_t kind[kMaxMoves];
  };

  template <bool kOrNode, bool kPnDn>
  void SearchImpl(Pos pos, uint32_t abn_th, uint32_t obn_th, int ply);

  SourcedView LookupChild(uint64_t hash, int ply) {
    SourcedView sv;
    if (!overrides_.empty()) {
      for (auto it = overrides_.rbegin(); it != overrides_.rend(); ++it) {
        if (it->hash == hash) {
          sv.view = it->view;
          sv.tainted = true;
          sv.anchor_ply = it->anchor_ply;
          sv.kind = it->kind;
          return sv;
        }
      }
    }
    if (BnsTTEntry* e = tt_.Probe(hash, ply)) {
      sv.view = {e->abn, e->obn};
    }
    return sv;
  }

  void RecordVerdict(uint64_t hash, int ply, bns::ChildView v, bool tainted,
                     int anchor_ply, TaintKind kind,
                     BnsTTEntry* cached_entry = nullptr);
  void DropOverridesAtReturn(int ply);

  // PV extraction by TT walk (OR node: any proved child; AND node: any
  // child, proved preferred; stop at terminal or when the chain breaks).
  void BuildPv(const Pos& root);

  // --- Members ---
  Geom geom_;
  size_t tt_mb_;
  uint64_t nodes_limit_ = ~uint64_t{0};
  BnsTT tt_;
  bool arith_pn_ = false;
  int max_ply_ = 512;
  std::function<int(const Pos&)> db_probe_;

  Stats stats_;
  std::vector<Pos> pv_;
  bool limit_hit_ = false;
  bool resource_taint_seen_ = false;
  std::unique_ptr<Frame[]> frames_;
  size_t frames_size_ = 0;

  std::vector<uint64_t> path_hashes_;
  std::vector<PathOverride> overrides_;
  uint64_t override_version_ = 0;
};

}  // namespace fan
