#include "solver.h"

#include <algorithm>

#include "eval.h"

namespace amazons {

namespace {

constexpr uint32_t kPnInitCap = 64;

// Cheap move ordering: arrows landing next to an opponent queen (and queen
// destinations next to one) first — these tend to restrict the opponent,
// who is the side to move at the child.
void OrderMoves(const Position& pos, std::vector<Move>* moves) {
  const Color opp = Opp(pos.stm);
  Bitboard dil = 0;
  Bitboard oq = pos.queens[opp];
  while (oq) {
    const int sq = __builtin_ctzll(oq);
    oq &= oq - 1;
    const int x = sq % pos.w, y = sq / pos.w;
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++) {
        const int nx = x + dx, ny = y + dy;
        if (nx < 0 || nx >= pos.w || ny < 0 || ny >= pos.h) continue;
        dil |= uint64_t{1} << (ny * pos.w + nx);
      }
  }
  std::stable_sort(moves->begin(), moves->end(), [&](Move a, Move b) {
    const int sa = 2 * ((dil >> a.arrow) & 1) + ((dil >> a.to) & 1);
    const int sb = 2 * ((dil >> b.arrow) & 1) + ((dil >> b.to) & 1);
    return sa > sb;
  });
}

}  // namespace

bool Solver::ShouldStop() {
  if (limit_hit_) return true;
  if (stop_.load(std::memory_order_acquire)) {
    limit_hit_ = true;
    return true;
  }
  if ((++stop_check_counter_ & 0x3FF) == 0) {
    if (stats_.node_entries >= opt_.node_limit ||
        Clock::now() >= deadline_) {
      limit_hit_ = true;
      return true;
    }
  }
  return false;
}

bns::ChildView Solver::LookupChild(const Position& child_canon,
                                   uint64_t chash,
                                   bns::ChildView init) {
  if (TTEntry* e = tt_.Probe(chash)) {
    return {e->abn, e->obn};
  }
  // No TT entry: the exact ZDD verdict DB may still know this position
  // (cross-search persistence, or a TT eviction).
  if (opt_.use_zdd) {
    const int v = vdb_.Probe(child_canon);
    if (v > 0) {
      stats_.zdd_hits++;
      return bns::WinView();
    }
    if (v < 0) {
      stats_.zdd_hits++;
      return bns::LossView();
    }
  }
  return init;
}

void Solver::RecordFinal(const Position& canon, uint64_t hash, TTEntry* entry,
                         bns::ChildView v) {
  entry->abn = v.abn;
  entry->obn = v.obn;
  if (opt_.use_zdd) {
    if (v.abn == 0)
      vdb_.InsertWin(canon);
    else
      vdb_.InsertLoss(canon);
  }
  (void)hash;
}

template <bool kPnDn>
void Solver::SearchImpl(const Position& pos, uint32_t abn_th, uint32_t obn_th,
                        int ply) {
  stats_.node_entries++;
  if (ply > stats_.max_ply) stats_.max_ply = ply;
  if (ShouldStop()) return;

  const Position canon = pos.Canonical();
  const uint64_t hash = canon.Hash();
  bool inserted = false;
  TTEntry* entry = tt_.ProbeOrStore(hash, &inserted);
  if (inserted) stats_.first_visits++;
  const uint64_t own_tag = entry->tag;

  Frame& f = frames_[ply];

  // One-time setup per visit: static bounds, move generation, child keys.
  // (The frame is per-ply scratch shared by all nodes at this depth, so it
  // must be refilled on every visit.)
  {
    // Static bounds verdict at fresh nodes (recorded in the TT on first
    // visit, so revisits skip the evaluation entirely).
    int margin = 0;
    if (inserted && (opt_.use_eval || opt_.use_pn_init)) {
      const EvalResult er = EvaluateBounds(pos);
      if (opt_.use_eval && er.decided) {
        stats_.eval_hits++;
        RecordFinal(canon, hash, entry,
                    er.winner == pos.stm ? bns::WinView() : bns::LossView());
        return;
      }
      margin = er.Margin(pos);
    }
    // Initial view for never-visited children (eval-informed pn init).
    bns::ChildView init{1, 1};
    if (opt_.use_pn_init && margin != 0) {
      // Children have the opposite sign of margin (opponent to move).
      const int cm = -margin;
      const uint32_t big =
          static_cast<uint32_t>(std::min(std::abs(cm) + 1, (int)kPnInitCap));
      init = cm > 0 ? bns::ChildView{1, big} : bns::ChildView{big, 1};
    }
    pos.GenerateMoves(&f.moves);
    if (f.moves.empty()) {
      // No legal move: side to move loses.
      RecordFinal(canon, hash, entry, bns::LossView());
      return;
    }
    if (opt_.move_ordering && f.moves.size() > 2)
      OrderMoves(pos, &f.moves);
    const int n = static_cast<int>(f.moves.size());
    f.child_hash.resize(n);
    f.child_canon.resize(n);
    f.views.resize(n);
    f.child_init = init;
    for (int i = 0; i < n; i++) {
      Position child = pos;
      child.DoMove(f.moves[i]);
      f.child_canon[i] = child.Canonical();
      f.child_hash[i] = f.child_canon[i].Hash();
      tt_.Prefetch(f.child_hash[i]);
    }
  }

  while (true) {
    if (entry->final_result()) return;
    // Threshold exceeded: back off, leaving the tightened numbers in the
    // table.
    if (entry->abn >= abn_th || entry->obn >= obn_th) return;

    const int n = static_cast<int>(f.moves.size());
    for (int i = 0; i < n; i++) {
      const bns::ChildView cv =
          LookupChild(f.child_canon[i], f.child_hash[i], f.child_init);
      // Swap into this node's frame: the child's win is my loss.
      f.views[i] = {cv.obn, cv.abn};
    }

    const bns::Summary s = bns::Summarize<true, kPnDn>(f.views.data(), n);
    stats_.summaries++;
    if (s.terminal()) {
      RecordFinal(canon, hash, entry, {s.abn, s.obn});
      return;
    }
    entry->abn = s.abn;
    entry->obn = s.obn;

    uint32_t cth_a, cth_o;
    bns::ChildThresholds<true>(s, f.views[s.best], abn_th, obn_th, &cth_a,
                               &cth_o);
    Position child = pos;
    child.DoMove(f.moves[s.best]);
    // Thresholds swap frames along with the numbers.
    SearchImpl<kPnDn>(child, cth_o, cth_a, ply + 1);
    if (limit_hit_) return;
    // The child's subtree may have evicted our entry; re-acquire if so.
    if (entry->tag != own_tag) {
      bool reinserted = false;
      entry = tt_.ProbeOrStore(hash, &reinserted);
    }
  }
}

