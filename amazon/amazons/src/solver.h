/*
  amazons — df-pn/BNS strong solver (win/loss) for small Amazons boards.

  One AND/OR search engine, two arithmetics, following
  JHBR3/mate/bns.cc (itself after Okabe's route-branch-number search and
  cshogi's df-pn): every node is searched under (abn_th, obn_th)
  thresholds and iterates "summarize children -> recurse into selected
  child" until the node is proved, disproved, or a threshold is exceeded.

  Frame convention: unlike the shogi mate solver there is no fixed
  attacker.  Every node's numbers are kept in its own "side to move wins"
  frame; a child's view is swapped ({obn, abn} <-> {abn, obn}) when the
  parent reads it.  With that swap every node is existential ("does the
  side to move have a winning move"), so the search is a single OR-shaped
  recursion — negamax-style df-pn.  kPnDn=false selects BNS branch-number
  arithmetic, kPnDn=true classic proof/disproof numbers; everything else
  is shared, so `bench` compares the two algorithms in identical engines.

  Amazons burns one square per move, so the game graph is a DAG: no
  repetition handling, no path overrides, and the transposition table is
  keyed by canonical position hash only (maximum cross-depth sharing).

  Final verdicts are additionally stored in an exact ZDD verdict database
  (no hash collisions, compresses shared structure) and probed on every
  node visit.
*/
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "arith.h"
#include "board.h"
#include "tt.h"
#include "zdd.h"

namespace amazons {

enum class Arith {
  kBns,   // branch numbers (Okabe)
  kPnDn,  // proof/disproof numbers (classic df-pn) in the identical engine
};

enum class SolveResult { kWin, kLoss, kUnknown };

struct SolverOptions {
  size_t tt_mb = 64;
  uint64_t node_limit = 1000000;   // SearchImpl invocations
  int time_limit_ms = 0;           // 0 = no limit
  Arith arith = Arith::kBns;
  bool use_eval = true;            // static bounds verdicts at fresh nodes
  bool use_pn_init = true;         // eval-informed child initialization
  bool use_zdd = true;             // exact verdict DB
  bool move_ordering = true;       // cheap arrow-to-opponent ordering
  // Persistent WDL tables: preload an exact verdict DB before the search
  // and/or save the (grown) one after.  Verdicts are final, so a table
  // accumulates across runs; board size must match.
  std::string wdl_load;
  std::string wdl_save;
};

class Solver {
 public:
  using Clock = std::chrono::steady_clock;

  explicit Solver(SolverOptions opt = {}) : opt_(opt) {}

  // Strongly solve the root position (side to move wins?).
  SolveResult Solve(const Position& root);

  // After kWin: a winning move for the side to move.
  Move best_move() const { return best_move_; }
  // After Solve: principal variation (both sides' moves).
  const std::vector<Move>& pv() const { return pv_; }

  struct Stats {
    uint64_t node_entries = 0;
    uint64_t first_visits = 0;
    uint64_t summaries = 0;
    uint64_t eval_hits = 0;    // nodes decided by static bounds
    uint64_t zdd_hits = 0;     // nodes decided by the ZDD verdict DB
    int max_ply = 0;
    double seconds = 0;
  };
  const Stats& stats() const { return stats_; }
  const TransTable& tt() const { return tt_; }
  const VerdictDb& verdict_db() const { return vdb_; }
  VerdictDb& verdict_db() { return vdb_; }

  void stop() { stop_.store(true, std::memory_order_release); }

 private:
  // Per-ply scratch (heap-allocated once; keeps recursion frames small).
  struct Frame {
    std::vector<Move> moves;
    std::vector<uint64_t> child_hash;      // canonical child hashes
    std::vector<Position> child_canon;     // canonical child positions
    std::vector<bns::ChildView> views;     // in this node's frame (swapped)
    bns::ChildView child_init{1, 1};       // init for never-visited children
  };

  template <bool kPnDn>
  void SearchImpl(const Position& pos, uint32_t abn_th, uint32_t obn_th,
                  int ply);

  bool ShouldStop();
  // Child view in the child's own frame: TT entry, ZDD verdict, or the
  // (eval-informed) initialization for never-visited children.
  bns::ChildView LookupChild(const Position& child_canon, uint64_t chash,
                             bns::ChildView init);
  void RecordFinal(const Position& canon, uint64_t hash, TTEntry* entry,
                   bns::ChildView v);
  void ExtractPv(Position pos);

  SolverOptions opt_;
  TransTable tt_;
  VerdictDb vdb_;
  Stats stats_;
  SolveResult result_ = SolveResult::kUnknown;
  Move best_move_;
  std::vector<Move> pv_;
  std::vector<Frame> frames_;
  std::atomic<bool> stop_{false};
  Clock::time_point deadline_ = Clock::time_point::max();
  uint64_t stop_check_counter_ = 0;
  bool limit_hit_ = false;
};

}  // namespace amazons
