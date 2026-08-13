// midsolve: exact WDL for positions with 18+ stones, computed on demand by
// downstream retrograde analysis (wraps girving's midengine — the same engine
// that powers perfect-pentago.net; see DESIGN.md for why this is not alpha-beta).
//
// Usage: midsolve BOARD_HEX
// BOARD_HEX is a 16-hex-digit board_t (radix-3 quadrants; see wdl.cpp).
// Prints the value for the player to move and the value of every child
// (place + rotate).

#include "pentago/mid/midengine.h"
#include "pentago/high/board.h"
#include <iostream>

using namespace pentago;

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: midsolve BOARD_HEX\n";
    return 1;
  }
  try {
    const board_t board = strtoull(argv[1], 0, 16);
    const auto hb = high_board_t::from_board(board, false);
    const int n = hb.count();
    if (n < 18) {
      std::cerr << "board has " << n << " stones (<18): use wdl with the slice files\n";
      return 1;
    }
    auto workspace = midsolve_workspace(n);
    const auto values = midsolve(hb, workspace);
    // midsolve_traverse appends post-order: children first, root last.
    const raw_t root_raw = hb.raw();
    for (const auto& [raw, value] : values) {
      const auto b = high_board_t::from_raw(raw);
      std::cout << std::hex << b.board() << std::dec
                << " ply " << b.ply() << " value " << value
                << (value > 0 ? " WIN" : value < 0 ? " LOSS" : " DRAW")
                << (raw == root_raw ? "  <== root" : "") << "\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
