// Query the Oware table.
//
//   probe <dir> s0 s1 ... s(PITS-1) captured0 captured1 side_to_move
//
// Pits and players are indexed as in oware_6x2_paper_rules.md (P0 owns 0..ROW-1, P1
// owns ROW..PITS-1).  The total of all seed counts is the game's seed count (48 for standard
// Oware) and must match the table in <dir>.  Prints the value for the side to move and
// for each legal move:
//   WIN / LOSS   forced, under the real rules, from a fresh repetition history
//   DRAW         both sides can force exactly half the seeds
//   DRAW*        neither side can force a win by play that terminates on its own;
//                the real outcome would be decided by the repetition rule
#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <unistd.h>
#include "layer.hpp"
#include "oware.hpp"
using namespace oware;

static Index IX;
static Layer L[MAX_SEEDS + 1];
static std::string dir;

static const Layer& layer(int n) {
  Layer& ly = L[n];
  if (ly.data) return ly;
  ly.describe(n, IX.layerSize(n));
  std::string path = layerPath(dir, n);
  int fd = open(path.c_str(), O_RDONLY);
  FileHeader h;
  if (fd < 0 || read(fd, &h, sizeof h) != sizeof h || memcmp(h.magic, "OWARELH1", 8) || h.n != n ||
      !headerSeedsMatch(h) || h.size != ly.size) {
    fprintf(stderr, "cannot use %s for a %d-seed game\n", path.c_str(), SEEDS); exit(1);
  }
  ly.mode = h.mode; ly.bytes = h.bytes;
  void* p = mmap(nullptr, sizeof h + h.bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  ly.data = static_cast<uint8_t*>(p) + sizeof h;
  return ly;
}

// Guarantees (seeds out of the n on the board) for mover / non-mover of board b.
static void guarantees(const Board& b, int n, int& gM, int& gN) {
  bool indexed = false;
  for (int j = ROW; j < PITS; ++j) indexed |= b.p[j] == 0;
  if (indexed) {
    const Layer& ly = layer(n);
    int p = ly.get(IX.rank(b, n));
    gM = ly.gM(p); gN = ly.gN(p);
    return;
  }
  int mask = legalMask(b);
  if (!mask) { gM = b.rowSum(0); gN = b.rowSum(ROW); return; }
  gM = 0; gN = 99;
  Board c;
  for (int i = 0; i < ROW; ++i) if (mask >> i & 1) {
    int cap = play(b, i, c), cm, cn;
    guarantees(c, n - cap, cm, cn);
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
  if (argc != PITS + 5) { fprintf(stderr, "usage: probe <dir> s0..s%d captured0 captured1 side_to_move\n", PITS - 1); return 1; }
  dir = argv[1];
  int s[PITS], cap[2], stm, tot = 0;
  for (int j = 0; j < PITS; ++j) tot += s[j] = atoi(argv[2 + j]);
  cap[0] = atoi(argv[PITS + 2]); cap[1] = atoi(argv[PITS + 3]); stm = atoi(argv[PITS + 4]);
  SEEDS = tot + cap[0] + cap[1];
  if (SEEDS < 2 || SEEDS > MAX_SEEDS || SEEDS % 2 || (stm != 0 && stm != 1)) { fprintf(stderr, "invalid state\n"); return 1; }
  const int win = winSeeds();
  Board b;
  for (int j = 0; j < PITS; ++j) b.p[j] = uint8_t(s[(j + ROW * stm) % PITS]);
  int cM = cap[stm], cN = cap[1 - stm], gM, gN;
  guarantees(b, tot, gM, gN);
  printf("side to move P%d: %s  (can force a final score >= %d, opponent can force >= %d;\n"
         "  bounds are clamped to the range that matters for win/draw/loss)\n",
         stm, verdict(cM, cN, gM, gN), cM + gM, cN + gN);
  if (cM >= win || cN >= win || (cM == win - 1 && cN == win - 1)) return 0;
  int mask = legalMask(b);
  if (!mask) { printf("no legal move: remaining seeds are collected by row ownership\n"); return 0; }
  for (int i = 0; i < ROW; ++i) if (mask >> i & 1) {
    Board c;
    int k = play(b, i, c), cm, cn;
    guarantees(c, tot - k, cm, cn);
    // after the move the opponent is the mover of c
    printf("  pit %2d: captures %2d -> %s\n", i + ROW * stm, k, verdict(cM + k, cN, cn, cm));
  }
  return 0;
}
