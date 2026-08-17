/*
  Fanorona — state indexing for the retrograde tables.

  Stored form: every position is stored as *white to move* (a black-to-move
  position is color-swapped). The stored value is always the value for the
  side to move. This folds the state space exactly in half and makes layers
  unordered stone-count pairs {a, b}, a <= b.

  Index within a layer (plain mode): combinadic (colex) rank of the white
  stone set times C(N-a, b) plus the restricted colex rank of the black set
  among non-white points. Sub-layer 0 holds (#white=a, #black=b); sub-layer
  1 (when a != b) holds (#white=b, #black=a) at an offset.

  Symmetry-folded mode: positions are canonicalized to the orbit
  representative minimizing (first white stone point, white bits, black
  bits) over a symmetry group — D4 (8 transforms, square boards) or D2
  (4 transforms {id, rot180, flip-x, flip-y}, boards with both sides odd).
  The index space is the *superset* "first white stone lies in the
  fundamental domain" — every representative lands in it exactly once and
  the rank is directly computable; slots belonging to non-representatives
  are simply never used (~2x overhead, still ~4x smaller than plain).
  Layers {0, b} have no distinguished white stone and stay plain.
*/

#pragma once

#include <cstdint>
#include <vector>

#include "fanorona.h"

namespace fan {

extern uint64_t gBinom[kMaxPoints + 2][kMaxPoints + 2];
void InitBinom();

// Colex rank of a k-subset of points 0..N-1 (bitboard encoding).
uint64_t RankSet(uint64_t set, int k);
// Inverse of RankSet (n = number of points).
uint64_t UnrankSet(uint64_t rank, int k, int n);

// Rank of `set` (k elements) among the points NOT in `block` (the available
// points, in increasing point order).
uint64_t RankRestricted(uint64_t set, uint64_t block, int k);
uint64_t UnrankRestricted(uint64_t rank, uint64_t block, int k, int n);

struct Layer {
  int a = 0, b = 0;  // a <= b
  bool sym = false;  // symmetry-superset indexing active for this layer
  uint64_t sub_size[2] = {0, 0};
  uint64_t total = 0;
  // Symmetry mode: per-sub-layer offsets of "first white stone = m" blocks.
  uint64_t fd_off[2][kMaxPoints] = {};
};

class Indexer {
 public:
  const Geom& geom;

  // sym_mode: 0 = plain, 2 = D2 fold (both side lengths odd), 4 = D4 fold
  // (square boards). Throws when the requested group is not a symmetry of
  // the board.
  explicit Indexer(const Geom& g, int sym_mode) : geom(g) {
    InitSym(sym_mode);
    InitLayers();
  }
  explicit Indexer(const Geom& g, bool d4) : Indexer(g, d4 ? 4 : 0) {}

  int num_layers() const { return static_cast<int>(layers_.size()); }
  const Layer& layer(int id) const { return layers_[id]; }

  // Symmetry group in use (for orbit checks): nsym() transform ids,
  // including the identity at index 0.
  int nsym() const { return nsym_; }
  int sym_id(int i) const { return sym_ids_[i]; }

  // Layer id for the unordered pair.
  int LayerId(int wc, int bc) const {
    return layer_id_[std::min(wc, bc)][std::max(wc, bc)];
  }

  // Index of a stored-form (white-to-move) position. In D4 layers the
  // position must already be the canonical representative.
  uint64_t IndexOf(const Layer& l, uint64_t white, uint64_t black) const;

  // Board of a stored position (white to move). In D4 layers, indexes that
  // do not correspond to representatives are still decodable (they are just
  // never produced by canonicalization).
  void BoardOf(const Layer& l, uint64_t idx, uint64_t* white,
               uint64_t* black) const;

  // Fold an arbitrary actual position to stored form: color-swap when black
  // is to move, then canonicalize when a symmetry group is active.
  void Fold(const Pos& p, uint64_t* white, uint64_t* black) const {
    *white = p.white_to_move ? p.white : p.black;
    *black = p.white_to_move ? p.black : p.white;
    if (nsym_ > 1 && *white && *black) Canon(white, black);
  }

  // Canonical representative of (white, black); requires white != 0.
  void Canon(uint64_t* white, uint64_t* black) const;

  // True when (white, black) is the canonical representative of its orbit.
  bool IsRep(uint64_t white, uint64_t black) const;

  bool sym_active() const { return nsym_ > 1; }

 private:
  void InitSym(int sym_mode);
  void InitLayers();

  int nsym_ = 1;            // transforms in the active group (1 = plain)
  int sym_ids_[8] = {0};    // transform ids, identity first
  uint64_t fd_mask_ = 0;    // fundamental domain of the active group
  std::vector<Layer> layers_;
  int layer_id_[kMaxPoints + 1][kMaxPoints + 1] = {};
};

}  // namespace fan
