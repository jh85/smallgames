// zdd1.hpp — the paper's first ZDD: minimal perfect hash over all non-overlapping
// two-color configurations with per-color piece-count bounds [lo, hi].
//
// Items: d = 0..2m-1 in point-major order; even d = "white piece on point d/2",
// odd d = "black piece on point d/2". Rank order equals numeric order of the interleaved
// code Board::code() (asserted by tests). Ranks are zero-based; count() is the number of
// accepted configurations; node counts reported for comparison with paper Table 11.
#pragma once
#include "board.hpp"
#include <vector>
#include <string>

struct Zdd1 {
  int m = 0, lo = 0, hi = 0;
  std::vector<u8> var_;       // item index per node (node 0/1 = leaves)
  std::vector<u32> lo_, hi_;
  std::vector<u64> cntlo_;    // count of accepting paths under the 0-child
  u32 root = 0;
  u64 total = 0;

  void build(int points, int loBound, int hiBound);
  u64 rank(u32 w, u32 b) const;                 // UINT64_MAX if not in family
  void unrank(u64 idx, u32& w, u32& b) const;
  u64 nodeCount() const { return var_.size(); }  // includes 2 leaves
  void save(const std::string& path, u64 boardHash) const;
  bool load(const std::string& path, u64 boardHash);
};

struct Zdd1Iter {
  const Zdd1* z = nullptr;
  int sp = 0;
  u32 nstack[80];
  u8 br[80];
  u32 w = 0, b = 0;
  void initAt(const Zdd1& zz, u64 idx);
  bool next();
};
