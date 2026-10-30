#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Minimal layered acyclic DFAs.  Zero is an implicit rejecting sink; one is
// the accepting empty suffix.  Nonterminal nodes never skip a variable.
// A manager shares equal suffix languages, including across different roots.
namespace twixt_dfa {

using Id = std::uint32_t;
using Children = std::array<Id, 3>;
using Word = std::vector<std::uint8_t>;

inline std::uint64_t hash64(std::uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

struct ChildrenHash {
  std::size_t operator()(const Children& c) const noexcept {
    return hash64((static_cast<std::uint64_t>(c[0]) << 32) | c[1]) ^
           hash64(c[2] + 0x9e3779b97f4a7c15ULL);
  }
};

struct Node {
  Children next{};
  std::uint32_t level = 0;
};

struct Limit : std::runtime_error {
  using std::runtime_error::runtime_error;
};

class Manager {
 public:
  explicit Manager(std::vector<std::uint8_t> alphabet,
                   std::size_t max_nodes = 5'000'000,
                   std::function<void()> check = {})
      : alphabet_(std::move(alphabet)), unique_(alphabet_.size()),
        max_nodes_(max_nodes), check_(std::move(check)) {
    if (alphabet_.empty()) throw std::invalid_argument("empty DFA schema");
    for (auto n : alphabet_) {
      if (n < 2 || n > 3) throw std::invalid_argument("bad DFA alphabet");
    }
    nodes_.push_back(Node{{}, length()});
    nodes_.push_back(Node{{}, length()});
  }

  std::uint32_t length() const { return static_cast<std::uint32_t>(alphabet_.size()); }
  std::size_t size() const { return nodes_.size(); }
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<std::uint8_t>& alphabet() const { return alphabet_; }
  const Node& node(Id id) const { return nodes_.at(id); }
  void checkpoint() {
    if (check_) check_();
  }

  Id intern(std::uint32_t level, const Children& children) {
    if (((++work_) & 0x3fffULL) == 0) checkpoint();
    if (children == Children{}) return 0;
    if (level >= length()) throw std::logic_error("DFA level overflow");
    const auto found = unique_[level].find(children);
    if (found != unique_[level].end()) return found->second;
    if ((max_nodes_ != 0 && nodes_.size() >= max_nodes_) ||
        nodes_.size() >= std::numeric_limits<Id>::max()) {
      throw Limit("DFA node pool cap reached (includes intermediate results)");
    }
    for (unsigned symbol = 0; symbol < 3; ++symbol) {
      const Id child = children[symbol];
      if (symbol >= alphabet_[level] && child != 0) {
        throw std::logic_error("DFA symbol outside alphabet");
      }
      if (child != 0 && (child >= nodes_.size() ||
                        nodes_[child].level != level + 1 ||
                        (child == 1 && level + 1 != length()))) {
        throw std::logic_error("DFA transition skips a level");
      }
    }
    const Id id = static_cast<Id>(nodes_.size());
    nodes_.push_back(Node{children, level});
    unique_[level].emplace(children, id);
    return id;
  }

  Id singleton(const Word& word) {
    if (word.size() != length()) throw std::invalid_argument("wrong DFA word length");
    Id result = 1;
    for (std::size_t i = word.size(); i-- > 0;) {
      if (word[i] >= alphabet_[i]) throw std::invalid_argument("bad DFA word symbol");
      Children children{};
      children[word[i]] = result;
      result = intern(static_cast<std::uint32_t>(i), children);
    }
    return result;
  }

  bool contains(Id root, const Word& word) const {
    if (word.size() != length()) return false;
    for (std::size_t i = 0; i < word.size(); ++i) {
      if (root < 2 || word[i] >= alphabet_[i]) return false;
      root = nodes_[root].next[word[i]];
    }
    return root == 1;
  }

  enum class Op { Union, Intersection, Difference };

  Id apply(Op op, Id a, Id b) {
    checkpoint();
    // IDs are stable until collection.  Reusing products across successive
    // move conditions avoids recomputing common suffix unions/differences.
    auto& memo = apply_cache_[static_cast<unsigned>(op)];
    if (memo.size() > 2'000'000) memo.clear();
    std::function<Id(Id, Id)> visit = [&](Id x, Id y) -> Id {
      if (op != Op::Difference && y < x) std::swap(x, y);
      if (op == Op::Union) {
        if (x == 0) return y;
        if (x == y) return x;
      } else if (op == Op::Intersection) {
        if (x == 0 || y == 0) return 0;
        if (x == y) return x;
      } else {
        if (x == 0 || x == y) return 0;
        if (y == 0) return x;
      }
      if (nodes_[x].level != nodes_[y].level || x < 2 || y < 2) {
        throw std::logic_error("incompatible DFA suffixes");
      }
      const std::uint64_t key = (static_cast<std::uint64_t>(x) << 32) | y;
      const auto found = memo.find(key);
      if (found != memo.end()) return found->second;
      // Interning can reallocate nodes_, so never hold references into it.
      const Node left = nodes_[x], right = nodes_[y];
      Children children{};
      for (unsigned c = 0; c < alphabet_[left.level]; ++c) {
        children[c] = visit(left.next[c], right.next[c]);
      }
      const Id result = intern(left.level, children);
      memo.emplace(key, result);
      return result;
    };
    return visit(a, b);
  }

  Id unite(Id a, Id b) { return apply(Op::Union, a, b); }
  Id intersect(Id a, Id b) { return apply(Op::Intersection, a, b); }
  Id difference(Id a, Id b) { return apply(Op::Difference, a, b); }

  // Filter by a conjunction of allowed symbols, then optionally rename one
  // symbol at one variable.  A rename requires the original symbol and drops
  // every other branch; it is an exact image, not an existential relaxation.
  Id filter(Id root, const Word& allowed, int variable = -1,
            unsigned before = 0, unsigned after = 0) {
    checkpoint();
    if (allowed.size() != length()) throw std::logic_error("bad DFA filter length");
    if (variable >= static_cast<int>(length()) ||
        (variable >= 0 && (before >= alphabet_[variable] || after >= alphabet_[variable]))) {
      throw std::logic_error("bad DFA rename");
    }
    int last = variable;
    for (unsigned i = 0; i < length(); ++i) {
      if (allowed[i] != ((1U << alphabet_[i]) - 1)) last = std::max(last, static_cast<int>(i));
    }
    // Reuse an epoch-tagged dense cache.  Filters revisit suffixes frequently;
    // allocating a hash entry for every visited node dominated small solves.
    const std::size_t input_size = nodes_.size();
    filter_values_.resize(input_size);
    filter_tags_.resize(input_size, 0);
    if (++filter_epoch_ == 0) {
      std::fill(filter_tags_.begin(), filter_tags_.end(), 0);
      filter_epoch_ = 1;
    }
    std::function<Id(Id)> visit = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (filter_tags_[id] == filter_epoch_) return filter_values_[id];
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((allowed[old.level] & (1U << c)) == 0) continue;
        if (static_cast<int>(old.level) == variable && c != before) continue;
        const unsigned out = static_cast<int>(old.level) == variable ? after : c;
        children[out] = visit(old.next[c]);
      }
      const Id result = intern(old.level, children);
      filter_tags_[id] = filter_epoch_;
      filter_values_[id] = result;
      return result;
    };
    return visit(root);
  }

