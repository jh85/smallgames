/*
  Fanorona WDL tools — CLI.

  Commands:
    perft    <WxH> <depth> [--stepwise] [--nodup] [--setup S]
    idextest <WxH> [--d4|--d2]
    build    <WxH> [--d4|--d2] [--out DIR] [--max-stones S] [--threads T]
    query    <WxH> [--d4|--d2] [--out DIR] --pos S
    root     <WxH> [--d4|--d2] [--out DIR]     value of the initial position
    verify   <WxH> [--d4|--d2] [--out DIR]     brute-force cross-check (small boards)
    reach    <WxH> [--pos S]                   reachable-position count (small boards)
    solve    <WxH> [--pn] [--db DIR] [--pos S] [--tt-mb M] [--max-ply P]

  Symmetry folds: --d4 = D4 (square boards, odd side); --d2 = D2
  (rectangular boards, both sides odd). A table built with a fold must be
  queried with the same flag.
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bns.h"
#include "fanorona.h"
#include "index.h"
#include "retro.h"

using namespace fan;

namespace {

std::string GetArg(int argc, char** argv, const char* key,
                   const std::string& def = "") {
  for (int i = 2; i + 1 < argc; i++)
    if (!strcmp(argv[i], key)) return argv[i + 1];
  return def;
}
bool HasFlag(int argc, char** argv, const char* key) {
  for (int i = 2; i < argc; i++)
    if (!strcmp(argv[i], key)) return true;
  return false;
}

// 0 = plain, 2 = D2 fold (--d2), 4 = D4 fold (--d4).
int SymMode(int argc, char** argv) {
  if (HasFlag(argc, argv, "--d4")) return 4;
  if (HasFlag(argc, argv, "--d2")) return 2;
  return 0;
}
const char* SymSuffix(int argc, char** argv) {
  if (HasFlag(argc, argv, "--d4")) return "_d4";
  if (HasFlag(argc, argv, "--d2")) return "_d2";
  return "";
}

void ParseBoard(const char* s, int* w, int* h) {
  if (sscanf(s, "%dx%d", w, h) != 2) {
    fprintf(stderr, "bad board size '%s' (use e.g. 5x5)\n", s);
    exit(1);
  }
}

const char* ValueName(int v) {
  switch (v) {
    case kWin: return "WIN";
    case kLoss: return "LOSS";
    case kDraw: return "DRAW";
    default: return "UNKNOWN";
  }
}

// ---------------------------------------------------------------------------

int CmdPerft(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  const int depth = atoi(argv[3]);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  Pos p = HasFlag(argc, argv, "--setup")
              ? ParsePos(g, GetArg(argc, argv, "--setup"))
              : InitialPos(g);
  const bool stepwise = HasFlag(argc, argv, "--stepwise");
  const bool nodup = HasFlag(argc, argv, "--nodup");
  PrintPos(g, p, stderr);
  for (int d = 1; d <= depth; d++) {
    const uint64_t n = Perft(g, p, d, stepwise, !nodup);
    printf("perft(%d) = %llu\n", d, (unsigned long long)n);
    fflush(stdout);
  }
  return 0;
}

// ---------------------------------------------------------------------------

int CmdIdexTest(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  Indexer ix(g, SymMode(argc, argv));

  uint64_t checked = 0;
  for (int lid = 0; lid < ix.num_layers(); lid++) {
    const Layer& l = ix.layer(lid);
    if (l.total > 2000000) continue;  // keep the test quick
    for (uint64_t idx = 0; idx < l.total; idx++) {
      uint64_t wf, bf;
      ix.BoardOf(l, idx, &wf, &bf);
      // round trip
      uint64_t idx2 = ix.IndexOf(l, wf, bf);
      if (idx2 != idx) {
        fprintf(stderr, "FAIL layer {%d,%d} idx %llu -> %llu\n", l.a, l.b,
                (unsigned long long)idx, (unsigned long long)idx2);
        return 1;
      }
      if (l.sym) {
        // Canonicalization must be idempotent and land on the same slot for
        // every member of the orbit.
        uint64_t cw = wf, cb = bf;
        ix.Canon(&cw, &cb);
        if (!ix.IsRep(cw, cb)) {
          fprintf(stderr, "FAIL {%d,%d}: canon not rep\n", l.a, l.b);
          return 1;
        }
        const uint64_t ridx = ix.IndexOf(l, cw, cb);
        uint64_t rw, rb;
        ix.BoardOf(l, ridx, &rw, &rb);
        if (rw != cw || rb != cb) {
          fprintf(stderr, "FAIL {%d,%d}: rep decode mismatch\n", l.a, l.b);
          return 1;
        }
        // All transformed images must canonicalize to the same index.
        for (int i = 1; i < ix.nsym(); i++) {
          const int tr = ix.sym_id(i);
          uint64_t tw = 0, tb = 0;
          for (int p = 0; p < g.N; p++) {
            if (wf >> p & 1) tw |= 1ull << g.perm[tr][p];
            if (bf >> p & 1) tb |= 1ull << g.perm[tr][p];
          }
          uint64_t iw = tw, ib = tb;
          ix.Canon(&iw, &ib);
          if (iw != cw || ib != cb) {
            fprintf(stderr, "FAIL {%d,%d}: orbit disagreement\n", l.a, l.b);
            return 1;
          }
        }
      }
      checked++;
    }
  }
  printf("idextest OK (%llu indexes checked)\n", (unsigned long long)checked);
  return 0;
}

// ---------------------------------------------------------------------------

int CmdBuild(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  const int max_stones =
      atoi(GetArg(argc, argv, "--max-stones", "999").c_str());
  const int threads = atoi(GetArg(argc, argv, "--threads", "64").c_str());
  std::string out = GetArg(argc, argv, "--out");
  if (out.empty()) {
    char buf[64];
    snprintf(buf, sizeof buf, "out/%dx%d%s", w, h, SymSuffix(argc, argv));
    out = buf;
  }
  Indexer ix(g, SymMode(argc, argv));
  RetroBuilder rb(g, ix, out, threads);
  if (!rb.BuildAll(max_stones)) {
    fprintf(stderr, "build failed\n");
    return 1;
  }
  // Report the initial position's value if its layer was built.
  Pos init = InitialPos(g);
  const int v = rb.Probe(init);
  printf("initial position: %s\n", ValueName(v));
  return 0;
}

// ---------------------------------------------------------------------------

ValueStore OpenStore(const Indexer& ix, const std::string& out) {
  ValueStore vs;
  for (int lid = 0; lid < ix.num_layers(); lid++) {
    const Layer& l = ix.layer(lid);
    char buf[128];
    snprintf(buf, sizeof buf, "%s/values/%d_%d.wdl", out.c_str(), l.a, l.b);
    vs.Register(lid, buf, l.total);  // silently skips missing
  }
  return vs;
}

int CmdQuery(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  Indexer ix(g, SymMode(argc, argv));
  const std::string out = GetArg(argc, argv, "--out", "out");
  ValueStore vs = OpenStore(ix, out);
  Pos p = ParsePos(g, GetArg(argc, argv, "--pos"));
  PrintPos(g, p, stderr);
  printf("%s\n", ValueName(ProbePosition(g, ix, vs, p)));
  return 0;
}

int CmdRoot(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  Indexer ix(g, SymMode(argc, argv));
  const std::string out = GetArg(argc, argv, "--out", "out");
  ValueStore vs = OpenStore(ix, out);
  Pos p = InitialPos(g);
  PrintPos(g, p, stderr);
  printf("initial position: %s\n", ValueName(ProbePosition(g, ix, vs, p)));
  return 0;
}

// ---------------------------------------------------------------------------
// Independent brute-force solver for small boards (value iteration over a
// hash map; no use of the indexing/retrograde code beyond movegen).
// ---------------------------------------------------------------------------

struct BFKey {
  uint64_t w, b;
  bool stm;
  bool operator==(const BFKey& o) const {
    return w == o.w && b == o.b && stm == o.stm;
  }
};
struct BFHash {
  size_t operator()(const BFKey& k) const {
    return HashPos(Pos{k.w, k.b, k.stm});
  }
};

int CmdVerify(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  if (g.N > 16) {
    fprintf(stderr, "verify: board too large for brute force\n");
    return 1;
  }
  Indexer ix(g, SymMode(argc, argv));
  const std::string out = GetArg(argc, argv, "--out", "out");
  ValueStore vs = OpenStore(ix, out);

  // Enumerate all positions.
  std::unordered_map<BFKey, uint8_t, BFHash> val;
  const int n = g.N;
  for (int wc = 0; wc + 0 <= n; wc++)
    for (int bc = 0; bc + wc <= n; bc++) {
      // iterate white sets then black sets
      // (simple recursive enumeration)
      std::vector<uint64_t> wsets;
      for (uint64_t wr = 0;; wr++) {
        // stop when unrank fails: use combinadic bound
        if (wr >= gBinom[n][wc]) break;
        wsets.push_back(UnrankSet(wr, wc, n));
      }
      for (uint64_t wset : wsets) {
        for (uint64_t br = 0; br < gBinom[n - wc][bc]; br++) {
          const uint64_t bset = UnrankRestricted(br, wset, bc, n);
          for (int stm = 0; stm < 2; stm++) {
            BFKey k{wset, bset, stm == 0};
            const int own = k.stm ? wc : bc;
            const int opp = k.stm ? bc : wc;
            uint8_t v = 0;  // unknown
            if (own == 0)
              v = kLoss;  // side to move has no stones
            else if (opp == 0)
              v = kWin;  // opponent has no stones
            val[k] = v;
          }
        }
      }
    }
  fprintf(stderr, "verify: %zu positions\n", val.size());

  // Value iteration.
  std::vector<Pos> children;
  bool changed = true;
  int pass = 0;
  while (changed) {
    changed = false;
    pass++;
    for (auto& [k, v] : val) {
      if (v != 0) continue;
      Pos p{k.w, k.b, k.stm};
      children.clear();
      GenMovesVec(g, p, &children, false);
      if (children.empty()) {
        v = kLoss;  // stalemate
        changed = true;
        continue;
      }
      bool any_loss = false, all_win = true;
      for (const Pos& c : children) {
        const uint8_t cv = val[BFKey{c.white, c.black, c.white_to_move}];
        if (cv == kLoss) any_loss = true;
        if (cv != kWin) all_win = false;
      }
      if (any_loss) {
        v = kWin;
        changed = true;
      } else if (all_win) {
        v = kLoss;
        changed = true;
      }
    }
    fprintf(stderr, "verify pass %d done\n", pass);
  }
  for (auto& [k, v] : val)
    if (v == 0) v = kDraw;

  // Compare against the tables.
  uint64_t mismatch = 0, checked = 0;
  for (const auto& [k, v] : val) {
    const int tv = ProbePosition(g, ix, vs, Pos{k.w, k.b, k.stm});
    if (tv == kUnknown) {
      fprintf(stderr, "verify: table missing layer for a position\n");
      return 1;
    }
    checked++;
    if (tv != v) {
      if (mismatch < 10) {
        Pos p{k.w, k.b, k.stm};
        PrintPos(g, p, stderr);
        fprintf(stderr, "  table=%s brute=%s\n", ValueName(tv), ValueName(v));
      }
      mismatch++;
    }
  }
  printf("verify: %llu positions compared, %llu mismatches\n",
         (unsigned long long)checked, (unsigned long long)mismatch);
  return mismatch ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Cross-check: compare every position with <= max_stones stones between two
// table sets (e.g. plain vs D4 build of the same board).
// ---------------------------------------------------------------------------

int CmdCrossCheck(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  const std::string out1 = GetArg(argc, argv, "--a");
  const std::string out2 = GetArg(argc, argv, "--b");
  const int max_stones = atoi(GetArg(argc, argv, "--max-stones", "6").c_str());
  Indexer ix1(g, 0);
  Indexer ix2(g, HasFlag(argc, argv, "--d2") ? 2 : 4);
  ValueStore vs1 = OpenStore(ix1, out1);
  ValueStore vs2 = OpenStore(ix2, out2);

  const int n = g.N;
  uint64_t checked = 0, mismatch = 0, skipped = 0;
  for (int wc = 0; wc + 0 <= n && wc <= max_stones; wc++)
    for (int bc = 0; bc + wc <= n && wc + bc <= max_stones; bc++) {
      for (uint64_t wr = 0; wr < gBinom[n][wc]; wr++) {
        const uint64_t wset = UnrankSet(wr, wc, n);
        for (uint64_t br = 0; br < gBinom[n - wc][bc]; br++) {
          const uint64_t bset = UnrankRestricted(br, wset, bc, n);
          for (int stm = 0; stm < 2; stm++) {
            const Pos p{wset, bset, stm == 0};
            const int v1 = ProbePosition(g, ix1, vs1, p);
            const int v2 = ProbePosition(g, ix2, vs2, p);
            if (v1 == kUnknown || v2 == kUnknown) {
              skipped++;
              continue;
            }
            checked++;
            if (v1 != v2) {
              if (mismatch < 5) {
                PrintPos(g, p, stderr);
                fprintf(stderr, "  plain=%s d4=%s\n", ValueName(v1),
                        ValueName(v2));
              }
              mismatch++;
            }
          }
        }
      }
    }
  printf("crosscheck: %llu compared, %llu mismatches, %llu skipped\n",
         (unsigned long long)checked, (unsigned long long)mismatch,
         (unsigned long long)skipped);
  return mismatch ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Reachable-position count from a given start (default: InitialPos) by BFS
// over the composite-move graph. Small boards only (hash-set based).
// ---------------------------------------------------------------------------

int CmdReach(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  if (g.N > 31) {
    fprintf(stderr, "reach: board too large for packed keys\n");
    return 1;
  }
  InitBinom();
  Pos start = HasFlag(argc, argv, "--pos")
                  ? ParsePos(g, GetArg(argc, argv, "--pos"))
                  : InitialPos(g);
  PrintPos(g, start, stderr);

  auto pack = [](const Pos& p) {
    return p.white | (p.black << 31) |
           (static_cast<uint64_t>(p.white_to_move) << 62);
  };
  std::unordered_set<uint64_t> seen;
  std::vector<Pos> frontier{start}, next;
  std::vector<Pos> children;
  seen.insert(pack(start));
  while (!frontier.empty()) {
    next.clear();
    for (const Pos& p : frontier) {
      children.clear();
      GenMovesVec(g, p, &children, false);
      for (const Pos& c : children)
        if (seen.insert(pack(c)).second) next.push_back(c);
    }
    frontier.swap(next);
    fprintf(stderr, "reach: depth done, %zu seen so far\n", seen.size());
  }
  printf("reachable positions: %zu\n", seen.size());
  return 0;
}

// ---------------------------------------------------------------------------

int CmdSolve(int argc, char** argv) {
  int w, h;
  ParseBoard(argv[2], &w, &h);
  Geom g;
  g.Init(w, h);
  g.seq_rule = atoi(GetArg(argc, argv, "--seq-rule", "0").c_str());
  InitBinom();
  const std::string db = GetArg(argc, argv, "--db");
  Indexer ix(g, SymMode(argc, argv));
  ValueStore vs;
  if (!db.empty()) vs = OpenStore(ix, db);

  BnsSolver solver(g, atoi(GetArg(argc, argv, "--tt-mb", "1024").c_str()));
  solver.set_arith_pn(HasFlag(argc, argv, "--pn"));
  solver.set_max_ply(atoi(GetArg(argc, argv, "--max-ply", "512").c_str()));
  const std::string lim = GetArg(argc, argv, "--nodes", "0");
  if (lim != "0") solver.set_nodes_limit(strtoull(lim.c_str(), nullptr, 10));
  const int db_stones = atoi(GetArg(argc, argv, "--db-stones", "99").c_str());
  auto probe = [&](const Pos& p) {
    if (p.OwnCount() + p.OppCount() > db_stones) return 0;
    return ProbePosition(g, ix, vs, p);
  };
  if (!db.empty()) solver.set_db_probe(probe);
  Pos root = HasFlag(argc, argv, "--pos")
                 ? ParsePos(g, GetArg(argc, argv, "--pos"))
                 : InitialPos(g);
  PrintPos(g, root, stderr);
  const int v1 = solver.Solve(root);
  fprintf(stderr, "nodes=%llu\n", (unsigned long long)solver.nodes());
  if (v1 > 0) {
    printf("side to move (%s) forces a WIN\n",
           root.white_to_move ? "White" : "Black");
    return 0;
  }
  if (v1 == 0) {
    printf("unresolved\n");
    return 0;
  }
  // Disproved: side to move cannot force a win. Solve for the opponent.
  Pos sw = root;
  sw.white_to_move = !sw.white_to_move;
  BnsSolver solver2(g, atoi(GetArg(argc, argv, "--tt-mb", "1024").c_str()));
  solver2.set_arith_pn(HasFlag(argc, argv, "--pn"));
  solver2.set_max_ply(atoi(GetArg(argc, argv, "--max-ply", "512").c_str()));
  if (lim != "0") solver2.set_nodes_limit(strtoull(lim.c_str(), nullptr, 10));
  if (!db.empty()) solver2.set_db_probe(probe);
  const int v2 = solver2.Solve(sw);
  fprintf(stderr, "nodes=%llu\n", (unsigned long long)solver2.nodes());
  if (v2 > 0)
    printf("side to move (%s) LOSES (opponent forces a win)\n",
           root.white_to_move ? "White" : "Black");
  else if (v2 == 0)
    printf("side to move cannot force a win; opponent unresolved\n");
  else
    printf("DRAW\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr,
            "usage: fanorona <perft|idextest|build|query|root|verify|crosscheck|"
            "reach|solve> <WxH> ...\n");
    return 1;
  }
  const std::string cmd = argv[1];
  try {
    if (cmd == "perft") return CmdPerft(argc, argv);
    if (cmd == "idextest") return CmdIdexTest(argc, argv);
    if (cmd == "build") return CmdBuild(argc, argv);
    if (cmd == "query") return CmdQuery(argc, argv);
    if (cmd == "root") return CmdRoot(argc, argv);
    if (cmd == "verify") return CmdVerify(argc, argv);
    if (cmd == "crosscheck") return CmdCrossCheck(argc, argv);
    if (cmd == "reach") return CmdReach(argc, argv);
    if (cmd == "solve") return CmdSolve(argc, argv);
  } catch (const std::exception& e) {
    fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  fprintf(stderr, "unknown command %s\n", argv[1]);
  return 1;
}
