// wdl: look up the exact value of a Pentago position in computed slice files.
//
// Usage:
//   wdl --dir DIR BOARD_HEX            value of board (any rotation/symmetry)
//   wdl --dir DIR --moves BOARD_HEX    value of every legal move
//
// BOARD_HEX is a 16-hex-digit board_t (four radix-3 quadrants, quadrant 0 in
// the low bits; empty/white/black = 0/1/2 per 3^cell digit).
// Output: W/D/L for the player to move (black if even stones, else white).
//
// Boards with more stones than the highest computed slice are not in the
// database; use midsolve for those.

#include "slice_file.h"
#include "pentago/base/symmetry.h"
#include "pentago/base/score.h"
#include <iostream>
#include <cstring>
#include <memory>

using namespace pgs;
using namespace pentago;

namespace {

// Value of a standardized board (standard section, rotation-minimal quadrants)
// as (wins, not_loss) supers for the player to move.
Vector<super_t,2> lookup_std(slice_reader_t& reader, board_t std_board) {
  const section_t section = count(std_board);
  const int si = reader.find_section(section);
  if (si < 0) throw std::runtime_error("section not found (slice file missing/incomplete?)");
  const auto& e = reader.entry(si);
  uint64_t node = 0;
  for (int q=0;q<4;q++) {
    const quadrant_t quad = (std_board >> (16*q)) & 0xffff;
    const uint16_t ir = rotation_minimal_quadrants_inverse[quad];
    if (ir & 3) throw std::runtime_error("internal: non-minimal quadrant after standardize");
    node = node*e.shape[q] + ir/4;
  }
  return reader.read_node(si, node);
}

// Value of an arbitrary board: -1/0/1 for the player to move.
int board_value(slice_reader_t& reader, board_t board) {
  const int n = count(board).sum();
  const int turn = n&1;
  const side_t s0 = unpack(board, turn), s1 = unpack(board, 1-turn);
  const bool w0 = won(s0), w1 = won(s1);
  if (w0 || w1) return w0 ? (w1 ? 0 : 1) : -1; // five in a row
  if (n == 36) return 0; // full board

  const auto std = superstandardize(board);
  const auto val = lookup_std(reader, get<0>(std));
  // Transform supers back into the original board's frame
  const symmetry_t inv = get<1>(std).inverse();
  const super_t wins = transform_super(inv, val[0]);
  const super_t not_loss = transform_super(inv, val[1]);
  const bool w = wins(0), nl = not_loss(0);
  return w ? 1 : nl ? 0 : -1;
}

const char* name(int v) { return v > 0 ? "WIN " : v < 0 ? "LOSS" : "DRAW"; }

}  // namespace

int main(int argc, char** argv) {
  std::string dir = ".";
  bool moves = false;
  const char* board_arg = 0;
  for (int i=1;i<argc;i++) {
    if (!strcmp(argv[i],"--dir")) dir = argv[++i];
    else if (!strcmp(argv[i],"--moves")) moves = true;
    else board_arg = argv[i];
  }
  if (!board_arg) {
    std::cerr << "usage: wdl --dir DIR [--moves] BOARD_HEX\n";
    return 1;
  }
  try {
    const board_t board = strtoull(board_arg, 0, 16);
    const int n = count(board).sum();
    std::unique_ptr<slice_reader_t> reader, child_reader;
    if (n <= 35)
      reader.reset(new slice_reader_t(dir + "/slice-" + std::to_string(n) + ".pgs", (uint64_t)2<<30));
    if (moves && n+1 <= 35)
      child_reader.reset(new slice_reader_t(dir + "/slice-" + std::to_string(n+1) + ".pgs", (uint64_t)2<<30));

    if (n > 35) {
      const int turn = n&1;
      const bool w0 = won(unpack(board,turn)), w1 = won(unpack(board,1-turn));
      if (w0 || w1) std::cout << name(w0 ? (w1?0:1) : -1) << "\n";
      else std::cout << "DRAW (0) [full board]\n";
      return 0;
    }

    const int v = board_value(*reader, board);
    std::cout << name(v) << " (" << v << ") for " << (n&1 ? "white" : "black") << " to move\n";

    if (moves) {
      // Enumerate place+rotate moves; a placement that makes five wins immediately.
      const int turn = n&1;
      const side_t s0 = unpack(board, turn), s1 = unpack(board, 1-turn);
      side_t free_ = side_mask & ~(s0|s1);
      while (free_) {
        const side_t move = free_ & -free_;
        free_ ^= move;
        const side_t placed = s0 | move;
        const int bit = __builtin_ctzll(move);
        const int q0 = bit/9; // quadrant of the placement
        const side_t in_quad = (move >> (16*q0)) & 0x1ff;
        if (won(placed)) {
          std::cout << "place bit " << bit << ": WIN (immediate)\n";
          continue;
        }
        for (int q=0;q<4;q++) for (int d=0;d<2;d++) {
          // Place stone (radix-3 update in quadrant q0), then rotate quadrant q
          quadrant_t quads[4];
          for (int i=0;i<4;i++) quads[i] = (board >> (16*i)) & 0xffff;
          quads[q0] += (1+turn)*pack_table[in_quad];
          if (q0 == q)
            quads[q] = pack(rotations[unpack(quads[q],0)][d], rotations[unpack(quads[q],1)][d]);
          else
            quads[q] = pack(rotations[unpack(quads[q],0)][d], rotations[unpack(quads[q],1)][d]);
          const board_t child = quadrants(quads[0], quads[1], quads[2], quads[3]);
          const int cv = -board_value(*child_reader, child);
          std::cout << "place bit " << bit << " rot q" << q << (d?"R":"L")
                    << ": " << name(cv) << "\n";
        }
      }
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
