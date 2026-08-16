#include "bruteforce.h"

#include <array>
#include <functional>
#include <unordered_map>
#include <vector>

namespace amazons {

namespace {

struct CanonKey {
  uint64_t q0, q1, burned;
  bool operator==(const CanonKey& o) const {
    return q0 == o.q0 && q1 == o.q1 && burned == o.burned;
  }
};

struct CanonKeyHash {
  size_t operator()(const CanonKey& k) const {
    uint64_t h = k.q0 * 0x9E3779B97F4A7C15ull;
    h ^= k.q1 * 0xBF58476D1CE4E5B9ull;
    h ^= k.burned * 0x94D049BB133111EBull;
    h ^= h >> 29;
    return static_cast<size_t>(h);
  }
};

class BruteForce {
 public:
  bool Wins(const Position& pos, uint64_t* nodes) {
    nodes_ = nodes;
    return Solve(pos);
  }

 private:
  bool Solve(const Position& pos) {
    if (nodes_) (*nodes_)++;
    const Position c = pos.Canonical();
    const CanonKey key{c.queens[0], c.queens[1], c.burned};
    auto it = memo_.find(key);
    if (it != memo_.end()) return it->second;
    std::vector<Move> moves;
    pos.GenerateMoves(&moves);
    bool win = false;
    for (Move m : moves) {
      Position child = pos;
      child.DoMove(m);
      if (!Solve(child)) {
        win = true;
        break;
      }
    }
    memo_.emplace(key, win);
    return win;
  }

  std::unordered_map<CanonKey, bool, CanonKeyHash> memo_;
  uint64_t* nodes_ = nullptr;
};

}  // namespace

bool BruteForceStmWins(const Position& pos, uint64_t* nodes) {
  BruteForce bf;
  return bf.Wins(pos, nodes);
}

bool EnumerateOneQueenWdl(int w, int h, std::vector<WdlEntry>* out,
                          uint64_t position_limit) {
  std::unordered_map<CanonKey, bool, CanonKeyHash> memo;
  bool overflow = false;
  struct overflow_tag {};

  // Plain DFS with memo; recursion depth is bounded by the number of
  // squares (one burn per move).  On overflow the unwind skips the
  // memo-store of every unfinished node, so no partial values leak.
  std::function<bool(const Position&)> solve = [&](const Position& pos) {
    const Position c = pos.Canonical();
    const CanonKey key{c.queens[0], c.queens[1], c.burned};
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    if (memo.size() >= position_limit) throw overflow_tag{};
    std::vector<Move> moves;
    pos.GenerateMoves(&moves);
    bool win = false;
    for (Move m : moves) {
      Position child = pos;
      child.DoMove(m);
      if (!solve(child)) {
        win = true;
        break;
      }
    }
    memo.emplace(key, win);
    return win;
  };

  try {
    for (int i = 0; i < w * h && !overflow; i++)
      for (int j = 0; j < w * h && !overflow; j++) {
        if (i == j) continue;
        solve(Position::OneQueen(w, h, i % w, i / w, j % w, j / w));
      }
  } catch (overflow_tag&) {
    overflow = true;
  }

  out->reserve(out->size() + memo.size());
  for (const auto& [k, v] : memo)
    out->push_back(WdlEntry{k.q0, k.q1, k.burned, v});
  return !overflow;
}

}  // namespace amazons
