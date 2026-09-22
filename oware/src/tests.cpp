// Unit tests: rule examples from oware_6x2_paper_rules.md section 10, index bijection,
// published position count, and closure of the indexed set under play().
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include "layer.hpp"
#include "oware.hpp"
using namespace oware;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static Board mk(std::initializer_list<int> v) { Board b; int j = 0; for (int x : v) b.p[j++] = uint8_t(x); return b; }
static Board rot(const Board& b) { Board r; for (int j = 0; j < PITS; ++j) r.p[j] = b.p[(j + ROW) % PITS]; return r; }

int main() {
  Board c;
#if OWARE_ROW == 6
  // Backward capture: pit 5 has 2; opponent pits 6,7,8 = 1,2,4.
  Board b = mk({0,0,0,0,0,2, 1,2,4,0,0,0});
  CHECK(play(b, 5, c) == 5);
  CHECK(rot(c).p[6] == 0 && rot(c).p[7] == 0 && rot(c).p[8] == 4 && rot(c).p[5] == 0);
  // Grand slam despite an alternative.
  b = mk({0,0,0,0,1,1, 1,0,0,0,0,0});
  CHECK(legalMask(b) == 0b110000);
  CHECK(play(b, 5, c) == 0);
  CHECK(rot(c).p[4] == 1 && rot(c).p[5] == 0 && rot(c).p[6] == 2);
  // Mandatory feeding.
  b = mk({1,0,0,0,0,1, 0,0,0,0,0,0});
  CHECK(legalMask(b) == 0b100000);
  // Feeding impossible.
  b = mk({1,0,0,0,0,0, 0,0,0,0,0,0});
  CHECK(legalMask(b) == 0);
  // Sowing skips the origin on later circuits: 12 seeds from pit 0.
  b = mk({12,0,0,0,0,0, 0,0,0,0,0,1});
  CHECK(play(b, 0, c) == 0);  // last seed lands in own pit 1
  { Board r = rot(c); CHECK(r.p[0] == 0 && r.p[1] == 2 && r.p[2] == 1 && r.p[11] == 2); }
  // Exactly one full lap (11 seeds) ends on the pit before the origin and can capture.
  b = mk({11,0,0,0,0,0, 1,0,0,0,0,1});
  CHECK(play(b, 0, c) == 2);
  { Board r = rot(c); CHECK(r.p[11] == 0 && r.p[10] == 1 && r.p[6] == 2 && r.p[0] == 0 && r.p[1] == 1); }
  // Capture chain stops at the row boundary and at a non-2/3 pit.
  b = mk({0,0,0,0,0,6, 1,1,5,1,2,1});
  CHECK(play(b, 5, c) == 2 + 3 + 2);  // pits 11,10,9 -> 2,3,2 ; pit 8 = 6 stops
  // Initial position, first move.
  b = mk({4,4,4,4,4,4, 4,4,4,4,4,4});
  CHECK(play(b, 2, c) == 0);
  { Board r = rot(c); CHECK(r.p[2] == 0 && r.p[3] == 5 && r.p[6] == 5 && r.p[7] == 4); }
#elif OWARE_ROW == 7
  // 7x2 with 2 seeds per pit (OpenSpiel oware(num_houses_per_player=7,num_seeds_per_house=2)):
  // the first move from pit 6 sows pits 7,8 to 3 each and captures both.
  Board b = mk({2,2,2,2,2,2,2, 2,2,2,2,2,2,2});
  CHECK(legalMask(b) == 0b1111111);
  CHECK(play(b, 6, c) == 6);
  { Board r = rot(c); CHECK(r.p[6] == 0 && r.p[7] == 0 && r.p[8] == 0 && r.p[9] == 2 && r.p[5] == 2); }
  // 13 seeds from pit 0 make one full lap (skipping the origin) and end on pit 13.
  b = mk({13,0,0,0,0,0,0, 0,0,0,0,0,0,1});
  CHECK(play(b, 0, c) == 2);
  { Board r = rot(c); CHECK(r.p[0] == 0 && r.p[1] == 1 && r.p[12] == 1 && r.p[13] == 0); }
  // Mandatory feeding: pit i must hold more than ROW-1-i seeds.
  b = mk({6,0,0,0,0,0,1, 0,0,0,0,0,0,0});
  CHECK(legalMask(b) == 0b1000000);
  b = mk({7,0,0,0,0,0,0, 0,0,0,0,0,0,0});
  CHECK(legalMask(b) == 0b0000001);
#endif

  Index ix;
  unsigned __int128 tot = 0;
  for (int n = 0; n <= MAX_SEEDS; ++n) if (n != MAX_SEEDS - 1) tot += ix.layerSize(n);
#if OWARE_ROW == 6
  CHECK(uint64_t(tot) + 1 == 889063398406ull);  // Romein & Bal's Awari position count
#elif OWARE_ROW == 7
  CHECK(uint64_t(tot) == 4161983837529ull);  // 7x2 with 42 seeds, from the closed form
#endif

  // Value clamping for the standard game and for a 36-seed (3 per pit) variant.
  SEEDS = 48;
  CHECK(Layer::lowA(24) == 0 && Layer::numV(24) == 25);
  CHECK(Layer::lowA(25) == 0 && Layer::numV(25) == 26);
  CHECK(Layer::lowA(48) == 23 && Layer::numV(48) == 3);
  SEEDS = 36;
  CHECK(winSeeds() == 19);
  CHECK(Layer::lowA(18) == 0 && Layer::numV(18) == 19);
  CHECK(Layer::lowA(19) == 0 && Layer::numV(19) == 20);
  CHECK(Layer::lowA(36) == 17 && Layer::numV(36) == 3);
  SEEDS = MAX_SEEDS;

  for (int n = 0; n <= 9; ++n) {
    uint64_t sz = ix.layerSize(n);
    for (uint64_t x = 0; x < sz; ++x) {
      Board q = ix.unrank(x, n);
      CHECK(q.total() == n);
      CHECK(ix.rank(q, n) == x);
      int m = legalMask(q);
      for (int i = 0; i < ROW; ++i) if (m >> i & 1) {
        int cap = play(q, i, c);
        CHECK(c.total() == n - cap);
        bool zero = false;
        for (int j = ROW; j < PITS; ++j) zero |= c.p[j] == 0;
        CHECK(zero);
        CHECK(ix.rank(c, n - cap) < ix.layerSize(n - cap));
      }
    }
  }
  // Random round trips in big layers.
  srand(1);
  for (int it = 0; it < 2000000; ++it) {
    int n = MAX_SEEDS / 2 - 4 + rand() % (MAX_SEEDS / 2 + 5);
    uint64_t x = ((uint64_t(rand()) << 31) ^ rand()) % ix.layerSize(n);
    CHECK(ix.rank(ix.unrank(x, n), n) == x);
  }
  printf(fails ? "%d FAILURES\n" : "all tests passed\n", fails);
  return fails != 0;
}
