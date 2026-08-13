// verify_counts: check a computed slice file against the author's counts-N.npy
// (per-section black-wins / white-wins / total over all 256 rotations of every
// node, weighted by rotation stabilizers; see girving/pentago/base/count.cc).
//
// This validates section structure, node indexing, and computed values in one
// pass.  Usage: verify_counts SLICE_PGS COUNTS_NPY

#include "slice_file.h"
#include "pentago/base/count.h"
#include "pentago/base/board.h"
#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>

using namespace pgs;
using namespace pentago;

struct count_row_t { uint64_t sig, bw, ww, total; };

static std::vector<count_row_t> read_counts(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("open failed: " + path);
  char magic[8];
  in.read(magic, 8);
  if (memcmp(magic, "\x93NUMPY\x01\x00", 8)) throw std::runtime_error("bad npy magic");
  uint16_t hlen;
  in.read((char*)&hlen, 2);
  std::string header(hlen, ' ');
  in.read(header.data(), hlen);
  const auto p = header.find("'shape': (");
  if (p == std::string::npos) throw std::runtime_error("no shape");
  long rows, cols;
  if (sscanf(header.c_str()+p+10, "%ld, %ld", &rows, &cols) != 2 || cols != 4)
    throw std::runtime_error("bad shape");
  std::vector<count_row_t> out(rows);
  in.read((char*)out.data(), 32L*rows);
  if (!in) throw std::runtime_error("short read");
  return out;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: verify_counts SLICE_PGS COUNTS_NPY\n";
    return 1;
  }
  try {
    slice_reader_t reader(argv[1], (uint64_t)8<<30);
    const auto rows = read_counts(argv[2]);
    const bool turn = reader.slice() & 1;
    if ((int)rows.size() != (int)reader.section_id.size()) {
      std::cerr << "section count mismatch: file " << reader.section_id.size()
                << " vs counts " << rows.size() << "\n";
      return 1;
    }

    long bad = 0;
    Vector<uint64_t,3> grand(0,0,0), grand_want(0,0,0);
    for (size_t r=0;r<rows.size();r++) {
      const auto& row = rows[r];
      // find section by sig
      const auto it = reader.section_id.find(row.sig);
      if (it == reader.section_id.end()) {
        std::cerr << "missing section sig " << row.sig << "\n";
        bad++; continue;
      }
      const int s = it->second;
      const auto& e = reader.entry(s);
      const Vector<int,4> shape(e.shape[0], e.shape[1], e.shape[2], e.shape[3]);

      // Reconstruct section_t for rmin lookups
      section_t section;
      for (int q=0;q<4;q++)
        section.counts[q] = vec((uint8_t)(row.sig >> (16*q) & 0xff),
                                (uint8_t)(row.sig >> (16*q+8) & 0xff));
      const auto rmin = vec(get<0>(rotation_minimal_quadrants(section.counts[0])),
                            get<0>(rotation_minimal_quadrants(section.counts[1])),
                            get<0>(rotation_minimal_quadrants(section.counts[2])),
                            get<0>(rotation_minimal_quadrants(section.counts[3])));

      // Sum popcounts over all nodes
      Vector<uint64_t,3> sum(0,0,0);
      for (int x0=0;x0<shape[0];x0++) for (int x1=0;x1<shape[1];x1++)
      for (int x2=0;x2<shape[2];x2++) for (int x3=0;x3<shape[3];x3++) {
        const board_t board = quadrants(rmin[0][x0], rmin[1][x1], rmin[2][x2], rmin[3][x3]);
        const uint64_t node = ((x0*(uint64_t)shape[1] + x1)*shape[2] + x2)*shape[3] + x3;
        const auto val = reader.read_node(s, node);
        Vector<super_t,2> wins;
        if (!turn) { wins[0] = val[0]; wins[1] = ~val[1]; }
        else       { wins[1] = val[0]; wins[0] = ~val[1]; }
        const auto pc = popcounts_over_stabilizers(board, wins);
        sum += Vector<uint64_t,3>(pc[0], pc[1], pc[2]);
      }
      grand += sum;
      grand_want += Vector<uint64_t,3>(row.bw, row.ww, row.total);
      if (sum != Vector<uint64_t,3>(row.bw, row.ww, row.total)) {
        bad++;
        if (bad <= 5)
          std::cerr << "section sig " << row.sig << ": got (" << sum[0] << "," << sum[1]
                    << "," << sum[2] << ") want (" << row.bw << "," << row.ww << ","
                    << row.total << ")\n";
      }
    }
    std::cout << "slice " << reader.slice() << ": sections " << rows.size()
              << ", bad " << bad << "\n  grand total got  (" << grand[0] << "," << grand[1]
              << "," << grand[2] << ")\n  grand total want (" << grand_want[0] << ","
              << grand_want[1] << "," << grand_want[2] << ")\n";
    return bad ? 1 : 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
