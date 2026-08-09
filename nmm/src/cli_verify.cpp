// cli_verify — reproduce paper Tables 8-10 (9/11/12 Men's Morris) with the full sweep.
#include "../src/board.hpp"
#include "../src/zdd1.hpp"
#include "../src/zdd2.hpp"
#include "../src/paper_tables.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char** argv) {
  int game = argc > 1 ? atoi(argv[1]) : 9;
  int threads = (int)std::thread::hardware_concurrency();
  const PaperRow* rows;
  size_t nRows;
  u64 total;
  switch (game) {
    case 9:  rows = PAPER_9;  nRows = sizeof(PAPER_9) / sizeof(PaperRow);  total = PAPER_9_TOTAL;  break;
    case 11: rows = PAPER_11; nRows = sizeof(PAPER_11) / sizeof(PaperRow); total = PAPER_11_TOTAL; break;
    case 12: rows = PAPER_12; nRows = sizeof(PAPER_12) / sizeof(PaperRow); total = PAPER_12_TOTAL; break;
    default: fprintf(stderr, "game must be 9, 11, or 12\n"); return 1;
  }
  GameSpec sp = gameSpec(game);
  Board bd = buildBoard(sp);
  Zdd1 z;
  z.build(bd.m, 3, sp.pieces);
  printf("[game %d] zdd1 count=%llu, sweeping with %d threads...\n", game,
         (unsigned long long)z.total, threads);
  fflush(stdout);
  auto t0 = std::chrono::steady_clock::now();
  bool filter = !getenv("NOFILTER");
  SweepResult r = sweepAndBuild(bd, z, 3, sp.pieces, filter, false, nullptr, threads);
  double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  u64 sum = 0;
  int bad = 0;
  for (size_t i = 0; i < nRows; ++i) {
    u64 mine = rows[i].x == rows[i].y
                   ? r.tally[rows[i].x][rows[i].y]
                   : r.tally[rows[i].x][rows[i].y] + r.tally[rows[i].y][rows[i].x];
    sum += mine;
    if (mine != rows[i].c) {
      printf("  subset %2d-%-2d: mine=%15llu paper=%15llu diff=%+lld\n", rows[i].x,
             rows[i].y, (unsigned long long)mine, (unsigned long long)rows[i].c,
             (long long)(mine - rows[i].c));
      ++bad;
    }
  }
  printf("[game %d] rows: %zu, mismatched rows: %d\n", game, nRows, bad);
  printf("[game %d] total mine=%llu published=%llu %s   (%.1fs)\n", game,
         (unsigned long long)sum, (unsigned long long)total,
         sum == total ? "MATCH" : "MISMATCH", el);
  return 0;
}
