// verify_sparse: check a computed slice file against the author's sparse
// samples (girving/data/sparse-N.npy), which were produced by his production
// run on Edison and are independent ground truth for this engine.
//
// Each sample is 9 little-endian uint64: board_t board, then two super_t
// (black_wins, white_wins) over the 256 rotations of that board.
// Conversion from our stored (wins, not_loss) for the player to move:
//   black to play (slice even): (black_wins, white_wins) = (wins, ~not_loss)
//   white to play (slice odd):  (black_wins, white_wins) = (~not_loss, wins)
// (see girving/pentago/mpi/io.cc write_sparse_samples).
//
// Usage: verify_sparse SLICE_PGS SPARSE_NPY [limit]

#include "slice_file.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>

using namespace pgs;
using namespace pentago;

static std::vector<Vector<uint64_t,9>> read_npy(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("open failed: " + path);
  char magic[8];
  in.read(magic, 8);
  if (memcmp(magic, "\x93NUMPY\x01\x00", 8)) throw std::runtime_error("bad npy magic");
  uint16_t hlen;
  in.read((char*)&hlen, 2);
  std::string header(hlen, ' ');
  in.read(header.data(), hlen);
  // Parse shape from "{'descr': '<u8', 'fortran_order': False, 'shape': (N, 9), }"
  const auto p = header.find("'shape': (");
  if (p == std::string::npos) throw std::runtime_error("no shape");
  long rows, cols;
  if (sscanf(header.c_str()+p+10, "%ld, %ld", &rows, &cols) != 2 || cols != 9)
    throw std::runtime_error("bad shape");
  std::vector<Vector<uint64_t,9>> out(rows);
  in.read((char*)out.data(), 72L*rows);
  if (!in) throw std::runtime_error("short read");
  return out;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: verify_sparse SLICE_PGS SPARSE_NPY [limit]\n";
    return 1;
  }
  try {
    slice_reader_t reader(argv[1], (uint64_t)1<<30);
    const auto samples = read_npy(argv[2]);
    const long limit = argc > 3 ? std::min<long>(atol(argv[3]), samples.size()) : samples.size();
    const bool turn = reader.slice() & 1;

    long checked = 0, mismatches = 0;
    for (long s=0;s<limit;s++) {
      const board_t board = samples[s][0];
      const section_t section = count(board);
      const int si = reader.find_section(section);
      if (si < 0) { std::cerr << "section not found for sample " << s << "\n"; mismatches++; continue; }

      // Node index: rmin index per quadrant
      const auto shape = reader.entry(si).shape;
      uint64_t node = 0;
      bool ok = true;
      for (int q=0;q<4;q++) {
        const quadrant_t quad = (board >> (16*q)) & 0xffff;
        const uint16_t ir = rotation_minimal_quadrants_inverse[quad];
        if (ir & 3) { ok = false; break; } // sample board quadrant must be rotation minimal
        node = node*shape[q] + ir/4;
      }
      if (!ok) { std::cerr << "non-minimal quadrant in sample " << s << "\n"; mismatches++; continue; }

      const auto node_val = reader.read_node(si, node);
      // Convert (wins, not_loss) -> (black_wins, white_wins)
      super_t bw, ww;
      if (!turn) { bw = node_val[0]; ww = ~node_val[1]; }
      else       { ww = node_val[0]; bw = ~node_val[1]; }

      Vector<uint64_t,8> got, want;
      memcpy(&got[0], &bw, 32);
      memcpy(&got[4], &ww, 32);
      for (int i=0;i<8;i++) want[i] = samples[s][1+i];
      checked++;
      if (memcmp(&got, &want, 64)) {
        mismatches++;
        if (mismatches <= 3) {
          std::cerr << "MISMATCH sample " << s << " board " << std::hex << board << std::dec << "\n  got  ";
          for (int i=0;i<8;i++) std::cerr << std::hex << got[i] << " ";
          std::cerr << "\n  want ";
          for (int i=0;i<8;i++) std::cerr << std::hex << want[i] << " ";
          std::cerr << std::dec << "\n";
        }
      }
    }
    std::cout << "slice " << reader.slice() << ": checked " << checked
              << ", mismatches " << mismatches << "\n";
    return mismatches ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
