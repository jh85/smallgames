// Query the gM-only block-compressed Oware table (.gmz files, see gmz.cpp).
//
//   probez <dir> s0 s1 ... s(PITS-1) captured0 captured1 side_to_move
//
// Same interface and output as probe, reading .gmz instead of .lh files.  gN is
// derived from the children's gM (GmzReader::gN), so a query touches one
// compressed block per layer visited (a few dozen microseconds each).
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "gmz.hpp"
#include "oware.hpp"
using namespace oware;

static Index IX;
static GmzTable T;

static void guarantees(GmzReader& R, const Board& b, int n, int& gM, int& gN) {
  bool indexed = false;
  for (int j = ROW; j < PITS; ++j) indexed |= b.p[j] == 0;
  if (indexed) {
    gM = R.gM(n, IX.rank(b, n));
    gN = R.gN(b, n, IX);
    return;
  }
  int mask = legalMask(b);
  if (!mask) { gM = b.rowSum(0); gN = b.rowSum(ROW); return; }
  gM = 0; gN = 99;
  Board c;
  for (int i = 0; i < ROW; ++i) if (mask >> i & 1) {
    int cap = play(b, i, c), cm, cn;
    guarantees(R, c, n - cap, cm, cn);
    gM = std::max(gM, cap + cn); gN = std::min(gN, cm);
  }
}

static const char* verdict(int cM, int cN, int gM, int gN) {
  const int win = winSeeds(), half = win - 1;
  if (cM >= win) return "WIN";
  if (cN >= win) return "LOSS";
  if (cM + gM >= win) return "WIN";
  if (cN + gN >= win) return "LOSS";
  if (cM + gM == half && cN + gN == half) return "DRAW";
  return "DRAW*";
}

int main(int argc, char** argv) {
  if (argc != PITS + 5) { fprintf(stderr, "usage: probez <dir> s0..s%d captured0 captured1 side_to_move\n", PITS - 1); return 1; }
  T.open(argv[1]);
  int s[PITS], cap[2], stm, tot = 0;
  for (int j = 0; j < PITS; ++j) tot += s[j] = atoi(argv[2 + j]);
  cap[0] = atoi(argv[PITS + 2]); cap[1] = atoi(argv[PITS + 3]); stm = atoi(argv[PITS + 4]);
  SEEDS = tot + cap[0] + cap[1];
  if (SEEDS < 2 || SEEDS > MAX_SEEDS || SEEDS % 2 || (stm != 0 && stm != 1)) { fprintf(stderr, "invalid state\n"); return 1; }
  const int win = winSeeds();
  GmzReader R(T);
  Board b;
  for (int j = 0; j < PITS; ++j) b.p[j] = uint8_t(s[(j + ROW * stm) % PITS]);
  int cM = cap[stm], cN = cap[1 - stm], gM, gN;
  guarantees(R, b, tot, gM, gN);
  printf("side to move P%d: %s  (can force a final score >= %d, opponent can force >= %d;\n"
         "  bounds are clamped to the range that matters for win/draw/loss)\n",
         stm, verdict(cM, cN, gM, gN), cM + gM, cN + gN);
  if (cM >= win || cN >= win || (cM == win - 1 && cN == win - 1)) return 0;
  int mask = legalMask(b);
  if (!mask) { printf("no legal move: remaining seeds are collected by row ownership\n"); return 0; }
  for (int i = 0; i < ROW; ++i) if (mask >> i & 1) {
    Board c;
    int k = play(b, i, c), cm, cn;
    guarantees(R, c, tot - k, cm, cn);
    printf("  pit %2d: captures %2d -> %s\n", i + ROW * stm, k, verdict(cM + k, cN, cn, cm));
  }
  return 0;
}
