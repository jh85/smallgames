/*
  amazons — plain negamax with memoization over canonical positions.
  Ground truth for tiny boards; used by the tests to validate the
  df-pn/BNS solver, and by `verify` for tiny variants.
*/
#pragma once

#include <cstdint>

#include "board.h"

namespace amazons {

// True iff the side to move wins. Exponential; only for tiny boards.
// If nodes is non-null, accumulates the number of positions expanded.
bool BruteForceStmWins(const Position& pos, uint64_t* nodes = nullptr);

// One solved canonical position (always white-to-move after
// canonicalization; stm_wins is from the side to move's perspective).
struct WdlEntry {
  uint64_t white, black, burned;
  bool stm_wins;
};

// Exhaustive WDL table for the one-amazon-each variant on a w x h board:
// retrograde-solves every canonical position reachable from any initial
// placement (W at square i, B at square j, i != j, White to move).
// Appends one entry per canonical position to `out`.  Returns false when
// `position_limit` was exceeded (output is then partial).  Amazons has no
// draws, so WDL here is W/L only.
bool EnumerateOneQueenWdl(int w, int h, std::vector<WdlEntry>* out,
                          uint64_t position_limit = UINT64_MAX);

}  // namespace amazons
