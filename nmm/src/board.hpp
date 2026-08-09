// board.hpp — boards, mills, symmetries, and canonicalization for N Men's Morris family.
//
// Point naming: point_id = ring*8 + dir, dir in {NW=0,N=1,NE=2,E=3,SE=4,S=5,SW=6,W=7},
// ring 0 outermost. Boards with a center point put it at id = rings*8.
// Games: 3 (board a: 1 ring + center), 5/6 (b: 2 rings, orthogonal spokes),
// 7 (c: b + center), 9 (d: 3 rings, orthogonal spokes), 11/12 (e: 3 rings, all 8 spokes),
// 16 (f: 4 rings, all 8 spokes; custom variant).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

struct GameSpec {
  int id;               // 3,5,6,7,9,11,12,16
  char boardType;       // 'a'..'f'
  int rings;            // 8-point rings
  bool center;
  bool allSpokes;       // spokes at all 8 dirs (else odd dirs only: N,E,S,W)
  int pieces;           // per color
  bool flying;
  bool fullBoardDraw;   // placement fills board => draw (12MM, 16MM)
  int expSyms, expMills, expEdges;
  int sevenMillVariant; // 7MM only: 0/1/2 candidate center-mill sets
};

struct Board {
  GameSpec spec;
  int m = 0;                                  // number of points
  std::vector<u32> adj;                       // adjacency mask per point
  std::vector<u32> millMask;                  // one mask per mill (3 bits set)
  std::vector<std::array<u8, 3>> millPts;
  std::vector<u32> millsOfPoint;              // mask over mill indices, per point
  std::vector<std::array<u8, 32>> perms;      // symmetry permutations (identity first)
  // mill-hypergraph-preserving permutations (superset of perms). Used for uniqueness in
  // 3-3 subsets of flying games: with both players flying, adjacency is permanently
  // irrelevant, so these are value-preserving there (and reproduce the paper/Gasser).
  std::vector<std::array<u8, 32>> millPerms;
  std::vector<std::string> names;             // human-readable point names
  // byte LUTs: permLut[s][byteIdx][byteVal] -> permuted mask bits
  std::vector<std::array<std::array<u32, 256>, 4>> permLut;
  std::vector<std::array<std::array<u32, 256>, 4>> millPermLut;
  std::array<std::array<u64, 256>, 4> codeLutW{}, codeLutB{};   // identity interleave LUTs

  u32 permuteMaskMill(int s, u32 mask) const {
    const auto& L = millPermLut[s];
    return L[0][mask & 255] | L[1][(mask >> 8) & 255] |
           L[2][(mask >> 16) & 255] | L[3][(mask >> 24) & 255];
  }
  u32 permuteMask(int s, u32 mask) const {
    const auto& L = permLut[s];
    return L[0][mask & 255] | L[1][(mask >> 8) & 255] |
           L[2][(mask >> 16) & 255] | L[3][(mask >> 24) & 255];
  }
  // Interleaved code: point order = id order; per point, white bit then black bit,
  // point 0 most significant. Rank order of ZDD1 equals numeric order of this code.
  u64 code(u32 w, u32 b) const {
    return codeLutW[0][w & 255] | codeLutW[1][(w >> 8) & 255] |
           codeLutW[2][(w >> 16) & 255] | codeLutW[3][(w >> 24) & 255] |
           codeLutB[0][b & 255] | codeLutB[1][(b >> 8) & 255] |
           codeLutB[2][(b >> 16) & 255] | codeLutB[3][(b >> 24) & 255];
  }
  // canonical representative: minimal code over all symmetries; returns sym index used
  bool useMillGroup(int pw, int pb) const {
    return spec.flying && pw == 3 && pb == 3 && millPerms.size() > perms.size();
  }
  int canonicalize(u32& w, u32& b) const {
    u64 best = code(w, b);
    u32 bw = w, bb = b;
    int bs = 0;
    for (int s = 1; s < (int)perms.size(); ++s) {
      u32 w2 = permuteMask(s, w), b2 = permuteMask(s, b);
      u64 c = code(w2, b2);
      if (c < best) { best = c; bw = w2; bb = b2; bs = s; }
    }
    w = bw; b = bb;
    return bs;
  }
  // group-aware versions: mill group for 3-3 flying subsets, board group otherwise
  int canonicalizeAuto(u32& w, u32& b) const {
    if (!useMillGroup(__builtin_popcount(w), __builtin_popcount(b))) return canonicalize(w, b);
    u64 best = code(w, b);
    u32 bw = w, bb = b;
    int bs = 0;
    for (int s = 1; s < (int)millPerms.size(); ++s) {
      u32 w2 = permuteMaskMill(s, w), b2 = permuteMaskMill(s, b);
      u64 c = code(w2, b2);
      if (c < best) { best = c; bw = w2; bb = b2; bs = s; }
    }
    w = bw; b = bb;
    return bs;
  }
  bool isCanonical(u32 w, u32 b) const {
    u64 c0 = code(w, b);
    for (int s = 1; s < (int)perms.size(); ++s) {
      u32 w2 = permuteMask(s, w), b2 = permuteMask(s, b);
      if (code(w2, b2) < c0) return false;
    }
    return true;
  }
  // minimum number of mill-formation events implied by one color's mask
  // (= exact minimum hitting set over the complete mills of that mask)
  int minMillEvents(u32 mask) const;

  void writeConfigJson(const std::string& path) const;
  u64 boardHash() const;
};

GameSpec gameSpec(int id, int sevenVariant = 1);
Board buildBoard(const GameSpec& spec);
int countGraphAutomorphisms(const Board& bd);  // brute-force, for the diagnostic report
