// amazons — solver tests: df-pn/BNS verdicts vs brute-force ground truth
// on tiny variants, across both arithmetics and eval/zdd flag combos.
#include <cstdio>
#include <vector>

#include "bruteforce.h"
#include "solver.h"

using namespace amazons;

#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, \
                   __LINE__, #cond);                      \
      return 1;                                           \
    }                                                     \
  } while (0)

static SolveResult Run(const Position& root, Arith a, bool eval, bool zdd,
                       bool pn_init) {
  SolverOptions o;
  o.arith = a;
  o.use_eval = eval;
  o.use_zdd = zdd;
  o.use_pn_init = pn_init;
  o.tt_mb = 16;
  o.node_limit = 20000000;
  o.time_limit_ms = 120000;
  Solver s(o);
  return s.Solve(root);
}

int main() {
  struct Tiny {
    int w, h, wx, wy, bx, by;
  };
  const Tiny tinies[] = {
      {2, 2, 0, 0, 1, 1}, {2, 3, 0, 0, 1, 2}, {2, 3, 0, 1, 1, 1},
      {2, 4, 0, 0, 1, 3}, {3, 3, 0, 0, 2, 2}, {3, 3, 1, 0, 1, 2},
      {3, 3, 0, 2, 2, 0}, {3, 4, 0, 0, 2, 3}, {3, 4, 1, 1, 1, 2},
      {4, 4, 0, 0, 3, 3},
  };
  for (const Tiny& t : tinies) {
    const Position root =
        Position::OneQueen(t.w, t.h, t.wx, t.wy, t.bx, t.by);
    const bool expect_win = BruteForceStmWins(root);
    const SolveResult expect =
        expect_win ? SolveResult::kWin : SolveResult::kLoss;
    for (Arith a : {Arith::kBns, Arith::kPnDn}) {
      // Full feature set, plus a no-eval no-zdd no-init control run.
      for (auto flags : {std::tuple{true, true, true},
                         std::tuple{false, false, false}}) {
        const SolveResult r =
            Run(root, a, std::get<0>(flags), std::get<1>(flags),
                std::get<2>(flags));
        CHECK(r == expect);
      }
    }
    std::printf("  %dx%d W(%d,%d) B(%d,%d): %s OK\n", t.w, t.h, t.wx, t.wy,
                t.bx, t.by, expect_win ? "win " : "loss");
  }

  // PV sanity: on a solved win, following the PV must end in a terminal
  // position where the side to move has no moves.
  {
    const Position root = Position::OneQueen(3, 3, 0, 0, 2, 2);
    SolverOptions o;
    o.tt_mb = 16;
    o.node_limit = 20000000;
    Solver s(o);
    const SolveResult r = s.Solve(root);
    CHECK(r == SolveResult::kWin || r == SolveResult::kLoss);
    Position p = root;
    for (Move m : s.pv()) p.DoMove(m);
    std::vector<Move> rest;
    p.GenerateMoves(&rest);
    CHECK(rest.empty());  // PV runs to the end of the game
  }

  // Exhaustive WDL enumeration cross-check: every one-queen starting
  // placement on 2x3 must get the same verdict from the enumeration
  // table and from the df-pn/BNS solver.
  {
    std::vector<WdlEntry> table;
    CHECK(EnumerateOneQueenWdl(2, 3, &table));
    uint64_t wins = 0;
    for (const WdlEntry& e : table) wins += e.stm_wins;
    std::printf("  2x3 one-queen WDL: %zu positions, %llu wins\n",
                table.size(), (unsigned long long)wins);
    CHECK(!table.empty());
    for (int i = 0; i < 6; i++)
      for (int j = 0; j < 6; j++) {
        if (i == j) continue;
        const Position root = Position::OneQueen(2, 3, i % 2, i / 2, j % 2, j / 2);
        const Position c = root.Canonical();
        bool found = false, win = false;
        for (const WdlEntry& e : table)
          if (e.white == c.queens[0] && e.black == c.queens[1] &&
              e.burned == c.burned) {
            found = true;
            win = e.stm_wins;
            break;
          }
        CHECK(found);
        const SolveResult r = Run(root, Arith::kBns, true, true, true);
        CHECK(r == (win ? SolveResult::kWin : SolveResult::kLoss));
      }
  }

  std::printf("test_solver OK\n");
  return 0;
}
