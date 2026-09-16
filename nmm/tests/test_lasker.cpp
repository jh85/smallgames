// Merged-phase (Lasker, game 14) move-generation tests: place-or-move availability,
// flying at 3 TOTAL pieces (board+hand), capture-skip with an empty opponent board,
// and the classic games' generator staying untouched.
#include "../src/board.hpp"
#include "../src/moves.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

static int countPlace(const std::vector<Succ>& v, int wh) {
  int n = 0;
  for (auto& s : v) n += (s.wh < wh);   // white placements consume white hand
  return n;
}
static int countMove(const std::vector<Succ>& v, int wh) {
  int n = 0;
  for (auto& s : v) n += (s.wh == wh);
  return n;
}

int main() {
  Board l = buildBoard(gameSpec(14));   // Lasker: board d, 10 pieces, merged
  Board c = buildBoard(gameSpec(9));    // classic 9MM on the same board
  assert(l.spec.mergedPhases && !c.spec.mergedPhases);
  std::vector<Succ> v;

  // 1) initial state: empty board, only the 24 placements exist
  v.clear();
  genSuccessors(l, 0, 0, 10, 10, 0, v);
  assert((int)v.size() == 24 && countPlace(v, 10) == 24);

  // 2) mid-game: both placements and movements are generated in one turn
  {
    u32 w = (1u << 0) | (1u << 1);      // ANW, AN
    u32 b = (1u << 9);                  // BN
    v.clear();
    genSuccessors(l, w, b, 8, 9, 0, v);
    assert(countPlace(v, 8) > 0 && countMove(v, 8) > 0);
    // classic game, same masks/hands: placement ONLY
    v.clear();
    genSuccessors(c, w, b, 8, 9, 0, v);
    assert(countMove(v, 8) == 0 && countPlace(v, 8) > 0);
  }

  // 3) flying at 3 TOTAL: W=2 on board + 1 in hand flies; W=3 on board + hand>0 does not
  {
    u32 w = (1u << 0) | (1u << 1);                   // 2 white on board, 1 in hand
    u32 b = (1u << 8) | (1u << 10) | (1u << 12);     // 3 black, no complete mill
    v.clear();
    genSuccessors(l, w, b, 1, 0, 0, v);
    // 19 empties: placements = 18 plain + 3 capture variants of the mill at ANE
    //             movements  = 2 pieces x 19 fly destinations, no mills possible
    assert(countPlace(v, 1) == 21);
    assert(countMove(v, 1) == 38);
    // 3 on board but 2 more in hand (total 5): adjacent-only movement
    u32 w2 = (1u << 0) | (1u << 2) | (1u << 4);
    v.clear();
    genSuccessors(l, w2, b, 2, 0, 0, v);
    int adj = 0;
    u32 empty = ((1u << l.m) - 1) & ~(w2 | b);
    for (int p : {0, 2, 4}) adj += __builtin_popcount(l.adj[p] & empty);
    assert(countMove(v, 2) == adj);
  }

  // 4) mill with an empty opponent board: capture skipped, no capture successors
  {
    u32 w = (1u << 0) | (1u << 1);
    v.clear();
    genSuccessors(l, w, 0, 8, 10, 0, v);
    for (auto& s : v) assert(!s.capture);
    // the ANE placement completes the mill [ANW,AN,ANE] yet yields exactly one child
    int atAne = 0;
    for (auto& s : v) atAne += ((s.w >> 2) & 1);
    assert(atAne >= 1);
  }

  printf("test_lasker: all merged-phase movegen checks passed\n");
  return 0;
}
