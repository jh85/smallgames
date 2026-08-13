// verify_forward: independent brute-force WDL check of computed slice files.
//
// Uses only basic board primitives (pack/unpack/rotations/won) — no sections,
// no supers, no block machinery — so it validates the engine's whole pipeline.
//
// Modes:
//   verify_forward board HEX              solve one board (side to move = sum&1)
//   verify_forward sample PGS N [seed]    solve N random nodes and compare
//   verify_forward exhaustive PGS         solve every node (small slices only)
//
// Warning: the plain solver is exponential; use only for high slices (>=30)
// or tiny boards.

#include "slice_file.h"
#include "pentago/base/board.h"
#include "pentago/base/score.h"
#include "pentago/mid/midengine.h"
#include "pentago/high/board.h"
#include <iostream>
#include <unordered_map>
#include <random>

using namespace pgs;
using namespace pentago;

namespace {

struct cache_t {
  std::unordered_map<uint64_t,int> m; // key = side0 ^ (side1 rotated into high bits)? use combine
  static uint64_t key(side_t a, side_t b) {
    return (a * 0x9e3779b97f4a7c15ull) ^ (b + 0x2545f4914f6cdd1dull);
  }
};

// Exact value for the player to move: side0 = mover's stones, side1 = other.
// 1 = win, 0 = tie, -1 = loss.  Rules per paper section II.
int solve(side_t side0, side_t side1, cache_t& cache, uint64_t& nodes) {
  nodes++;
  const bool w0 = won(side0), w1 = won(side1);
  if (w0 || w1) return w0 ? (w1 ? 0 : 1) : -1; // someone already made five
  const side_t occ = side0|side1;
  if (occ == side_mask) return 0; // full board: tie

  const uint64_t key = cache_t::key(side0, side1);
  const auto it = cache.m.find(key);
  if (it != cache.m.end()) return it->second;

  int best = -1;
  side_t free_ = side_mask & ~occ;
  while (free_ && best < 1) {
    const side_t move = free_ & -free_;
    free_ ^= move;
    const side_t placed = side0 | move;
    if (won(placed)) { best = 1; break; } // immediate win, no rotation
    for (int q=0;q<4 && best<1;q++) {
      const int shift = 16*q;
      const side_t oq0 = (placed>>shift)&0x1ff, oq1 = (side1>>shift)&0x1ff;
      for (int dir=0;dir<2 && best<1;dir++) {
        const side_t c0 = (placed & ~((side_t)0x1ff<<shift)) | (side_t)rotations[oq0][dir]<<shift;
        const side_t c1 = (side1  & ~((side_t)0x1ff<<shift)) | (side_t)rotations[oq1][dir]<<shift;
        const int v = -solve(c1, c0, cache, nodes);
        if (v > best) best = v;
      }
    }
  }
  cache.m[key] = best;
  return best;
}

// Board value from the slice file: -1/0/1 for the player to move.
// The board must be a section representative (standard section, rotation
// minimal quadrants).
int file_value(slice_reader_t& reader, board_t board) {
  const section_t section = count(board);
  const int si = reader.find_section(section);
  if (si < 0) throw std::runtime_error("section not found");
  const auto& e = reader.entry(si);
  uint64_t node = 0;
  for (int q=0;q<4;q++) {
    const quadrant_t quad = (board >> (16*q)) & 0xffff;
    const uint16_t ir = rotation_minimal_quadrants_inverse[quad];
    if (ir & 3) throw std::runtime_error("non-minimal quadrant");
    node = node*e.shape[q] + ir/4;
  }
  const auto val = reader.read_node(si, node);
  const bool w = val[0](0), nl = val[1](0); // identity rotation
  return w ? 1 : nl ? 0 : -1;
}

board_t node_board(const section_t& section, const Vector<int,4>& shape, uint64_t node) {
  const auto rmin = vec(get<0>(rotation_minimal_quadrants(section.counts[0])),
                        get<0>(rotation_minimal_quadrants(section.counts[1])),
                        get<0>(rotation_minimal_quadrants(section.counts[2])),
                        get<0>(rotation_minimal_quadrants(section.counts[3])));
  quadrant_t qs[4];
  for (int q=3;q>=0;q--) { qs[q] = rmin[q][node % shape[q]]; node /= shape[q]; }
  return quadrants(qs[0], qs[1], qs[2], qs[3]);
}

section_t entry_section(const section_entry_t& e) {
  section_t s;
  for (int q=0;q<4;q++)
    s.counts[q] = vec((uint8_t)(e.sig >> (16*q) & 0xff), (uint8_t)(e.sig >> (16*q+8) & 0xff));
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: verify_forward board HEX | sample PGS N [seed] | exhaustive PGS\n";
    return 1;
  }
  try {
    const std::string mode = argv[1];
    if (mode == "board") {
      const board_t b = strtoull(argv[2], 0, 16);
      const int turn = count(b).sum()&1;
      cache_t cache;
      uint64_t nodes = 0;
      const int v = solve(unpack(b,turn), unpack(b,1-turn), cache, nodes);
      std::cout << "value " << v << " (nodes " << nodes << ")\n";
      return 0;
    }

    slice_reader_t reader(argv[2], (uint64_t)8<<30);
    long bad = 0, checked = 0;
    uint64_t nodes = 0;

    auto check = [&](int si, uint64_t node) {
      const auto& e = reader.entry(si);
      const section_t section = entry_section(e);
      const Vector<int,4> shape(e.shape[0], e.shape[1], e.shape[2], e.shape[3]);
      const board_t b = node_board(section, shape, node);
      const int turn = count(b).sum()&1;
      cache_t cache;
      const int want = solve(unpack(b,turn), unpack(b,1-turn), cache, nodes);
      const int got = file_value(reader, b);
      checked++;
      if (got != want) {
        bad++;
        if (bad <= 10)
          std::cerr << "MISMATCH section " << si << " node " << node << " board "
                    << std::hex << b << std::dec << ": file " << got << " vs forward " << want << "\n";
      }
    };

    if (mode == "sample" || mode == "mid") {
      const long n = atol(argv[3]);
      std::mt19937_64 rng(argc > 4 ? strtoull(argv[4],0,10) : 12345);
      // Weight sections by size
      std::vector<uint64_t> sizes;
      uint64_t total = 0;
      for (size_t i=0;i<reader.section_id.size();i++) {
        const auto& e = reader.entry(i);
        total += (uint64_t)e.shape[0]*e.shape[1]*e.shape[2]*e.shape[3];
        sizes.push_back(total);
      }
      Array<halfsuper_s> workspace;
      if (mode == "mid") workspace = midsolve_workspace(reader.slice());
      for (long i=0;i<n;i++) {
        const uint64_t r = rng() % total;
        const int si = std::lower_bound(sizes.begin(), sizes.end(), r+1) - sizes.begin();
        const auto& e = reader.entry(si);
        const uint64_t size = (uint64_t)e.shape[0]*e.shape[1]*e.shape[2]*e.shape[3];
        if (mode == "sample") {
          check(si, rng() % size);
        } else {
          // midengine: independent engine, identity rotation only
          const section_t section = entry_section(e);
          const Vector<int,4> shape(e.shape[0], e.shape[1], e.shape[2], e.shape[3]);
          const uint64_t node = rng() % size;
          const board_t b = node_board(section, shape, node);
          const auto values = midsolve(high_board_t::from_board(b, false), workspace);
          // midsolve_traverse appends post-order; find the root by raw board
          const raw_t root_raw = high_board_t::from_board(b, false).raw();
          int want = -2;
          for (const auto& [raw, value] : values)
            if (raw == root_raw) want = value;
          if (want == -2) throw std::runtime_error("root not in midsolve results");
          const int got = file_value(reader, b);
          checked++;
          if (got != want) {
            bad++;
            if (bad <= 10)
              std::cerr << "MISMATCH section " << si << " node " << node << " board "
                        << std::hex << b << std::dec << ": file " << got << " vs midengine " << want << "\n";
          }
        }
      }
    } else if (mode == "exhaustive") {
      for (size_t si=0;si<reader.section_id.size();si++) {
        const auto& e = reader.entry(si);
        const uint64_t size = (uint64_t)e.shape[0]*e.shape[1]*e.shape[2]*e.shape[3];
        for (uint64_t node=0;node<size;node++) check(si, node);
      }
    } else {
      std::cerr << "unknown mode\n";
      return 1;
    }
    std::cout << "slice " << reader.slice() << ": checked " << checked << ", mismatches "
              << bad << " (forward nodes " << nodes << ")\n";
    return bad != 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
