// solver.cpp -- Optimized strong solver for two-player Chinese Checkers on
// the m x m diamond board (Sturtevant, ACG 2019).
//
// State storage: 2 bits per canonical state. Canonical = side to move is
// always "P1" (top player, moving down); P2-to-move states map through the
// 180-degree rotation + color swap. When the start area is mirror-symmetric
// (p in {1,3,6}) left-right symmetry additionally drops ~half of the P1
// placement blocks (self-symmetric blocks keep both mirror images).
//
// Solving: static classification pass (illegal / terminal loss), then
// cyclic retrograde value-iteration passes to a fixpoint, with immediate
// back-propagation of wins to predecessors when a loss is proven.
// Remaining unknown states are draws.
//
// Passes iterate "b-major" (grouped by the non-mover's placement): every
// successor of every state in a group lands in one fixed a-block (the
// mover's move changes only `a`, and the canonical successor block is
// determined by rot(b)), so successor lookups stay in an L2-resident
// ~C(N-p,p)/4-byte window instead of missing to DRAM.
#include "cc_core.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <functional>
#include <mutex>
#include <thread>

using namespace cc;
using Clock = std::chrono::steady_clock;

static double secondsSince(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Options {
  int m = 6, p = 6;
  int threads = (int)std::thread::hardware_concurrency();
  bool useLR = true;
  bool backprop = true;
  bool nullMoves = false;   // rule variant: hop-cycle pass moves
  int areaMode = 0;         // 0 = first-k cells, 1 = symmetric shapes
  std::string dir;          // run directory (checkpoints, logs)
  double ckptMinutes = 60;  // mid-pass checkpoint interval (0 = off)
  bool resume = false;
  int order = 2;            // 0 = ascending groups, 1 = descending, 2 = alternate
  bool validate = true;     // compare against paper's Table 1 if known
};

struct Solver {
  Options opt;
  Game g;
  Index ix;
  Table table;
  std::vector<std::atomic<u32>> groupUnknown;  // per b-group unknown count
  std::atomic<u64> passWins{0}, passLosses{0}, passBackpropWins{0};
  std::atomic<u64> stuckLosses{0};
  std::atomic<u32> nextGroup{0};
  std::atomic<u64> groupsDone{0};
  std::atomic<bool> stopRequested{false}, ckptRequested{false};
  std::atomic<bool> abortPass{false};
  int curPass = 0;
  Clock::time_point t0 = Clock::now();
  Clock::time_point lastCkptTime = Clock::now();

  // Checkpoint coordination: workers park at group boundaries.
  std::mutex ckptMu;
  std::condition_variable ckptCv;
  int parked = 0, activeWorkers = 0;

  Solver(const Options& o)
      : opt(o), g(o.m, o.p, (Game::AreaMode)o.areaMode), ix(g, o.useLR),
        groupUnknown(ix.C1) {
    for (auto& c : groupUnknown) c.store(UINT32_MAX, std::memory_order_relaxed);
    table.alloc(ix.totalStored);
  }

  std::string tablePath() const { return opt.dir + "/table.bin"; }
  std::string metaPath() const { return opt.dir + "/meta.txt"; }

  void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stdout, "[%9.1fs] %s\n", secondsSince(t0), buf);
    fflush(stdout);
    FILE* f = fopen((opt.dir + "/log.txt").c_str(), "a");
    if (f) {
      fprintf(f, "[%9.1fs] %s\n", secondsSince(t0), buf);
      fclose(f);
    }
  }

  // ------------------------------------------------------------------
  // Phase 1: static classification (block-major: sequential writes).
  // ------------------------------------------------------------------
  struct InitCounts { u64 ill = 0, loss = 0, unk = 0; };
  InitCounts initCounts;

  void initPhase() {
    std::atomic<u64> nIll{0}, nLoss{0};
    std::atomic<u32> next{0};
    u32 nb = (u32)ix.storedBlocks.size();
    auto worker = [&]() {
      u64 myIll = 0, myLoss = 0;
      while (true) {
        u32 blk = next.fetch_add(1, std::memory_order_relaxed);
        if (blk >= nb) break;
        u32 r1 = ix.storedBlocks[blk];
        u64 a = ix.rk.unrankK(r1, g.p, g.N);
        int comp[49], nc = 0;
        for (int c = 0; c < g.N; c++)
          if (!(a >> c & 1)) comp[nc++] = c;
        i64 idx = ix.base[r1];
        // enumerate b = p-subsets of comp[] in colex order
        int id_[8];
        u64 bm[8];
        forEachColex(nc, [&](const int* sel) {
          u64 b = 0;
          for (int i = 0; i < g.p; i++) b |= bit(comp[sel[i]]);
          u8 v = g.classify(a, b);
          if (v != UNKNOWN) {
            table.setIfUnknown(idx, v);
            if (v == ILLEGAL) myIll++;
            else myLoss++;
          }
          idx++;
        });
        (void)id_;
        (void)bm;
      }
      nIll += myIll;
      nLoss += myLoss;
    };
    runWorkers(worker);
    initCounts.ill = nIll;
    initCounts.loss = nLoss;
    initCounts.unk = ix.totalStored - nIll - nLoss;
    log("init done: illegal=%llu terminal_loss=%llu unknown=%llu",
        (unsigned long long)initCounts.ill, (unsigned long long)initCounts.loss,
        (unsigned long long)initCounts.unk);
  }

  // Enumerate p-subsets sel[0..p-1] (ascending indices into 0..n-1) in
  // colex order.
  template <typename F>
  void forEachColex(int n, F f) {
    int p = g.p;
    int sel[8];
    for (int i = 0; i < p; i++) sel[i] = i;
    while (true) {
      f(sel);
      int i = 0;
      while (i < p) {
        sel[i]++;
        int lim = (i + 1 < p) ? sel[i + 1] : n;
        if (sel[i] < lim) break;
        i++;
      }
      if (i == p) break;
      for (int j = 0; j < i; j++) sel[j] = j;
    }
  }

  void runWorkers(const std::function<void()>& fn) {
    std::vector<std::thread> ts;
    for (int i = 0; i < opt.threads; i++) ts.emplace_back(fn);
    for (auto& t : ts) t.join();
  }

  // ------------------------------------------------------------------
  // Phase 2: retrograde passes (b-group major).
  // ------------------------------------------------------------------

  // Process one b-group; returns unknown states remaining in the group.
  u32 processGroup(u32 bRank, u64& wins, u64& losses, u64& bpWins, u64& stuck) {
    const u64 b = ix.rk.unrankK(bRank, g.p, g.N);
    const u64 rotb = g.rotMask(b);
    // Successor block: canonical index of (rot(b), rot(a')). Determine the
    // branch (direct or mirrored) once for the whole group.
    u32 succR1 = ix.rk.rankK(rotb);
    bool succMirrored = (ix.base[succR1] < 0);
    const u64 succA = succMirrored ? g.mirrorMask(rotb) : rotb;
    const i64 succBase = succMirrored ? ix.base[ix.mirR1[succR1]] : ix.base[succR1];

    int comp[49], nc = 0;
    for (int c = 0; c < g.N; c++)
      if (!(b >> c & 1)) comp[nc++] = c;

    u32 unknownLeft = 0;
    i64 succIdxBuf[512];
    int nSucc;

    int sel[8];
    for (int i = 0; i < g.p; i++) sel[i] = i;
    while (true) {
      // --- state (a, b), P1 (=a) to move ---
      u64 a = 0;
      for (int i = 0; i < g.p; i++) a |= bit(comp[sel[i]]);
      u32 r1a = ix.rk.rankK(a);
      i64 selfBase = ix.base[r1a];
      if (selfBase >= 0) {  // canonical a-block only (LR reduction)
        i64 selfIdx = selfBase + ix.rk.rank2(a, b);
        if (table.get(selfIdx) == UNKNOWN) {
          u64 occ = a | b;
          bool anyLoss = false, allWin = true;
          nSucc = 0;
          u64 am = a;
          bool hasPass = false;
          while (am) {
            int from = __builtin_ctzll(am);
            am &= am - 1;
            u64 dst;
            if (opt.nullMoves) {
              bool canPass;
              dst = pieceDestsNull(from, occ, canPass);
              hasPass |= canPass;
            } else {
              dst = g.pieceDests(from, occ);
            }
            while (dst) {
              int to = __builtin_ctzll(dst);
              dst &= dst - 1;
              u64 a2 = (a ^ bit(from)) | bit(to);
              u64 ra2 = g.rotMask(a2);
              i64 si = succBase +
                       (succMirrored ? ix.rk.rank2(succA, g.mirrorMask(ra2))
                                     : ix.rk.rank2(succA, ra2));
              succIdxBuf[nSucc++] = si;
              __builtin_prefetch(&table.words[si >> 5]);
            }
          }
          if (hasPass) {
            // pass successor: same board, other side to move
            succIdxBuf[nSucc++] = ix.indexOfP2ToMove(a, b);
          }
          int legal = 0;
          for (int i = 0; i < nSucc; i++) {
            u8 v = table.get(succIdxBuf[i]);
            if (v == ILLEGAL) continue;
            legal++;
            if (v == LOSS) { anyLoss = true; break; }
            if (v != WIN) allWin = false;
          }
          if (anyLoss) {
            if (table.setIfUnknown(selfIdx, WIN)) wins++;
          } else if (allWin) {
            if (legal == 0) stuck++;
            if (table.setIfUnknown(selfIdx, LOSS)) {
              losses++;
              if (opt.backprop) bpWins += backpropagate(a, b, occ);
            }
          } else {
            unknownLeft++;
          }
        }
      }
      // --- next a (colex) ---
      int i = 0;
      while (i < g.p) {
        sel[i]++;
        int lim = (i + 1 < g.p) ? sel[i + 1] : nc;
        if (sel[i] < lim) break;
        i++;
      }
      if (i == g.p) break;
      for (int j = 0; j < i; j++) sel[j] = j;
    }
    return unknownLeft;
  }

  // pieceDests + "can the piece hop back to its origin" (null-move rule).
  u64 pieceDestsNull(int from, u64 occ, bool& canPass) const {
    u64 occ2 = occ & ~bit(from);
    u64 dests = 0;
    canPass = false;
    for (int d = 0; d < 6; d++) {
      int n = g.nbr[from][d];
      if (n >= 0 && !(occ2 >> n & 1)) dests |= bit(n);
    }
    u64 visited = 0;  // origin NOT pre-marked: chains may land back on it
    int stack[64];
    int sp = 0;
    stack[sp++] = from;
    bool originSeeded = true;
    while (sp) {
      int c = stack[--sp];
      for (int d = 0; d < 6; d++) {
        int over = g.nbr[c][d];
        if (over < 0 || !(occ2 >> over & 1)) continue;
        int land = g.hopLand[c][d];
        if (land < 0 || (occ2 >> land & 1) || (visited >> land & 1)) continue;
        visited |= bit(land);
        if (land == from) canPass = true;
        else dests |= bit(land);
        stack[sp++] = land;
      }
    }
    (void)originSeeded;
    return dests;
  }

  // s = (a,b) was just proven LOSS (P1 to move). Every predecessor (P2 to
  // move, one b-piece moved) gains a losing successor -> WIN. The move
  // relation is symmetric, so predecessors = b-piece moves on this board.
  u64 backpropagate(u64 a, u64 b, u64 occ) {
    u64 n = 0;
    u64 bm2 = b;
    while (bm2) {
      int v = __builtin_ctzll(bm2);
      bm2 &= bm2 - 1;
      u64 dst;
      bool canPass = false;
      if (opt.nullMoves) dst = pieceDestsNull(v, occ, canPass);
      else dst = g.pieceDests(v, occ);
      while (dst) {
        int u = __builtin_ctzll(dst);
        dst &= dst - 1;
        u64 b2 = (b ^ bit(v)) | bit(u);
        i64 pi = ix.indexOfP2ToMove(a, b2);
        if (table.setIfUnknown(pi, WIN)) n++;
      }
      if (canPass) {
        i64 pi = ix.indexOfP2ToMove(a, b);
        if (table.setIfUnknown(pi, WIN)) n++;
      }
    }
    return n;
  }

  // One full pass; returns number of value changes.
  u64 runPass(int pass) {
    passWins = passLosses = passBackpropWins = 0;
    nextGroup = 0;
    groupsDone = 0;
    bool descending = (opt.order == 1) || (opt.order == 2 && (pass & 1));
    u32 nGroups = ix.C1;

    auto worker = [&]() {
      u64 wins = 0, losses = 0, bp = 0, stuck = 0;
      while (!abortPass.load(std::memory_order_relaxed)) {
        maybePark();
        u32 gi = nextGroup.fetch_add(1, std::memory_order_relaxed);
        if (gi >= nGroups) break;
        u32 bRank = descending ? (nGroups - 1 - gi) : gi;
        if (groupUnknown[bRank].load(std::memory_order_relaxed) == 0) {
          groupsDone.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        u32 left = processGroup(bRank, wins, losses, bp, stuck);
        groupUnknown[bRank].store(left, std::memory_order_relaxed);
        groupsDone.fetch_add(1, std::memory_order_relaxed);
      }
      passWins += wins;
      passLosses += losses;
      passBackpropWins += bp;
      stuckLosses += stuck;
      {
        std::lock_guard<std::mutex> lk(ckptMu);
        activeWorkers--;
        ckptCv.notify_all();
      }
    };

    {
      std::lock_guard<std::mutex> lk(ckptMu);
      activeWorkers = opt.threads;
    }
    std::vector<std::thread> ts;
    for (int i = 0; i < opt.threads; i++) ts.emplace_back(worker);

    // progress / checkpoint monitor
    std::atomic<bool> done{false};
    std::thread mon([&]() {
      auto lastLog = Clock::now();
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (secondsSince(lastLog) > 30) {
          lastLog = Clock::now();
          log("  pass %d progress: groups %llu/%u, wins=%llu losses=%llu "
              "bpwins=%llu",
              pass, (unsigned long long)groupsDone.load(), nGroups,
              (unsigned long long)passWins.load(),
              (unsigned long long)passLosses.load(),
              (unsigned long long)passBackpropWins.load());
        }
        bool wantCkpt =
            (opt.ckptMinutes > 0 &&
             secondsSince(lastCkptTime) > opt.ckptMinutes * 60) ||
            stopRequested.load();
        if (wantCkpt && !done.load()) {
          doMidPassCheckpoint(pass);
          if (stopRequested.load()) return;  // abortPass set; workers exiting
        }
      }
    });

    for (auto& t : ts) t.join();
    done = true;
    mon.join();
    abortPass = false;
    u64 changes = passWins + passLosses + passBackpropWins;
    log("pass %d done (%s): wins=%llu losses=%llu bpwins=%llu changes=%llu",
        pass, descending ? "desc" : "asc", (unsigned long long)passWins.load(),
        (unsigned long long)passLosses.load(),
        (unsigned long long)passBackpropWins.load(),
        (unsigned long long)changes);
    return changes;
  }

  // Worker-side pause point (group granularity).
  void maybePark() {
    if (!ckptRequested.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(ckptMu);
    parked++;
    ckptCv.notify_all();
    ckptCv.wait(lk, [&] { return !ckptRequested.load(); });
    parked--;
  }

  void doMidPassCheckpoint(int pass) {
    {
      std::unique_lock<std::mutex> lk(ckptMu);
      ckptRequested = true;
      ckptCv.notify_all();
      // Wait until every still-running worker is parked (workers that
      // finished the queue decrement activeWorkers on exit).
      ckptCv.wait(lk, [&] { return parked >= activeWorkers; });
    }
    writeCheckpoint(pass, /*midPass=*/true);
    if (stopRequested.load()) abortPass = true;  // released workers will exit
    {
      std::lock_guard<std::mutex> lk(ckptMu);
      ckptRequested = false;
      ckptCv.notify_all();
    }
  }

  void writeCheckpoint(int pass, bool midPass) {
    log("checkpoint: writing table (%0.1f GB)...",
        table.nWords * 8.0 / 1e9);
    if (!table.saveTo(tablePath())) {
      log("checkpoint FAILED");
      return;
    }
    // group counters
    {
      std::string p2 = opt.dir + "/groups.bin.tmp";
      FILE* f = fopen(p2.c_str(), "wb");
      if (f) {
        std::vector<u32> tmp(ix.C1);
        for (u32 i = 0; i < ix.C1; i++) tmp[i] = groupUnknown[i].load();
        fwrite(tmp.data(), 4, ix.C1, f);
        fclose(f);
        rename(p2.c_str(), (opt.dir + "/groups.bin").c_str());
      }
    }
    FILE* f = fopen((metaPath() + ".tmp").c_str(), "w");
    if (f) {
      fprintf(f, "m %d\np %d\nlr %d\nnull %d\nphase solve\npass %d\nmid %d\n",
              g.m, g.p, ix.lr ? 1 : 0, opt.nullMoves ? 1 : 0, pass,
              midPass ? 1 : 0);
      fclose(f);
      rename((metaPath() + ".tmp").c_str(), metaPath().c_str());
    }
    log("checkpoint written (pass %d%s)", pass, midPass ? ", mid-pass" : "");
    lastCkptTime = Clock::now();
  }

  // ------------------------------------------------------------------
  // Final tally (block-major, weighted for LR symmetry).
  // ------------------------------------------------------------------
  struct Totals { u64 W = 0, L = 0, D = 0, I = 0; };
  Totals tally() {
    std::atomic<u64> W{0}, L{0}, D{0}, I{0};
    std::atomic<u32> next{0};
    u32 nb = (u32)ix.storedBlocks.size();
    auto worker = [&]() {
      u64 w = 0, l = 0, d = 0, il = 0;
      while (true) {
        u32 blk = next.fetch_add(1, std::memory_order_relaxed);
        if (blk >= nb) break;
        u32 r1 = ix.storedBlocks[blk];
        int wt = ix.blockWeight(r1);
        i64 idx = ix.base[r1];
        for (u32 r2 = 0; r2 < ix.C2; r2++, idx++) {
          switch (table.get(idx)) {
            case WIN: w += wt; break;
            case LOSS: l += wt; break;
            case UNKNOWN: d += wt; break;
            case ILLEGAL: il += wt; break;
          }
        }
      }
      W += w; L += l; D += d; I += il;
    };
    runWorkers(worker);
    return {W.load(), L.load(), D.load(), I.load()};
  }

  int run() {
    log("game %dx%d p=%d: C1=%u C2=%u storedStates=%llu (%.2f GB) lr=%d "
        "threads=%d null=%d",
        g.m, g.m, g.p, ix.C1, ix.C2, (unsigned long long)ix.totalStored,
        ix.totalStored / 4.0 / 1e9, ix.lr ? 1 : 0, opt.threads,
        opt.nullMoves ? 1 : 0);

    int startPass = 1;
    if (opt.resume) {
      // parse meta
      FILE* f = fopen(metaPath().c_str(), "r");
      if (!f) { log("resume requested but no meta at %s", metaPath().c_str()); return 1; }
      char key[64];
      int vm = 0, vp = 0, vlr = 0, vnull = 0, vpass = 0, vmid = 0;
      char phase[32] = "";
      while (fscanf(f, "%63s", key) == 1) {
        if (!strcmp(key, "m")) { if (fscanf(f, "%d", &vm) != 1) break; }
        else if (!strcmp(key, "p")) { if (fscanf(f, "%d", &vp) != 1) break; }
        else if (!strcmp(key, "lr")) { if (fscanf(f, "%d", &vlr) != 1) break; }
        else if (!strcmp(key, "null")) { if (fscanf(f, "%d", &vnull) != 1) break; }
        else if (!strcmp(key, "phase")) { if (fscanf(f, "%31s", phase) != 1) break; }
        else if (!strcmp(key, "pass")) { if (fscanf(f, "%d", &vpass) != 1) break; }
        else if (!strcmp(key, "mid")) { if (fscanf(f, "%d", &vmid) != 1) break; }
      }
      fclose(f);
      if (vm != g.m || vp != g.p || vlr != (ix.lr ? 1 : 0) ||
          vnull != (opt.nullMoves ? 1 : 0)) {
        log("resume config mismatch (meta m=%d p=%d lr=%d null=%d)", vm, vp,
            vlr, vnull);
        return 1;
      }
      if (!table.loadFrom(tablePath())) { log("failed to load table"); return 1; }
      FILE* gf = fopen((opt.dir + "/groups.bin").c_str(), "rb");
      if (gf) {
        std::vector<u32> tmp(ix.C1);
        if (fread(tmp.data(), 4, ix.C1, gf) == ix.C1)
          for (u32 i = 0; i < ix.C1; i++) groupUnknown[i].store(tmp[i]);
        fclose(gf);
      }
      startPass = vmid ? vpass : vpass + 1;
      log("resumed at pass %d (mid=%d)", startPass, vmid);
    } else {
      initPhase();
      writeCheckpoint(0, false);
    }

    for (int pass = startPass;; pass++) {
      curPass = pass;
      u64 changes = runPass(pass);
      if (stopRequested.load()) {
        log("stopping on request after pass %d", pass);
        return 3;
      }
      if (changes == 0 || opt.ckptMinutes <= 0 ||
          secondsSince(lastCkptTime) > opt.ckptMinutes * 60)
        writeCheckpoint(pass, false);
      if (changes == 0) break;
    }

    Totals t = tally();
    u64 canon = t.W + t.L + t.D + t.I;
    u64 positions = 2 * canon;
    u64 paperWins = t.W + t.L;   // states where the FIRST player wins
    u64 paperDraws = 2 * t.D;
    u64 paperIll = 2 * t.I;
    log("FINAL %dx%d p=%d:", g.m, g.m, g.p);
    log("  canonical (P1-to-move, weighted): win=%llu loss=%llu draw=%llu "
        "illegal=%llu stuck_losses=%llu",
        (unsigned long long)t.W, (unsigned long long)t.L,
        (unsigned long long)t.D, (unsigned long long)t.I,
        (unsigned long long)stuckLosses.load());
    log("  paper-style: positions=%llu wins=%llu draws=%llu illegal=%llu",
        (unsigned long long)positions, (unsigned long long)paperWins,
        (unsigned long long)paperDraws, (unsigned long long)paperIll);
    // append to meta
    FILE* f = fopen(metaPath().c_str(), "w");
    if (f) {
      fprintf(f,
              "m %d\np %d\nlr %d\nnull %d\nphase done\npass %d\nmid 0\n"
              "wins %llu\ndraws %llu\nillegal %llu\npositions %llu\n",
              g.m, g.p, ix.lr ? 1 : 0, opt.nullMoves ? 1 : 0, curPass,
              (unsigned long long)paperWins, (unsigned long long)paperDraws,
              (unsigned long long)paperIll, (unsigned long long)positions);
      fclose(f);
    }
    if (opt.validate) {
      const KnownResult* k = knownResult(g.m, g.p);
      if (k) {
        bool ok = k->positions == positions && k->wins == paperWins &&
                  k->draws == paperDraws && k->illegal == paperIll;
        log("  TABLE-1 CHECK: %s (expected wins=%llu draws=%llu illegal=%llu)",
            ok ? "MATCH" : "MISMATCH", (unsigned long long)k->wins,
            (unsigned long long)k->draws, (unsigned long long)k->illegal);
        return ok ? 0 : 2;
      }
    }
    return 0;
  }
};

