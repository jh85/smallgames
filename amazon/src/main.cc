/*
  amazons — command line driver.

    amazons solve WxH [options]     strongly solve one starting position
    amazons verify [options]        check known results + tiny variants
    amazons bench WxH [options]     BNS vs pn/dn comparison

  Options: --setup std|one  --arith bns|pn  --tt-mb N  --nodes N
           --seconds S  --eval on|off  --zdd on|off  --pn-init on|off
           --order on|off
*/
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bruteforce.h"
#include "solver.h"

namespace amazons {
namespace {

SolverOptions ParseOpts(int argc, char** argv, int from) {
  SolverOptions o;
  for (int i = from; i < argc; i++) {
    auto eat = [&](const char* name) -> const char* {
      const size_t n = std::strlen(name);
      if (std::strncmp(argv[i], name, n) == 0 && argv[i][n] == '=')
        return argv[i] + n + 1;
      if (std::strcmp(argv[i], name) == 0 && i + 1 < argc) return argv[++i];
      return nullptr;
    };
    const char* v;
    if ((v = eat("--arith")))
      o.arith = std::string(v) == "pn" ? Arith::kPnDn : Arith::kBns;
    else if ((v = eat("--tt-mb")))
      o.tt_mb = std::stoull(v);
    else if ((v = eat("--nodes")))
      o.node_limit = std::stoull(v);
    else if ((v = eat("--seconds")))
      o.time_limit_ms = std::stoi(v) * 1000;
    else if ((v = eat("--eval")))
      o.use_eval = std::string(v) == "on";
    else if ((v = eat("--zdd")))
      o.use_zdd = std::string(v) == "on";
    else if ((v = eat("--pn-init")))
      o.use_pn_init = std::string(v) == "on";
    else if ((v = eat("--order")))
      o.move_ordering = std::string(v) == "on";
    else if ((v = eat("--save-wdl")))
      o.wdl_save = v;
    else if ((v = eat("--load-wdl")))
      o.wdl_load = v;
  }
  return o;
}

const char* ArithName(Arith a) { return a == Arith::kBns ? "bns" : "pn/dn"; }

void PrintOutcome(const char* label, const Solver& s, SolveResult r,
                  const Position& root) {
  std::printf("%s: ", label);
  if (r == SolveResult::kWin)
    std::printf("FIRST PLAYER (%s) wins",
                root.stm == kWhite ? "White" : "Black");
  else if (r == SolveResult::kLoss)
    std::printf("SECOND PLAYER (%s) wins",
                root.stm == kWhite ? "Black" : "White");
  else
    std::printf("UNKNOWN (limit reached)");
  const auto& st = s.stats();
  std::printf("  [nodes=%llu first=%llu summaries=%llu ply=%d %.2fs]\n",
              (unsigned long long)st.node_entries,
              (unsigned long long)st.first_visits,
              (unsigned long long)st.summaries, st.max_ply, st.seconds);
  std::printf(
      "  tt: probes=%llu hits=%llu evictions=%llu | eval_hits=%llu "
      "zdd_hits=%llu | zdd: wins=%llu losses=%llu nodes=%zu\n",
      (unsigned long long)s.tt().probes(), (unsigned long long)s.tt().hits(),
      (unsigned long long)s.tt().evictions(),
      (unsigned long long)st.eval_hits, (unsigned long long)st.zdd_hits,
      (unsigned long long)s.verdict_db().num_wins(),
      (unsigned long long)s.verdict_db().num_losses(),
      s.verdict_db().zdd_nodes());
  if (r == SolveResult::kWin && !s.pv().empty()) {
    std::printf("  pv:");
    Position p = root;
    for (Move m : s.pv()) {
      std::printf(" %s", p.MoveString(m).c_str());
      p.DoMove(m);
    }
    std::printf("\n");
  }
}

SolveResult SolveAndPrint(const Position& root, const SolverOptions& o,
                          const char* label) {
  Solver s(o);
  if (!o.wdl_load.empty()) {
    if (s.verdict_db().Load(o.wdl_load.c_str(), root.w, root.h)) {
      std::printf("loaded WDL table %s: wins=%llu losses=%llu zdd_nodes=%zu\n",
                  o.wdl_load.c_str(),
                  (unsigned long long)s.verdict_db().num_wins(),
                  (unsigned long long)s.verdict_db().num_losses(),
                  s.verdict_db().zdd_nodes());
    } else {
      std::fprintf(stderr, "warning: could not load WDL table %s\n",
                   o.wdl_load.c_str());
    }
  }
  const SolveResult r = s.Solve(root);
  PrintOutcome(label, s, r, root);
  if (!o.wdl_save.empty()) {
    if (!s.verdict_db().Save(o.wdl_save.c_str(), root.w, root.h))
      std::fprintf(stderr, "warning: could not save WDL table %s\n",
                   o.wdl_save.c_str());
  }
  return r;
}

int CmdSolve(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: amazons solve WxH [options]\n");
    return 1;
  }
  int w, h;
  if (std::sscanf(argv[2], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 ||
      w * h > 64) {
    std::fprintf(stderr, "bad board size '%s'\n", argv[2]);
    return 1;
  }
  SolverOptions o = ParseOpts(argc, argv, 3);
  std::string setup = "std";
  for (int i = 3; i + 1 < argc; i++)
    if (std::strcmp(argv[i], "--setup") == 0) setup = argv[i + 1];
  Position root;
  if (setup == "one") {
    // Default tiny placement: white lower-left area, black upper-right.
    root = Position::OneQueen(w, h, 0, 0, w - 1, h - 1);
  } else {
    if (w < 4 || h < 4) {
      std::fprintf(stderr, "std setup needs w,h >= 4\n");
      return 1;
    }
    root = Position::Standard(w, h);
  }
  std::printf("%s", root.ToString().c_str());
  SolveAndPrint(root, o, "solve");
  return 0;
}

// One row of the verify suite.
struct VerifyCase {
  const char* name;
  Position root;
  SolveResult expect;      // kUnknown = just run, don't check
  uint64_t node_limit;
  int time_limit_ms;
};

int CmdVerify(int argc, char** argv) {
  SolverOptions base = ParseOpts(argc, argv, 2);
  int fails = 0;

  // 1. Tiny one-queen variants cross-checked against brute force.
  std::printf("== tiny variants vs brute force ==\n");
  struct Tiny {
    int w, h, wx, wy, bx, by;
  };
  const Tiny tinies[] = {
      {2, 2, 0, 0, 1, 1}, {2, 3, 0, 0, 1, 2}, {2, 3, 0, 1, 1, 1},
      {3, 3, 0, 0, 2, 2}, {3, 3, 1, 0, 1, 2}, {3, 4, 0, 0, 2, 3},
      {3, 4, 1, 1, 1, 2}, {4, 4, 0, 0, 3, 3},
  };
  for (const Tiny& t : tinies) {
    const Position root =
        Position::OneQueen(t.w, t.h, t.wx, t.wy, t.bx, t.by);
    uint64_t bnodes = 0;
    const bool bf = BruteForceStmWins(root, &bnodes);
    SolverOptions o = base;
    o.node_limit = 10000000;
    o.time_limit_ms = 60000;
    Solver s(o);
    const SolveResult r = s.Solve(root);
    const bool got = r == SolveResult::kWin;
    const bool ok = r != SolveResult::kUnknown && got == bf;
    if (!ok) fails++;
    char name[64];
    std::snprintf(name, sizeof name, "%dx%d one-queen W(%d,%d) B(%d,%d)", t.w,
                  t.h, t.wx, t.wy, t.bx, t.by);
    std::printf("%-36s brute=%s solver=%s nodes=%llu %s\n", name,
                bf ? "win " : "loss",
                r == SolveResult::kWin      ? "win "
                : r == SolveResult::kLoss   ? "loss"
                                            : "unknown",
                (unsigned long long)s.stats().node_entries,
                ok ? "OK" : "MISMATCH");
  }

  // 2. Boards with established verdicts.  Only 4x4 is within reach of the
  // search solver with the corrected (sound) static eval; 4x5/5x4 verdicts
  // are established by the exhaustive wdlretro tables (see README.md) and
  // are kept here as short smoke attempts.
  std::printf("== known results ==\n");
  struct Known {
    int w, h;
    SolveResult expect;
    uint64_t nodes;
    int seconds;
  };
  const Known knowns[] = {
      {4, 4, SolveResult::kLoss, 50000000ull, 600},   // 2nd player win
      {4, 5, SolveResult::kUnknown, 20000000ull, 60},  // table: 2nd player win
      {5, 4, SolveResult::kUnknown, 20000000ull, 60},  // table: see README
      {5, 5, SolveResult::kUnknown, 20000000ull, 60},  // attempt
  };
  for (const Known& k : knowns) {
    if (base.time_limit_ms > 0 && k.seconds > base.time_limit_ms / 1000) {
      // Honor a global --seconds cap as a per-board budget override.
    }
    SolverOptions o = base;
    o.node_limit = k.nodes;
    if (o.time_limit_ms == 0) o.time_limit_ms = k.seconds * 1000;
    const Position root = Position::Standard(k.w, k.h);
    char label[64];
    std::snprintf(label, sizeof label, "%dx%d standard", k.w, k.h);
    Solver s(o);
    const SolveResult r = s.Solve(root);
    PrintOutcome(label, s, r, root);
    if (k.expect != SolveResult::kUnknown) {
      const bool ok = r == k.expect;
      if (!ok) fails++;
      std::printf("  expected %s -> %s\n",
                  k.expect == SolveResult::kWin ? "1st player win"
                                                : "2nd player win",
                  ok ? "OK" : "MISMATCH");
    }
  }

  std::printf(fails ? "VERIFY FAILED (%d)\n" : "VERIFY OK\n", fails);
  return fails ? 1 : 0;
}

// Exhaustive WDL table for one-amazon-each on WxH:
//   amazons wdl WxH [--limit N] [--table FILE] [--save-wdl FILE]
// Prints per-board summary (positions / wins / losses; Amazons has no
// draws), optionally writes a text table of canonical positions and/or a
// ZDD WDL database loadable by `solve --load-wdl`.
int CmdWdl(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: amazons wdl WxH [--limit N] [--table FILE]\n");
    return 1;
  }
  int w, h;
  if (std::sscanf(argv[2], "%dx%d", &w, &h) != 2 || w < 1 || h < 1 ||
      w * h > 64) {
    std::fprintf(stderr, "bad board size '%s'\n", argv[2]);
    return 1;
  }
  uint64_t limit = 100000000ull;
  std::string table, save_wdl;
  for (int i = 3; i + 1 < argc; i++) {
    if (std::strcmp(argv[i], "--limit") == 0) limit = std::stoull(argv[++i]);
    if (std::strcmp(argv[i], "--table") == 0) table = argv[++i];
    if (std::strcmp(argv[i], "--save-wdl") == 0) save_wdl = argv[++i];
  }
  std::vector<WdlEntry> entries;
  const auto t0 = std::chrono::steady_clock::now();
  const bool complete = EnumerateOneQueenWdl(w, h, &entries, limit);
  const double secs = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
  uint64_t wins = 0;
  for (const WdlEntry& e : entries) wins += e.stm_wins;
  std::printf(
      "%dx%d one-amazon WDL: %s, canonical positions=%llu wins=%llu "
      "losses=%llu draws=0 (%.2fs)\n",
      w, h, complete ? "complete" : "PARTIAL (limit reached)",
      (unsigned long long)entries.size(), (unsigned long long)wins,
      (unsigned long long)(entries.size() - wins), secs);
  if (!table.empty()) {
    FILE* f = std::fopen(table.c_str(), "w");
    if (!f) {
      std::fprintf(stderr, "cannot write %s\n", table.c_str());
      return 1;
    }
    std::fprintf(f,
                 "# amazons one-amazon WDL table, board %dx%d, canonical "
                 "positions (white to move)\n# white black burned verdict "
                 "(hex bitboards; verdict W = side to move wins, L = loses; "
                 "no draws in Amazons)\n",
                 w, h);
    for (const WdlEntry& e : entries)
      std::fprintf(f, "%llx %llx %llx %c\n", (unsigned long long)e.white,
                   (unsigned long long)e.black, (unsigned long long)e.burned,
                   e.stm_wins ? 'W' : 'L');
    std::fclose(f);
    std::printf("wrote %s (%zu entries)\n", table.c_str(), entries.size());
  }
  if (!save_wdl.empty()) {
    VerdictDb db;
    for (const WdlEntry& e : entries) {
      Position p;
      p.w = w;
      p.h = h;
      p.queens[kWhite] = e.white;
      p.queens[kBlack] = e.black;
      p.burned = e.burned;
      p.stm = kWhite;  // entries are canonical already
      if (e.stm_wins)
        db.InsertWin(p);
      else
        db.InsertLoss(p);
    }
    if (!db.Save(save_wdl.c_str(), w, h)) {
      std::fprintf(stderr, "cannot write %s\n", save_wdl.c_str());
      return 1;
    }
    std::printf("wrote %s (zdd_nodes=%zu)\n", save_wdl.c_str(),
                db.zdd_nodes());
  }
  return complete ? 0 : 2;
}

int CmdBench(int argc, char** argv) {  if (argc < 3) {
    std::fprintf(stderr, "usage: amazons bench WxH [options]\n");
    return 1;
  }
  int w, h;
  if (std::sscanf(argv[2], "%dx%d", &w, &h) != 2 || w * h > 64) return 1;
  SolverOptions base = ParseOpts(argc, argv, 3);
  const Position root = Position::Standard(w, h);
  SolveResult results[2];
  for (int i = 0; i < 2; i++) {
    SolverOptions o = base;
    o.arith = i == 0 ? Arith::kBns : Arith::kPnDn;
    Solver s(o);
    const auto t0 = Solver::Clock::now();
    results[i] = s.Solve(root);
    char label[64];
    std::snprintf(label, sizeof label, "%dx%d %s", w, h, ArithName(o.arith));
    PrintOutcome(label, s, results[i], root);
  }
  if (results[0] != results[1] && results[0] != SolveResult::kUnknown &&
      results[1] != SolveResult::kUnknown) {
    std::printf("VERDICT DISAGREEMENT between arithmetics!\n");
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace amazons

int main(int argc, char** argv) {
  using namespace amazons;
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: amazons solve|verify|bench ...\n");
    return 1;
  }
  if (std::strcmp(argv[1], "solve") == 0) return CmdSolve(argc, argv);
  if (std::strcmp(argv[1], "verify") == 0) return CmdVerify(argc, argv);
  if (std::strcmp(argv[1], "bench") == 0) return CmdBench(argc, argv);
  if (std::strcmp(argv[1], "wdl") == 0) return CmdWdl(argc, argv);
  std::fprintf(stderr, "unknown command '%s'\n", argv[1]);
  return 1;
}
