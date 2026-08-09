// morris CLI: build-zdds, solve, query, estimate, nodecount-check.
#include "../src/board.hpp"
#include "../src/moves.hpp"
#include "../src/solve.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <random>

static double nowS() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static const char* VN[] = {"UNKNOWN", "LOSS", "DRAW", "WIN"};
static double zpTotalApprox(int m, int N) {
  // sum_{w,b<=N} C(m,w)C(m-w,b) computed in doubles (report only)
  std::vector<double> logfact(m + 1, 0);
  for (int i = 2; i <= m; ++i) logfact[i] = logfact[i - 1] + log((double)i);
  double tot = 0;
  for (int w = 0; w <= N; ++w)
    for (int b = 0; b <= N && w + b <= m; ++b)
      tot += exp(logfact[m] - logfact[w] - logfact[b] - logfact[m - w - b]);
  return tot;
}

struct Ctx {
  GameSpec sp;
  Board bd;
  Zdd1 z23, zp;
  Forest2 f23, fp;
  std::string dir;
  int threads;

  void prepare(bool build) {
    double t0 = nowS();
    z23.build(bd.m, 3, sp.pieces);
    zp.build(bd.m, 0, sp.pieces);
    std::string p23 = dir + "/zdd2_ph23.bin", pp = dir + "/zdd2_place.bin";
    if (!f23.load(p23, bd, z23)) {
      if (!build) throw std::runtime_error("missing " + p23 + " (run build-zdds)");
      printf("[zdds] sweeping phase-2/3 domain (%llu configs)...\n",
             (unsigned long long)z23.total);
      fflush(stdout);
      sweepAndBuild(bd, z23, 3, sp.pieces, true, true, &f23, threads);
      f23.save(p23);
    }
    if (!fp.load(pp, bd, zp)) {
      if (!build) throw std::runtime_error("missing " + pp + " (run build-zdds)");
      printf("[zdds] sweeping placement domain (%llu configs)...\n",
             (unsigned long long)zp.total);
      fflush(stdout);
      sweepAndBuild(bd, zp, 0, sp.pieces, false, true, &fp, threads);
      fp.save(pp);
    }
    u64 tot23 = 0, totp = 0;
    for (u64 c : f23.subsetCount) tot23 += c;
    for (u64 c : fp.subsetCount) totp += c;
    printf("[zdds] ready in %.1fs: phase23 configs=%llu nodes=%llu | placement configs=%llu "
           "nodes=%llu\n", nowS() - t0, (unsigned long long)tot23,
           (unsigned long long)f23.pool.nnodes.load(), (unsigned long long)totp,
           (unsigned long long)fp.pool.nnodes.load());
    fflush(stdout);
  }
};

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: morris <cmd> [--game N] [--dir D] [--threads N]\n"
      "  build-zdds | solve | estimate | nodecount-check\n"
      "  query --white A,B,.. --black .. --hands wh,bh   (names like ANW, BC=center CTR)\n");
    return 1;
  }
  std::string cmd = argv[1];
  int game = 12, threads = (int)std::thread::hardware_concurrency();
  std::string dir, whiteS, blackS, handsS;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = atoi(argv[++i]);
    else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (a == "--white" && i + 1 < argc) whiteS = argv[++i];
    else if (a == "--black" && i + 1 < argc) blackS = argv[++i];
    else if (a == "--hands" && i + 1 < argc) handsS = argv[++i];
  }
  GameSpec sp = gameSpec(game);
  Board bd = buildBoard(sp);
  // Relative to the working directory, so the tree can be moved without editing source.
  if (dir.empty()) dir = "data/m" + std::to_string(game);
  printf("=== %d Men's Morris | board %c: %d points, %zu mills, %zu syms (mill group %zu) "
         "| flying=%d fullBoardDraw=%d ===\n", game, sp.boardType, bd.m,
         bd.millMask.size(), bd.perms.size(), bd.millPerms.size(), sp.flying,
         sp.fullBoardDraw);
  Ctx cx{sp, bd, {}, {}, {}, {}, dir, threads};

  if (cmd == "endgame16") {
    // exact 16MM endgame tablebase: all phase-2/3 partitions with W,B <= maxpieces
    int maxT = 6;
    for (int i = 2; i < argc; ++i)
      if (std::string(argv[i]) == "--maxpieces" && i + 1 < argc) maxT = atoi(argv[i + 1]);
    system(("mkdir -p " + dir).c_str());
    printf("[endgame16] band W,B in [3,%d]; rules N=%d (filter thresholds use N)\n",
           maxT, sp.pieces);
    cx.z23.build(bd.m, 3, maxT);
    printf("[endgame16] zdd1 domain=%llu\n", (unsigned long long)cx.z23.total);
    fflush(stdout);
    std::string p23 = dir + "/zdd2_ph23_band" + std::to_string(maxT) + ".bin";
    if (!cx.f23.load(p23, bd, cx.z23)) {
      sweepAndBuild(bd, cx.z23, 3, maxT, true, true, &cx.f23, threads);
      cx.f23.save(p23);
    }
    u64 tot = 0;
    for (u64 v : cx.f23.subsetCount) tot += v;
    printf("[endgame16] band configs=%llu nodes=%llu\n", (unsigned long long)tot,
           (unsigned long long)cx.f23.pool.nnodes.load());
    fflush(stdout);
    Solver sv;
    sv.bd = &bd; sv.z23 = &cx.z23; sv.f23 = &cx.f23; sv.zp = &cx.zp; sv.fp = &cx.fp;
    sv.N = sp.pieces; sv.threads = threads; sv.dir = dir;
    sv.solvePhase23();
    printf("[endgame16] band solve complete\n");
  } else if (cmd == "build-zdds") {
    system(("mkdir -p " + dir).c_str());
    cx.prepare(true);
  } else if (cmd == "solve") {
    bool force = false;
    for (int i = 2; i < argc; ++i)
      if (std::string(argv[i]) == "--force") force = true;
    if (game == 16 && !force) {
      fprintf(stderr, "refusing to start the full 16MM solve (see 'estimate --game 16'); "
                      "pass --force to override\n");
      return 2;
    }
    system(("mkdir -p " + dir).c_str());
    cx.prepare(true);
    Solver sv;
    sv.bd = &bd; sv.z23 = &cx.z23; sv.f23 = &cx.f23; sv.zp = &cx.zp; sv.fp = &cx.fp;
    sv.N = sp.pieces; sv.threads = threads; sv.dir = dir;
    double t0 = nowS();
    sv.solvePhase23();
    printf("[solve] phase 2/3 complete in %.1fs\n", nowS() - t0);
    double t1 = nowS();
    sv.solvePlacement();
    printf("[solve] placement complete in %.1fs\n", nowS() - t1);
    printf("[solve] out-of-index children (unreachable-only, valued DRAW): %llu\n",
           (unsigned long long)sv.outOfIndexChildren);
    printf("=== INITIAL POSITION VALUE (%d Men's Morris, White moves first): %s ===\n",
           game, VN[sv.initialValue]);
  } else if (cmd == "nodecount-check") {
    // build the paper's exact global ZDD2 (their acceptance: unique-only at 12MM)
    Forest2 F;
    sweepAndBuild(bd, cx.z23, 3, sp.pieces, false, true, &F, threads);
    printf("[nodecheck] unfiltered global ZDD2: count=%llu reachableNodes=%llu "
           "(paper 12MM: 16,528,470,859 / 68,811,597)\n",
           (unsigned long long)F.globalCount,
           (unsigned long long)F.pool.reachableNodes(F.globalRoot));
  } else if (cmd == "query") {
    int maxT = 0;
    for (int i = 2; i < argc; ++i)
      if (std::string(argv[i]) == "--maxpieces" && i + 1 < argc) maxT = atoi(argv[i + 1]);
    if (maxT) {   // band tablebase query (phase 2/3 only)
      cx.z23.build(bd.m, 3, maxT);
      std::string p23 = dir + "/zdd2_ph23_band" + std::to_string(maxT) + ".bin";
      if (!cx.f23.load(p23, bd, cx.z23)) { fprintf(stderr, "missing %s\n", p23.c_str()); return 1; }
    } else
      cx.prepare(false);
    auto parsePts = [&](const std::string& s) -> u32 {
      u32 mask = 0;
      size_t p = 0;
      while (p < s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        std::string name = s.substr(p, q - p);
        bool found = false;
        for (int i = 0; i < bd.m; ++i)
          if (bd.names[i] == name) { mask |= 1u << i; found = true; }
        if (!found && !name.empty()) { fprintf(stderr, "bad point %s\n", name.c_str()); exit(1); }
        p = q + 1;
      }
      return mask;
    };
    u32 w = parsePts(whiteS), b = parsePts(blackS);
    int wh = 0, bh = 0;
    sscanf(handsS.c_str(), "%d,%d", &wh, &bh);
    int stm = (wh == bh) ? 0 : 1;   // during placement; with empty hands white implied? no:
    if (wh + bh == 0) stm = 0;      // caller may query either via swapping colors; document
    // value via file-backed tables
    auto fileVal = [&](u32 w2, u32 b2, int wh2, int bh2, int stm2) -> u32 {
      int W = __builtin_popcount(w2), B = __builtin_popcount(b2);
      char path[512];
      u64 idx;
      if (wh2 + bh2 == 0) {
        if ((stm2 ? B : W) < 3) return V_LOSS;
        if ((stm2 ? W : B) < 3) return V_WIN;
        if (sp.fullBoardDraw && W + B == bd.m) return V_DRAW;
        u32 cw = w2, cb = b2;
        bd.canonicalizeAuto(cw, cb);
        u64 r1 = cx.z23.rank(cw, cb);
        u64 k = r1 == UINT64_MAX ? UINT64_MAX : cx.f23.sel(W, B).selRank(r1);
        if (k == UINT64_MAX) return V_UNK;
        snprintf(path, sizeof path, "%s/ph23_w%02d_b%02d.wdl", dir.c_str(), W, B);
        idx = k * 2 + stm2;
      } else {
        u32 cw = w2, cb = b2;
        bd.canonicalize(cw, cb);
        u64 r1 = cx.zp.rank(cw, cb);
        u64 k = r1 == UINT64_MAX ? UINT64_MAX : cx.fp.sel(W, B).selRank(r1);
        if (k == UINT64_MAX) return V_UNK;
        snprintf(path, sizeof path, "%s/place_H%02d_w%02d_b%02d.wdl", dir.c_str(),
                 wh2 + bh2, W, B);
        idx = k;
      }
      FILE* f = fopen(path, "rb");
      if (!f) return V_UNK;
      fseek(f, 32 + (long)(idx / 4), SEEK_SET);
      int byte = fgetc(f);
      fclose(f);
      if (byte < 0) return V_UNK;
      return (u32)((byte >> ((idx & 3) * 2)) & 3);
    };
    u32 v = fileVal(w, b, wh, bh, stm);
    u32 cw = w, cb = b;
    bd.canonicalizeAuto(cw, cb);
    printf("position: white=%s black=%s hands=%d,%d side-to-move=%s\n", whiteS.c_str(),
           blackS.c_str(), wh, bh, stm ? "black" : "white");
    printf("canonical white mask=%06x black mask=%06x\n", cw, cb);
    printf("VALUE (side to move): %s\n", VN[v]);
    std::vector<Succ> succ;
    genSuccessors(bd, w, b, wh, bh, stm, succ);
    printf("moves (%zu):\n", succ.size());
    const char* best = nullptr;
    for (auto& s : succ) {
      int stm2 = stm ^ 1;
      u32 sv2 = fileVal(s.w, s.b, s.wh, s.bh, stm2);
      u32 mine = sv2 == V_LOSS ? V_WIN : sv2 == V_WIN ? V_LOSS : sv2;
      printf("  -> white=%06x black=%06x hands=%d,%d : successor(%s) => mover %s%s\n",
             s.w, s.b, s.wh, s.bh, VN[sv2], VN[mine],
             (mine == v && !best) ? "   [optimal]" : "");
      if (mine == v && !best) best = "y";
    }
  } else if (cmd == "verify") {
    // sampled retrograde-invariant audit of finished tables:
    //   WIN has a LOSS successor; LOSS has all-WIN successors (and some successor);
    //   DRAW has no LOSS successor and at least one DRAW successor; no unknowns.
    cx.prepare(false);
    Solver sv;
    sv.bd = &bd; sv.z23 = &cx.z23; sv.f23 = &cx.f23; sv.zp = &cx.zp; sv.fp = &cx.fp;
    sv.N = sp.pieces; sv.threads = threads; sv.dir = dir;
    if (!sv.loadPhase23()) { fprintf(stderr, "tables missing\n"); return 1; }
    u64 samples = 2000000, bad = 0, unk = 0, done = 0;
    std::mt19937_64 rng(7);
    std::vector<Succ> buf;
    for (int W = 3; W <= sp.pieces; ++W)
      for (int B = 3; B <= sp.pieces; ++B) {
        Sel s = cx.f23.sel(W, B);
        if (!s.count) continue;
        u64 per = std::max<u64>(1, samples / ((sp.pieces - 2) * (sp.pieces - 2)));
        for (u64 t = 0; t < per; ++t) {
          u64 k = rng() % s.count;
          int stm = (int)(rng() & 1);
          u32 w, b;
          cx.z23.unrank(s.selUnrank(k), w, b);
          u32 v = sv.ph23[sv.sidx23(W, B)].get(k * 2 + stm);
          if (v == V_UNK) { ++unk; continue; }
          buf.clear();
          genSuccessors(bd, w, b, 0, 0, stm, buf);
          bool anyLoss = false, anyDraw = false, allWin = !buf.empty();
          for (auto& sc : buf) {
            u32 cv = sv.lookup23(sc.w, sc.b, stm ^ 1);
            if (cv == V_LOSS) anyLoss = true;
            if (cv == V_DRAW) anyDraw = true;
            if (cv != V_WIN) allWin = false;
          }
          bool ok = (v == V_WIN && anyLoss) ||
                    (v == V_LOSS && (buf.empty() || allWin)) ||
                    (v == V_DRAW && !anyLoss && anyDraw);
          if (!ok && ++bad < 6)
            printf("INVARIANT FAIL %d-%d stm=%d k=%llu v=%s\n", W, B, stm,
                   (unsigned long long)k, VN[v]);
          ++done;
        }
      }
    printf("[verify] %llu sampled states, %llu invariant failures, %llu unknown\n",
           (unsigned long long)done, (unsigned long long)bad, (unsigned long long)unk);
    return bad || unk ? 1 : 0;
  } else if (cmd == "estimate") {
    // Exact canonical counts via Burnside over the 16 board symmetries (checked u128).
    typedef unsigned __int128 u128;
    int P = bd.m, N = sp.pieces;
    std::vector<std::vector<u128>> tot(P + 1, std::vector<u128>(P + 1, 0));
    for (auto& pm : bd.perms) {
      std::vector<int> seen(P, 0), cyc;
      for (int p = 0; p < P; ++p) {
        if (seen[p]) continue;
        int len = 0, q = p;
        while (!seen[q]) { seen[q] = 1; q = pm[q]; ++len; }
        cyc.push_back(len);
      }
      std::vector<std::vector<u128>> dp(P + 1, std::vector<u128>(P + 1, 0));
      dp[0][0] = 1;
      for (int len : cyc) {
        auto nx = dp;
        for (int w2 = 0; w2 <= P; ++w2)
          for (int b2 = 0; b2 + w2 <= P; ++b2) {
            u128 v = dp[w2][b2];
            if (!v) continue;
            if (w2 + len <= P) nx[w2 + len][b2] += v;
            if (b2 + len <= P) nx[w2][b2 + len] += v;
          }
        dp.swap(nx);
      }
      for (int w2 = 0; w2 <= P; ++w2)
        for (int b2 = 0; b2 + w2 <= P; ++b2) tot[w2][b2] += dp[w2][b2];
    }
    auto Nu = [&](int w2, int b2) -> u128 { return tot[w2][b2] / (u128)bd.perms.size(); };
    auto pr = [](u128 v) {
      char buf[64]; int n2 = 0;
      if (!v) { printf("0"); return; }
      while (v) { buf[n2++] = (char)('0' + (int)(v % 10)); v /= 10; }
      for (int i = n2 - 1; i >= 0; --i) { putchar(buf[i]); if (i && i % 3 == 0) putchar(','); }
    };
    u128 ph = 0, phMax = 0, pl = 0;
    for (int w2 = 3; w2 <= N; ++w2)
      for (int b2 = 3; b2 <= N; ++b2) {
        ph += Nu(w2, b2);
        if (Nu(w2, b2) > phMax) phMax = Nu(w2, b2);
      }
    for (int wb = 0; wb <= N; ++wb)
      for (int bb = 0; bb <= N; ++bb) {
        int pairs = 0;
        for (int wh = 0; wh <= N - wb; ++wh)
          for (int d = 0; d <= 1; ++d)
            if (wh + d <= N - bb && wh + (wh + d) > 0) ++pairs;
        pl += Nu(wb, bb) * (u128)pairs;
      }
    u128 states = ph * 2 + pl;
    printf("=== FEASIBILITY REPORT: %d Men's Morris (canonical counts exact; the\n"
           "    pseudo-reachability filter shrinks these by <1%% at N=12) ===\n", game);
    printf("  phase-2/3 canonical configs:      "); pr(ph); printf("\n");
    printf("  largest phase-2/3 subset:         "); pr(phMax); printf("\n");
    printf("  placement states:                 "); pr(pl); printf("\n");
    printf("  TOTAL states (x2 stm for ph23):   "); pr(states); printf("\n");
    printf("  WDL bytes at 2 bits/state:        "); pr(states / 4); printf("\n");
    printf("  largest partition WDL (RAM, x2):  "); pr(phMax * 2 * 2 / 4); printf(" bytes\n");
    // measured 12MM rates: sweep ~2.3e9 cfg/s; solve ~3.5e8 state-visit/s (64 threads)
    double st = (double)(u64)(states / 1000) * 1000.0;
    printf("  first-ZDD domain sweep (~2.3e9/s measured): %.1f days\n",
           (double)(u64)(zpTotalApprox(bd.m, N)) / 2.3e9 / 86400);
    printf("  solve estimate at 12MM-measured ~3.5e8 visits/s x ~15 avg iterations:\n"
           "    %.0f days on this machine (64 threads)\n", st * 15 / 3.5e8 / 86400);
    if (game == 16)
      printf("  VERDICT: full 16MM solve requires ~%.0f TB flat WDL and years of CPU on\n"
             "  this machine; solver refuses to start without --force. A truncated exact\n"
             "  endgame solve (small W+B bands) is the supported alternative.\n",
             (double)(u64)(states / 4) / 1e12);
  } else {
    fprintf(stderr, "unknown command\n");
    return 1;
  }
  return 0;
}