static Solver* gSolver = nullptr;
static void onSignal(int) {
  if (gSolver) gSolver->stopRequested = true;
}

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; i++) {
    std::string s = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (s == "--m") opt.m = atoi(next());
    else if (s == "--p") opt.p = atoi(next());
    else if (s == "--threads") opt.threads = atoi(next());
    else if (s == "--dir") opt.dir = next();
    else if (s == "--no-lr") opt.useLR = false;
    else if (s == "--no-backprop") opt.backprop = false;
    else if (s == "--null-moves") opt.nullMoves = true;
    else if (s == "--areas-sym") opt.areaMode = 1;
    else if (s == "--ckpt-mins") opt.ckptMinutes = atof(next());
    else if (s == "--resume") opt.resume = true;
    else if (s == "--order") opt.order = atoi(next());
    else if (s == "--no-validate") opt.validate = false;
    else {
      fprintf(stderr,
              "usage: %s --m M --p P [--dir D] [--threads N] [--no-lr]\n"
              "  [--no-backprop] [--null-moves] [--ckpt-mins M] [--resume]\n"
              "  [--order 0|1|2] [--no-validate]\n",
              argv[0]);
      return 2;
    }
  }
  if (opt.dir.empty())
    opt.dir = "runs/m" + std::to_string(opt.m) + "p" + std::to_string(opt.p);
  std::string cmd = "mkdir -p " + opt.dir;
  if (system(cmd.c_str()) != 0) return 1;

  Solver solver(opt);
  gSolver = &solver;
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  return solver.run();
}
