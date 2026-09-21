// Cross-check play()/legalMask() against transitions dumped from OpenSpiel
// (tools/openspiel_trace.py).  Reads the trace on stdin.
#include <cstdio>
#include "oware.hpp"
using namespace oware;

int main() {
  long lines = 0, bad = 0, captures = 0;
  int v[32];
  for (;;) {
    for (int j = 0; j < 32; ++j)
      if (scanf("%d", &v[j]) != 1) {
        printf("%ld transitions checked, %ld with captures, %ld mismatches\n", lines, captures, bad);
        return bad != 0;
      }
    ++lines;
    int stm = v[14], mask = v[15], a = v[16];
    const int* nx = v + 17;
    Board b, c;
    for (int j = 0; j < PITS; ++j) b.p[j] = uint8_t(v[(j + 6 * stm) % PITS]);
    bool ok = legalMask(b) == mask;
    int cap = play(b, a, c);
    captures += cap > 0;
    int gained = nx[12 + stm] - v[12 + stm];
    if (!v[31]) {  // successor still in play: boards and scores must agree exactly
      for (int j = 0; j < PITS; ++j) ok &= c.p[j] == nx[(j + 6 * (1 - stm)) % PITS];
      ok &= gained == cap && nx[13 - stm] == v[13 - stm];
    } else {       // terminal successor: OpenSpiel has swept the board
      ok &= gained >= cap;
    }
    if (!ok && ++bad <= 10) printf("MISMATCH at line %ld\n", lines);
  }
}
