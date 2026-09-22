// Cross-check play()/legalMask() against transitions dumped from OpenSpiel
// (tools/openspiel_trace.py).  Reads the trace on stdin.
#include <cstdio>
#include "oware.hpp"
using namespace oware;

int main() {
  long lines = 0, bad = 0, captures = 0;
  constexpr int NV = 2 * PITS + 8;  // seeds, c0, c1, stm, mask, action, seeds', c0', c1', terminal
  int v[NV];
  for (;;) {
    for (int j = 0; j < NV; ++j)
      if (scanf("%d", &v[j]) != 1) {
        printf("%ld transitions checked, %ld with captures, %ld mismatches\n", lines, captures, bad);
        return bad != 0;
      }
    ++lines;
    int stm = v[PITS + 2], mask = v[PITS + 3], a = v[PITS + 4];
    const int* nx = v + PITS + 5;
    Board b, c;
    for (int j = 0; j < PITS; ++j) b.p[j] = uint8_t(v[(j + ROW * stm) % PITS]);
    bool ok = legalMask(b) == mask;
    int cap = play(b, a, c);
    captures += cap > 0;
    int gained = nx[PITS + stm] - v[PITS + stm];
    if (!v[NV - 1]) {  // successor still in play: boards and scores must agree exactly
      for (int j = 0; j < PITS; ++j) ok &= c.p[j] == nx[(j + ROW * (1 - stm)) % PITS];
      ok &= gained == cap && nx[PITS + 1 - stm] == v[PITS + 1 - stm];
    } else {       // terminal successor: OpenSpiel has swept the board
      ok &= gained >= cap;
    }
    if (!ok && ++bad <= 10) printf("MISMATCH at line %ld\n", lines);
  }
}
