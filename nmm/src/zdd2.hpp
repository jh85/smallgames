// zdd2.hpp — the paper's second ZDD, generalized: compressed rank/select structures over
// the first-ZDD integer domain, built blockwise (Algorithm 3 with configurable block
// size). One shared node pool holds many "streams": the paper-exact global set (unique +
// pseudo-reachable) plus one set per piece-count subset (for partitioned solving).
#pragma once
#include "board.hpp"
#include "zdd1.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Pool2 {
  std::vector<u8> var_;
  std::vector<u32> lo_, hi_;
  std::vector<u64> cntlo_;
  std::vector<u32> next_;          // hash-chain links (build only)
  u32* buckets = nullptr;
  u64 nbuckets = 0;
  std::unique_ptr<std::mutex[]> locks;   // striped bucket locks (build only)
  static const u64 NSTRIPES = 1 << 16;
  std::atomic<u64> nnodes{2};
  void init(u64 maxNodes, int slotsLog);
  void releaseBuildStructures();
  u32 intern(u8 v, u32 lo, u32 hi);
  void finalizeCounts();                       // fills cntlo_, requires topological ids
  u64 countAt(u32 root) const;                 // accepting paths under root
  u64 reachableNodes(u32 root) const;          // incl. 2 leaves
};

// A "selection" over a Zdd1 domain: dense rank/unrank within the selected subset.
struct Sel {
  const Pool2* P = nullptr;
  int totBits = 0;
  u32 root = 0;
  u64 count = 0;
  bool contains(u64 rank1) const;
  u64 selRank(u64 rank1) const;                // dense rank within selection
  u64 selUnrank(u64 k) const;                  // -> rank1
};

// Forest: filter results for one game, phase-2/3 domain (paper) or placement domain.
struct Forest2 {
  const Board* bd = nullptr;
  const Zdd1* z1 = nullptr;
  Pool2 pool;
  int totBits = 0, nMin = 0, nMax = 0;
  bool paperFilter = false;                    // pseudo-reachability filter on?
  std::vector<u32> subsetRoot;                 // (w,b) -> root, index (w-nMin)*(nMax-nMin+1)+(b-nMin)
  std::vector<u64> subsetCount;
  u32 globalRoot = 0;                          // union of all subsets (paper's ZDD2)
  u64 globalCount = 0;

  int sidx(int w, int b) const { return (w - nMin) * (nMax - nMin + 1) + (b - nMin); }
  Sel sel(int w, int b) const {
    return {&pool, totBits, subsetRoot[sidx(w, b)], subsetCount[sidx(w, b)]};
  }
  void save(const std::string& path) const;
  bool load(const std::string& path, const Board& b, const Zdd1& z);
};

// Runs the enumeration sweep over z1's whole domain: keeps only canonical (lex-min code)
// configurations; if paperFilter, also applies the pseudo-reachability filter
// (opp_count + minMillEvents(own) <= board pieces). Tallies per ordered (w,b) and, if
// buildZdd is true, constructs the selection ZDDs.
struct SweepResult {
  std::vector<std::vector<u64>> tally;   // [w][b], canonical (+filtered) counts
  u64 totalAccepted = 0;
};
SweepResult sweepAndBuild(const Board& bd, const Zdd1& z1, int nMin, int nMax,
                          bool paperFilter, bool buildZdd, Forest2* out, int threads);