  Word unrestricted() const {
    Word masks;
    for (auto n : alphabet_) masks.push_back(static_cast<std::uint8_t>((1U << n) - 1));
    return masks;
  }

  // Exact image/preimage of "old bit is zero; write predicate ? 1 : 0".
  // Split the suffix at the output variable, rather than constructing a pair
  // of whole-word languages followed by a whole-word union.  Predicate is a
  // conjunction and must not mention the bit being assigned.
  Id conditional_bit(Id root, const Word& predicate, unsigned variable, bool reverse) {
    checkpoint();
    if (variable >= length() || alphabet_[variable] != 2 ||
        predicate.size() != length() || predicate[variable] != 3) {
      throw std::logic_error("bad conditional DFA bit assignment");
    }
    filter_values_.resize(nodes_.size());
    filter_tags_.resize(nodes_.size(), 0);
    if (++filter_epoch_ == 0) {
      std::fill(filter_tags_.begin(), filter_tags_.end(), 0);
      filter_epoch_ = 1;
    }
    int last = -1;
    for (unsigned i = variable + 1; i < length(); ++i) {
      if (predicate[i] != ((1U << alphabet_[i]) - 1)) last = static_cast<int>(i);
    }
    std::function<Id(Id)> select = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (filter_tags_[id] == filter_epoch_) return filter_values_[id];
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((predicate[old.level] & (1U << c)) != 0) children[c] = select(old.next[c]);
      }
      const Id result = intern(old.level, children);
      filter_tags_[id] = filter_epoch_;
      filter_values_[id] = result;
      return result;
    };
    std::unordered_map<std::uint64_t, Id> memo;
    std::function<Id(Id, bool)> transform = [&](Id id, bool matches) -> Id {
      if (id == 0) return 0;
      if (id == 1 || nodes_[id].level > variable) throw std::logic_error("missing DFA assignment variable");
      const std::uint64_t key = (static_cast<std::uint64_t>(id) << 1) | matches;
      const auto found = memo.find(key);
      if (found != memo.end()) return found->second;
      const Node old = nodes_[id];
      Children children{};
      if (old.level == variable) {
        if (reverse) {
          const Id yes = matches ? select(old.next[1]) : 0;
          const Id selected_zero = matches ? select(old.next[0]) : 0;
          const Id no = difference(old.next[0], selected_zero);
          children[0] = unite(no, yes);
        } else {
          const Id yes = matches ? select(old.next[0]) : 0;
          children[0] = difference(old.next[0], yes);
          children[1] = yes;
        }
      } else {
        for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
          children[c] = transform(old.next[c], matches && (predicate[old.level] & (1U << c)) != 0);
        }
      }
      const Id result = intern(old.level, children);
      memo.emplace(key, result);
      return result;
    };
    return transform(root, true);
  }

  std::uint64_t count(Id root) const {
    std::vector<std::uint64_t> memo(nodes_.size(), std::numeric_limits<std::uint64_t>::max());
    std::function<std::uint64_t(Id)> visit = [&](Id id) -> std::uint64_t {
      if (id < 2) return id;
      if (memo[id] != std::numeric_limits<std::uint64_t>::max()) return memo[id];
      std::uint64_t result = 0;
      for (Id child : nodes_[id].next) {
        const std::uint64_t n = visit(child);
        if (n > std::numeric_limits<std::uint64_t>::max() - result) {
          throw std::overflow_error("represented position count exceeds uint64");
        }
        result += n;
      }
      memo[id] = result;
      return result;
    };
    return visit(root);
  }

  std::uint64_t reachable_nodes(Id root) const {
    if (root == 0) return 0;
    std::vector<std::uint8_t> seen(nodes_.size());
    std::uint64_t result = 0;
    std::function<void(Id)> visit = [&](Id id) {
      if (id == 0 || seen[id]) return;
      seen[id] = 1;
      ++result;
      if (id >= 2) for (Id child : nodes_[id].next) visit(child);
    };
    visit(root);
    return result;
  }

  // Keep only nodes accessible from the supplied live roots.  Copying in
  // postorder also gives a portable topological ordering for serialization.
  void collect(const std::vector<Id*>& roots) {
    Manager fresh(alphabet_, max_nodes_, check_);
    std::vector<std::uint8_t> live(nodes_.size());
    std::vector<Id> stack;
    auto mark = [&](Id id) {
      if (id < 2 || live[id]) return;
      live[id] = 1;
      stack.push_back(id);
    };
    for (Id* root : roots) mark(*root);
    std::vector<std::size_t> per_level(length());
    std::size_t live_count = 0;
    while (!stack.empty()) {
      const Id id = stack.back();
      stack.pop_back();
      ++per_level[nodes_[id].level];
      ++live_count;
      for (Id child : nodes_[id].next) mark(child);
      if ((live_count & 0x3fffULL) == 0) checkpoint();
    }
    fresh.nodes_.reserve(live_count + 2);
    for (unsigned i = 0; i < length(); ++i) fresh.unique_[i].reserve(per_level[i]);
    std::vector<Id> mapped(nodes_.size());
    mapped[1] = 1;
    // Interned IDs are already topological.  Renumbering a minimal DAG cannot
    // introduce new equivalent nodes, so rebuilding needs no minimization.
    for (std::size_t old_id = 2; old_id < nodes_.size(); ++old_id) {
      if (!live[old_id]) continue;
      const Node& old = nodes_[old_id];
      Children children{};
      for (unsigned c = 0; c < 3; ++c) children[c] = mapped[old.next[c]];
      const Id id = static_cast<Id>(fresh.nodes_.size());
      fresh.nodes_.push_back(Node{children, old.level});
      fresh.unique_[old.level].emplace(children, id);
      mapped[old_id] = id;
      if ((id & 0x3fffULL) == 0) checkpoint();
    }
    // Roots may alias.  Resolve all old IDs before changing any caller root.
    std::vector<Id> updated;
    updated.reserve(roots.size());
    for (Id* root : roots) updated.push_back(mapped[*root]);
    for (std::size_t i = 0; i < roots.size(); ++i) *roots[i] = updated[i];
    *this = std::move(fresh);
  }

 private:
  std::vector<std::uint8_t> alphabet_;
  std::vector<Node> nodes_;
  std::vector<std::unordered_map<Children, Id, ChildrenHash>> unique_;
  std::size_t max_nodes_;
  std::function<void()> check_;
  std::uint64_t work_ = 0;
  std::vector<Id> filter_values_;
  std::vector<std::uint32_t> filter_tags_;
  std::uint32_t filter_epoch_ = 0;
  std::array<std::unordered_map<std::uint64_t, Id>, 3> apply_cache_;
};

}  // namespace twixt_dfa