SolveResult Solver::Solve(const Position& root) {
  tt_.Resize(opt_.tt_mb);
  tt_.NewSearch();
  // vdb_ deliberately survives Solve() calls: verdicts are final, so a
  // preloaded WDL table stays valid and keeps growing across runs.
  stats_ = Stats();
  result_ = SolveResult::kUnknown;
  pv_.clear();
  limit_hit_ = false;
  stop_.store(false, std::memory_order_release);
  stop_check_counter_ = 0;
  frames_.clear();
  frames_.resize(root.NumSquares() + 2);
  deadline_ = opt_.time_limit_ms > 0
                  ? Clock::now() + std::chrono::milliseconds(opt_.time_limit_ms)
                  : Clock::time_point::max();

  const auto t0 = Clock::now();
  const uint64_t root_hash = root.Canonical().Hash();
  while (true) {
    if (opt_.arith == Arith::kBns)
      SearchImpl<false>(root, bns::kInf, bns::kInf, 0);
    else
      SearchImpl<true>(root, bns::kInf, bns::kInf, 0);
    TTEntry* e = tt_.Probe(root_hash);
    if (e && e->final_result()) {
      result_ = e->abn == 0 ? SolveResult::kWin : SolveResult::kLoss;
      break;
    }
    // With infinite root thresholds a call only returns early on a limit.
    if (limit_hit_) break;
  }
  stats_.seconds =
      std::chrono::duration<double>(Clock::now() - t0).count();
  if (result_ != SolveResult::kUnknown) ExtractPv(root);
  return result_;
}

void Solver::ExtractPv(Position pos) {
  pv_.clear();
  best_move_ = Move{};
  for (int depth = 0; depth < 200; depth++) {
    const uint64_t hash = pos.Canonical().Hash();
    TTEntry* e = tt_.Probe(hash);
    if (!e || !e->final_result()) break;
    std::vector<Move> moves;
    pos.GenerateMoves(&moves);
    if (moves.empty()) break;
    // Winning node: a move whose child is a proven loss for the opponent.
    // Losing node: any proven-win child (they all are); pick the first.
    const bool winning = e->abn == 0;
    Move chosen{};
    bool found = false;
    for (Move m : moves) {
      Position child = pos;
      child.DoMove(m);
      TTEntry* ce = tt_.Probe(child.Canonical().Hash());
      if (!ce) continue;
      if (winning ? ce->obn == 0 : ce->abn == 0) {
        chosen = m;
        found = true;
        break;
      }
    }
    if (!found) break;
    if (depth == 0) best_move_ = chosen;
    pv_.push_back(chosen);
    pos.DoMove(chosen);
  }
}

// Explicit instantiations.
template void Solver::SearchImpl<false>(const Position&, uint32_t, uint32_t,
                                        int);
template void Solver::SearchImpl<true>(const Position&, uint32_t, uint32_t,
                                       int);

}  // namespace amazons
