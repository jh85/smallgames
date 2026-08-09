// query.cpp -- Query and verification utility for solved tables.
//
// Usage:
//   query <run_dir> --m M --p P [--areas-sym] [--no-lr] <command>
// Commands:
//   --initial                 value of the initial position
//   --p1 c1,..,cp --p2 c1,..,cp [--stm 1|2]   value of a given state
//                             (cell ids 0..N-1, row-major from the top tip;
//                              player 1 starts at the top and moves down)
//   --verify N [--seed S]     sample N random states and check local
//                             consistency of the solved values:
//                               WIN  => exists a successor with value LOSS
//                               LOSS => all legal successors are WIN (or
//                                       terminal/stuck), and static terminal
//                                       classification agrees
//                               DRAW => no LOSS successor, >=1 DRAW successor
//   --stats                   scan the table and print value counts
#include "cc_core.hpp"

#include <random>
#include <string>
#include <vector>

using namespace cc;

static u64 parseCells(const char* s, int N) {
  u64 mask = 0;
  int c = 0;
  bool have = false;
  for (const char* q = s;; q++) {
    if (*q >= '0' && *q <= '9') {
      c = c * 10 + (*q - '0');
      have = true;
    } else {
      if (have) {
        if (c < 0 || c >= N) { fprintf(stderr, "bad cell %d\n", c); exit(2); }
        mask |= bit(c);
        c = 0;
        have = false;
      }
      if (!*q) break;
    }
  }
  return mask;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <run_dir> --m M --p P [options] <command>\n",
            argv[0]);
    return 2;
  }
  std::string dir = argv[1];
  int m = 0, p = 0, stm = 1;
  bool areasSym = false, useLR = true, initial = false, stats = false;
  u64 verifyN = 0;
  u64 seed = 20260804;
  u64 p1mask = 0, p2mask = 0;
  bool havePieces = false;
  for (int i = 2; i < argc; i++) {
    std::string s = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    if (s == "--m") m = atoi(next());
    else if (s == "--p") p = atoi(next());
    else if (s == "--areas-sym") areasSym = true;
    else if (s == "--no-lr") useLR = false;
    else if (s == "--initial") initial = true;
    else if (s == "--stats") stats = true;
    else if (s == "--stm") stm = atoi(next());
    else if (s == "--p1") { p1mask = parseCells(next(), 49); havePieces = true; }
    else if (s == "--p2") { p2mask = parseCells(next(), 49); havePieces = true; }
    else if (s == "--verify") verifyN = strtoull(next(), nullptr, 10);
    else if (s == "--seed") seed = strtoull(next(), nullptr, 10);
    else { fprintf(stderr, "unknown arg %s\n", s.c_str()); return 2; }
  }
  if (!m || !p) { fprintf(stderr, "--m and --p required\n"); return 2; }
  Game g(m, p, areasSym ? Game::SYMMETRIC : Game::FIRSTK);
  Index ix(g, useLR);
  Table table;
  table.alloc(ix.totalStored);
  if (!table.loadFrom(dir + "/table.bin")) {
    fprintf(stderr, "cannot load %s/table.bin\n", dir.c_str());
    return 1;
  }

  auto lookup = [&](u64 a, u64 b, int sideToMove) -> u8 {
    // a = player 1 pieces (starts top, moves down), b = player 2 pieces.
    return (sideToMove == 1) ? table.get(ix.indexOf(a, b))
                             : table.get(ix.indexOfP2ToMove(a, b));
  };

  if (initial) {
    u8 v = lookup(g.startMask, g.goalMask, 1);
    printf("initial position (player 1 to move): %s\n", valueName(v));
    return 0;
  }
  if (havePieces) {
    if (__builtin_popcountll(p1mask) != p || __builtin_popcountll(p2mask) != p ||
        (p1mask & p2mask)) {
      fprintf(stderr, "invalid piece sets\n");
      return 2;
    }
    u8 v = lookup(p1mask, p2mask, stm);
    printf("state (player %d to move): %s\n", stm, valueName(v));
    return 0;
  }
  if (stats) {
    u64 cnt[4] = {0, 0, 0, 0};
    for (u32 blk = 0; blk < ix.storedBlocks.size(); blk++) {
      u32 r1 = ix.storedBlocks[blk];
      int w = ix.blockWeight(r1);
      i64 idx = ix.base[r1];
      for (u32 r2 = 0; r2 < ix.C2; r2++, idx++) cnt[table.get(idx)] += w;
    }
    u64 canon = cnt[0] + cnt[1] + cnt[2] + cnt[3];
    printf("canonical states: %llu\n", (unsigned long long)canon);
    printf("  win=%llu loss=%llu draw=%llu illegal=%llu\n",
           (unsigned long long)cnt[WIN], (unsigned long long)cnt[LOSS],
           (unsigned long long)cnt[UNKNOWN], (unsigned long long)cnt[ILLEGAL]);
    printf("paper-style: positions=%llu wins=%llu draws=%llu illegal=%llu\n",
           (unsigned long long)(2 * canon),
           (unsigned long long)(cnt[WIN] + cnt[LOSS]),
           (unsigned long long)(2 * cnt[UNKNOWN]),
           (unsigned long long)(2 * cnt[ILLEGAL]));
    return 0;
  }
  if (verifyN) {
    std::mt19937_64 rng(seed);
    u64 bad = 0;
    for (u64 t = 0; t < verifyN; t++) {
      // random canonical state
      u64 a = 0, b = 0;
      while (__builtin_popcountll(a) < p) a |= bit(rng() % g.N);
      while (__builtin_popcountll(b) < p) {
        int c = rng() % g.N;
        if (!(a >> c & 1)) b |= bit(c);
      }
      i64 idx = ix.indexOf(a, b);
      // re-canonicalize through the index so we check the stored slot itself
      u8 v = table.get(idx);
      u8 st = g.classify(a, b);
      // static classes must match exactly
      if (st == ILLEGAL || st == LOSS) {
        if (v != st) {
          bad++;
          fprintf(stderr, "static mismatch at idx %lld: table=%s static=%s\n",
                  (long long)idx, valueName(v), valueName(st));
        }
        continue;
      }
      if (v == ILLEGAL) { bad++; continue; }
      // dynamic check: examine successors. NOTE: because of left-right
      // symmetry the stored slot may represent the mirrored state; values
      // are mirror-invariant so checking (a,b) itself is equivalent.
      bool anyLoss = false, anyDraw = false, allWin = true;
      int legal = 0;
      u64 am = a;
      while (am) {
        int from = __builtin_ctzll(am);
        am &= am - 1;
        u64 d = g.pieceDests(from, a | b);
        while (d) {
          int to = __builtin_ctzll(d);
          d &= d - 1;
          u64 a2 = (a ^ bit(from)) | bit(to);
          u8 sv = table.get(ix.indexOfP2ToMove(a2, b));
          if (sv == ILLEGAL) continue;
          legal++;
          if (sv == LOSS) anyLoss = true;
          else if (sv == UNKNOWN) { anyDraw = true; allWin = false; }
          else if (sv != WIN) allWin = false;
        }
      }
      bool ok = true;
      if (v == WIN) ok = anyLoss;
      else if (v == LOSS) ok = !anyLoss && allWin;  // includes legal==0
      else if (v == UNKNOWN) ok = !anyLoss && anyDraw;
      if (!ok) {
        bad++;
        fprintf(stderr,
                "consistency FAIL idx %lld value=%s anyLoss=%d allWin=%d "
                "anyDraw=%d legal=%d\n",
                (long long)idx, valueName(v), anyLoss, allWin, anyDraw, legal);
      }
    }
    printf("verify: %llu samples, %llu failures -> %s\n",
           (unsigned long long)verifyN, (unsigned long long)bad,
           bad == 0 ? "PASS" : "FAIL");
    return bad == 0 ? 0 : 1;
  }
  fprintf(stderr, "no command given\n");
  return 2;
}
