// amazons — branch-number / proof-number arithmetic tests, mirroring the
// invariants checked by JHBR3/test/test_bns.cc.
#include <cstdio>

#include "arith.h"

using namespace amazons;
using amazons::bns::ChildView;
using amazons::bns::Summary;
using amazons::bns::kInf;

#define CHECK(cond)                                       \
  do {                                                    \
    if (!(cond)) {                                        \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, \
                   __LINE__, #cond);                      \
      return 1;                                           \
    }                                                     \
  } while (0)

static int TestTerminalPropagation() {
  // OR node with a proved child is proved.
  {
    const ChildView c[3] = {{1, 1}, {0, kInf}, {3, 7}};
    const Summary s = bns::Summarize<true, false>(c, 3);
    CHECK(s.proved && !s.disproved);
    CHECK(s.abn == 0 && s.obn == kInf);
    CHECK(s.best == 1);
  }
  // OR node with all children disproved is disproved.
  {
    const ChildView c[2] = {{kInf, 0}, {kInf, 0}};
    const Summary sb = bns::Summarize<true, false>(c, 2);
    const Summary sp = bns::Summarize<true, true>(c, 2);
    CHECK(sb.disproved && sb.obn == 0 && sb.abn == kInf);
    CHECK(sp.disproved && sp.obn == 0 && sp.abn == kInf);
  }
  return 0;
}

static int TestArithmeticDifference() {
  // Children {2,3},{2,3}: BNS counts unresolved siblings (obn = 3 + 1),
  // pn/dn sums them (obn = 3 + 3).
  const ChildView c[2] = {{2, 3}, {2, 3}};
  const Summary sb = bns::Summarize<true, false>(c, 2);
  CHECK(sb.abn == 2 && sb.obn == 4 && sb.k == 2 && sb.second == 2);
  const Summary sp = bns::Summarize<true, true>(c, 2);
  CHECK(sp.abn == 2 && sp.obn == 6);
  // Single unresolved child: both agree (obn = best.obn + 0 = sum of one).
  const ChildView one[1] = {{2, 3}};
  CHECK((bns::Summarize<true, false>(one, 1).obn) == 3);
  CHECK((bns::Summarize<true, true>(one, 1).obn) == 3);
  // Disproved children (rel >= kInf) are excluded from k.
  const ChildView mix[3] = {{2, 3}, {kInf, 0}, {5, 1}};
  const Summary sm = bns::Summarize<true, false>(mix, 3);
  CHECK(sm.k == 2 && sm.abn == 2 && sm.obn == 3 + 1 && sm.second == 5);
  return 0;
}

static int TestThresholds() {
  // OR node: ABN' = min(second + 1, ABN), OBN' = OBN - (node.obn - best.obn).
  const ChildView c[3] = {{2, 4}, {3, 9}, {6, 2}};
  const Summary s = bns::Summarize<true, false>(c, 3);
  CHECK(s.best == 0 && s.second == 3 && s.obn == 4 + 2);
  uint32_t ca, co;
  bns::ChildThresholds<true>(s, c[s.best], kInf, kInf, &ca, &co);
  CHECK(ca == 4 && co == kInf);  // min(second + 1, INF)
  bns::ChildThresholds<true>(s, c[s.best], 3, 10, &ca, &co);
  CHECK(ca == 3);              // clamped by ABN
  CHECK(co == 10 - (s.obn - 4));  // OBN - (node.obn - best.obn)
  return 0;
}

int main() {
  if (int r = TestTerminalPropagation()) return r;
  if (int r = TestArithmeticDifference()) return r;
  if (int r = TestThresholds()) return r;
  std::printf("test_arith OK\n");
  return 0;
}
