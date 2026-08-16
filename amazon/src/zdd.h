/*
  amazons — Zero-suppressed Decision Diagram (ZDD) manager and the
  position-set verdict database built on it.

  A position is encoded as a set of "on" variables: two variables per
  square, v0(sq) = 2*sq, v1(sq) = 2*sq+1, with square states
    empty = 00 (no variable present), white queen = 01 (v0),
    black queen = 10 (v1), burned = 11 (v0 and v1).
  A set of positions is then a family of sets, represented by a ZDD with
  the standard zero-suppression reduction (a node whose hi edge is the
  empty family is replaced by its lo child).  Positions along a game share
  most of their squares, so families of proven positions compress well.

  The VerdictDb stores two families — positions proven won and proven lost
  for the side to move — and serves the df-pn/BNS solver as an exact
  verdict store alongside the lossy hash transposition table.  Only
  canonical positions (Position::Canonical(), hence white-to-move) are
  stored, so no side-to-move variable is needed.
*/
#pragma once

#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace amazons {

class Position;

class ZddManager {
 public:
  using NodeId = uint32_t;
  static constexpr NodeId kEmpty = 0;  // the empty family {}
  static constexpr NodeId kUnit = 1;   // the family { {} }

  // Reduced node constructor (hash-consed).
  NodeId GetNode(uint32_t var, NodeId lo, NodeId hi);

  // The single set {vars}, vars sorted ascending.
  NodeId Singleton(const std::vector<uint32_t>& vars);

  NodeId Union(NodeId a, NodeId b);
  bool Contains(NodeId family, const std::vector<uint32_t>& vars) const;
  // Number of sets in the family (saturating at UINT64_MAX).
  uint64_t Count(NodeId family);

  size_t NodeCount() const { return nodes_.size() - 2; }
  size_t UnionCacheSize() const { return union_cache_.size(); }

  // Serialization: raw node table (terminals excluded from the count but
  // included in the image).  Caches are not saved; Load rebuilds the
  // unique table.  Returns false on malformed input.
  bool Save(FILE* f) const;
  bool Load(FILE* f);

 private:
  struct Node {
    uint32_t var;
    NodeId lo, hi;
  };
  struct NodeKey {
    uint32_t var;
    NodeId lo, hi;
    bool operator==(const NodeKey& o) const {
      return var == o.var && lo == o.lo && hi == o.hi;
    }
  };
  struct NodeKeyHash {
    size_t operator()(const NodeKey& k) const {
      uint64_t h = static_cast<uint64_t>(k.var) * 0x9E3779B97F4A7C15ull;
      h ^= static_cast<uint64_t>(k.lo) * 0xBF58476D1CE4E5B9ull;
      h ^= static_cast<uint64_t>(k.hi) * 0x94D049BB133111EBull;
      h ^= h >> 29;
      return static_cast<size_t>(h);
    }
  };

  NodeId UnionRec(NodeId a, NodeId b);

  std::vector<Node> nodes_ = {{0, 0, 0}, {0, 0, 0}};  // two terminals
  std::unordered_map<NodeKey, NodeId, NodeKeyHash> unique_;
  std::unordered_map<uint64_t, NodeId> union_cache_;
  std::unordered_map<NodeId, uint64_t> count_cache_;
};

// Exact store of proven verdicts: two ZDD families over canonical
// positions.  Verdicts are in the "side to move" frame of the stored
// (canonical, hence white-to-move) position.
class VerdictDb {
 public:
  // +1 = side to move wins, -1 = side to move loses.
  void InsertWin(const Position& canonical_pos);
  void InsertLoss(const Position& canonical_pos);
  // +1 win, -1 loss, 0 unknown.
  int Probe(const Position& canonical_pos) const;

  uint64_t num_wins() const { return num_wins_; }
  uint64_t num_losses() const { return num_losses_; }
  size_t zdd_nodes() const { return mgr_.NodeCount(); }
  uint64_t probes() const { return probes_; }
  uint64_t hits() const { return hits_; }

  // Persistent WDL table: saves/loads both families plus the board
  // dimensions the encoding depends on.  Load refuses a table built for
  // a different board size.  Tables only ever grow (verdicts are final),
  // so saving after every run accumulates a per-board-size database.
  bool Save(const char* path, int w, int h) const;
  bool Load(const char* path, int w, int h);

 private:
  // Sorted variable list encoding of a position (must be canonical).
  static std::vector<uint32_t> Encode(const Position& pos);

  ZddManager mgr_;
  ZddManager::NodeId wins_ = ZddManager::kEmpty;
  ZddManager::NodeId losses_ = ZddManager::kEmpty;
  uint64_t num_wins_ = 0, num_losses_ = 0;
  mutable uint64_t probes_ = 0, hits_ = 0;
};

}  // namespace amazons
