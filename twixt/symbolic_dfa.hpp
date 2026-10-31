#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
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

// Stable addresses let workers traverse old nodes while other workers append.
// Blocks are allocated lazily; publication through an intern bucket's mutex
// happens after the node has been written. GC runs only after workers join.
class NodeStore {
 public:
  static constexpr unsigned kBlockBits = 16;
  static constexpr std::size_t kBlockSize = 1ULL << kBlockBits;
  static constexpr std::size_t kBlocks = 1ULL << (32 - kBlockBits);
  NodeStore() : blocks_(new std::atomic<Node*>[kBlocks]) {
    for (std::size_t i = 0; i < kBlocks; ++i) blocks_[i].store(nullptr, std::memory_order_relaxed);
  }
  ~NodeStore() { clear(); }
  NodeStore(const NodeStore&) = delete;
  NodeStore& operator=(const NodeStore&) = delete;
  NodeStore(NodeStore&& other) noexcept : blocks_(std::move(other.blocks_)), size_(other.size()) {
    other.size_.store(0);
  }
  NodeStore& operator=(NodeStore&& other) noexcept {
    if (this != &other) {
      clear();
      blocks_ = std::move(other.blocks_);
      size_.store(other.size()); other.size_.store(0);
    }
    return *this;
  }
  std::size_t size() const { return size_.load(std::memory_order_relaxed); }
  const Node& operator[](std::size_t id) const {
    return blocks_[id >> kBlockBits].load(std::memory_order_acquire)[id & (kBlockSize - 1)];
  }
  const Node& at(std::size_t id) const {
    if (id >= size()) throw std::out_of_range("DFA node ID");
    return (*this)[id];
  }
  void reserve(std::size_t) {}  // stable blocks need no vector reallocation
  Id append(const Node& node, std::size_t cap = std::numeric_limits<Id>::max()) {
    const auto range = claim(1, cap);
    store(range.first, node);
    return static_cast<Id>(range.first);
  }
  std::pair<std::size_t, std::size_t> claim(std::size_t count, std::size_t cap) {
    std::size_t id = size();
    for (;;) {
      if (id >= cap || id >= std::numeric_limits<Id>::max()) {
        throw Limit("DFA node pool cap reached (includes intermediate results)");
      }
      const std::size_t available = std::min(cap, static_cast<std::size_t>(std::numeric_limits<Id>::max())) - id;
      const std::size_t take = std::min(count, available);
      for (std::size_t block = id >> kBlockBits; block <= (id + take - 1) >> kBlockBits; ++block) {
        if (blocks_[block].load(std::memory_order_acquire) == nullptr) {
          std::lock_guard<std::mutex> guard(allocation_);
          if (blocks_[block].load(std::memory_order_relaxed) == nullptr) {
            blocks_[block].store(new Node[kBlockSize], std::memory_order_release);
          }
        }
      }
      if (size_.compare_exchange_weak(id, id + take, std::memory_order_relaxed)) return {id, id + take};
    }
  }
  void store(std::size_t id, const Node& node) {
    const std::size_t block = id >> kBlockBits;
    if (blocks_[block].load(std::memory_order_acquire) == nullptr) {
      std::lock_guard<std::mutex> guard(allocation_);
      if (blocks_[block].load(std::memory_order_relaxed) == nullptr) {
        blocks_[block].store(new Node[kBlockSize], std::memory_order_release);
      }
    }
    blocks_[id >> kBlockBits].load(std::memory_order_relaxed)[id & (kBlockSize - 1)] = node;
  }
  void gc_slots(std::size_t count) {
    if (size() != 2 || count < 2 || count >= std::numeric_limits<Id>::max()) {
      throw std::logic_error("invalid compacted node reservation");
    }
    size_.store(count, std::memory_order_relaxed);
  }
  void push_back(const Node& node) { (void)append(node); }
 private:
  void clear() noexcept {
    if (!blocks_) return;
    for (std::size_t i = 0; i < kBlocks; ++i) delete[] blocks_[i].load(std::memory_order_relaxed);
  }
  std::unique_ptr<std::atomic<Node*>[]> blocks_;
  std::atomic<std::size_t> size_{0};
  std::mutex allocation_;
};

inline thread_local const void* worker_manager = nullptr;
inline thread_local unsigned worker_slot = 0;

class Manager {
  static constexpr unsigned kShards = 64;
  struct Bucket {
    std::mutex mutex;
    std::unordered_map<Children, Id, ChildrenHash> nodes;
  };
  struct Level { std::array<Bucket, kShards> buckets; };
  struct Context {
    std::uint64_t work = 0;
    std::vector<Id> filter_values;
    std::vector<std::uint32_t> filter_tags;
    std::uint32_t filter_epoch = 0;
    std::array<std::unordered_map<std::uint64_t, Id>, 3> apply_cache;
    std::size_t next_node = 0, end_node = 0;
  };
  struct ProductBucket {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, Id> values;
  };
  using Products = std::array<ProductBucket, 256>;
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
    for (auto& level : unique_) level = std::make_unique<Level>();
    contexts_.push_back(std::make_unique<Context>());
    nodes_.push_back(Node{{}, length()});
    nodes_.push_back(Node{{}, length()});
  }

  std::uint32_t length() const { return static_cast<std::uint32_t>(alphabet_.size()); }
  std::size_t size() const { return nodes_.size(); }
  const NodeStore& nodes() const { return nodes_; }
  const std::vector<std::uint8_t>& alphabet() const { return alphabet_; }
  const Node& node(Id id) const { return nodes_.at(id); }
  void checkpoint() {
    // The coordinator reports its own time/memory errors. Only workers throw
    // cancellation, so a racing checkpoint cannot hide the original failure.
    if (concurrent_ && worker_manager == this && worker_stop_->load(std::memory_order_relaxed)) {
      throw Limit("parallel workers cancelled");
    }
    if (check_) check_();
  }

  void prepare_workers(unsigned count) {
    while (contexts_.size() <= count) contexts_.push_back(std::make_unique<Context>());
    for (auto& product : shared_products_) if (!product) product = std::make_unique<Products>();
    worker_stop_->store(false, std::memory_order_relaxed);
    concurrent_ = true;
  }
  void finish_workers() { concurrent_ = false; worker_stop_->store(false, std::memory_order_relaxed); }
  void request_worker_stop() { worker_stop_->store(true, std::memory_order_relaxed); }
  bool worker_stopping() const { return worker_stop_->load(std::memory_order_relaxed); }
  // Call inside the owning worker after its last operation. Results contain
  // node IDs, so no returned language depends on these scratch arrays. Release
  // them before slow jobs, union operations, or copying GC raise the next peak.
  void release_worker_filter_buffers() noexcept {
    auto& ctx = context();
    std::vector<Id>().swap(ctx.filter_values);
    std::vector<std::uint32_t>().swap(ctx.filter_tags);
    ctx.filter_epoch = 0;
  }
  // Inspect only while workers are quiescent (for diagnostics and tests).
  std::size_t filter_cache_bytes() const {
    std::size_t bytes = 0;
    for (const auto& ctx : contexts_) {
      bytes += ctx->filter_values.capacity() * sizeof(Id) +
               ctx->filter_tags.capacity() * sizeof(std::uint32_t);
    }
    return bytes;
  }
  class WorkerScope {
   public:
    WorkerScope(Manager& manager, unsigned slot) : old_owner_(worker_manager), old_slot_(worker_slot) {
      worker_manager = &manager; worker_slot = slot;
    }
    ~WorkerScope() { worker_manager = old_owner_; worker_slot = old_slot_; }
   private:
    const void* old_owner_;
    unsigned old_slot_;
  };

  Id intern(std::uint32_t level, const Children& children) {
    if (((++context().work) & 0x3fffULL) == 0) checkpoint();
    if (children == Children{}) return 0;
    if (level >= length()) throw std::logic_error("DFA level overflow");
    auto& bucket = unique_[level]->buckets[ChildrenHash{}(children) % kShards];
    std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
    if (concurrent_) guard.lock();
    const auto found = bucket.nodes.find(children);
    if (found != bucket.nodes.end()) return found->second;
    if (!concurrent_ && ((max_nodes_ != 0 && nodes_.size() >= max_nodes_) ||
        nodes_.size() >= std::numeric_limits<Id>::max())) {
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
    const std::size_t cap = max_nodes_ == 0 ? std::numeric_limits<Id>::max() : max_nodes_;
    Id id = 0;
    if (concurrent_) {
      auto& ctx = context();
      const Id latest_child = std::max({children[0], children[1], children[2]});
      // Reserve a few IDs per worker rather than bouncing a shared counter
      // for every node. Discard a stale range when another worker supplied a
      // later child: every published child must still precede its parent.
      if (ctx.next_node >= ctx.end_node || ctx.next_node <= latest_child) {
        const auto range = nodes_.claim(64, cap);
        ctx.next_node = range.first; ctx.end_node = range.second;
      }
      id = static_cast<Id>(ctx.next_node++);
      nodes_.store(id, Node{children, level});
    } else {
      id = nodes_.append(Node{children, level}, cap);
    }
    bucket.nodes.emplace(children, id);
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
    auto& memo = context().apply_cache[static_cast<unsigned>(op)];
    Products* shared = shared_products_[static_cast<unsigned>(op)].get();
    if (memo.size() > (concurrent_ ? 32'768U : 2'000'000U)) memo.clear();
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
      if (shared != nullptr) {
        auto& bucket = (*shared)[hash64(key) % shared->size()];
        std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
        if (concurrent_) guard.lock();
        const auto known = bucket.values.find(key);
        if (known != bucket.values.end()) {
          memo.emplace(key, known->second);
          return known->second;
        }
      }
      // Interning can reallocate nodes_, so never hold references into it.
      const Node left = nodes_[x], right = nodes_[y];
      Children children{};
      for (unsigned c = 0; c < alphabet_[left.level]; ++c) {
        children[c] = visit(left.next[c], right.next[c]);
      }
      const Id result = intern(left.level, children);
      memo.emplace(key, result);
      if (shared != nullptr) {
        auto& bucket = (*shared)[hash64(key) % shared->size()];
        std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
        if (concurrent_) guard.lock();
        if (bucket.values.size() < 4'096) bucket.values.emplace(key, result);
      }
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
    auto& ctx = context();
    const std::size_t input_size = static_cast<std::size_t>(root) + 2;
    ctx.filter_values.resize(input_size);
    ctx.filter_tags.resize(input_size, 0);
    if (++ctx.filter_epoch == 0) {
      std::fill(ctx.filter_tags.begin(), ctx.filter_tags.end(), 0);
      ctx.filter_epoch = 1;
    }
    std::function<Id(Id)> visit = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (ctx.filter_tags[id] == ctx.filter_epoch) return ctx.filter_values[id];
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((allowed[old.level] & (1U << c)) == 0) continue;
        if (static_cast<int>(old.level) == variable && c != before) continue;
        const unsigned out = static_cast<int>(old.level) == variable ? after : c;
        children[out] = visit(old.next[c]);
      }
      const Id result = intern(old.level, children);
      ctx.filter_tags[id] = ctx.filter_epoch;
      ctx.filter_values[id] = result;
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
    auto& ctx = context();
    ctx.filter_values.resize(static_cast<std::size_t>(root) + 2);
    ctx.filter_tags.resize(static_cast<std::size_t>(root) + 2, 0);
    if (++ctx.filter_epoch == 0) {
      std::fill(ctx.filter_tags.begin(), ctx.filter_tags.end(), 0);
      ctx.filter_epoch = 1;
    }
    int last = -1;
    for (unsigned i = variable + 1; i < length(); ++i) {
      if (predicate[i] != ((1U << alphabet_[i]) - 1)) last = static_cast<int>(i);
    }
    std::function<Id(Id)> select = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (ctx.filter_tags[id] == ctx.filter_epoch) return ctx.filter_values[id];
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((predicate[old.level] & (1U << c)) != 0) children[c] = select(old.next[c]);
      }
      const Id result = intern(old.level, children);
      ctx.filter_tags[id] = ctx.filter_epoch;
      ctx.filter_values[id] = result;
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
  void collect(const std::vector<Id*>& roots, unsigned workers = 1) {
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
    for (unsigned i = 0; i < length(); ++i) {
      for (auto& bucket : fresh.unique_[i]->buckets) bucket.nodes.reserve(per_level[i] / kShards);
    }
    std::vector<Id> mapped(nodes_.size());
    mapped[1] = 1;
    // Compute the complete renumbering first. Node copying/registry rebuilding
    // can then run independently, since every new child ID is already known.
    std::vector<Id> live_ids;
    live_ids.reserve(live_count);
    for (std::size_t old_id = 2; old_id < nodes_.size(); ++old_id) {
      if (!live[old_id]) continue;
      mapped[old_id] = static_cast<Id>(live_ids.size() + 2);
      live_ids.push_back(static_cast<Id>(old_id));
    }
    fresh.nodes_.gc_slots(live_count + 2);
    std::atomic<std::size_t> next_copy{0};
    std::atomic<bool> copy_stop{false};
    std::exception_ptr copy_error;
    std::mutex copy_error_mutex;
    auto copy_nodes = [&]() {
      try {
        while (!copy_stop.load(std::memory_order_relaxed)) {
          const std::size_t begin = next_copy.fetch_add(1024, std::memory_order_relaxed);
          if (begin >= live_ids.size()) break;
          checkpoint();
          for (std::size_t i = begin; i < std::min(begin + 1024, live_ids.size()); ++i) {
            const Node& old = nodes_[live_ids[i]];
            Children children{};
            for (unsigned c = 0; c < 3; ++c) children[c] = mapped[old.next[c]];
            const Id id = static_cast<Id>(i + 2);
            fresh.nodes_.store(id, Node{children, old.level});
            auto& bucket = fresh.unique_[old.level]->buckets[ChildrenHash{}(children) % kShards];
            std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
            if (workers > 1) guard.lock();
            bucket.nodes.emplace(children, id);
          }
        }
      } catch (...) {
        std::lock_guard<std::mutex> guard(copy_error_mutex);
        if (!copy_error) copy_error = std::current_exception();
        copy_stop.store(true, std::memory_order_relaxed);
      }
    };
    std::vector<std::thread> builders;
    if (workers > 1) {
      try {
        builders.reserve(workers - 1);
        for (unsigned i = 1; i < workers; ++i) builders.emplace_back(copy_nodes);
      } catch (...) {}  // complete with the threads that could be created
    }
    copy_nodes();
    for (auto& builder : builders) builder.join();
    if (copy_error) std::rethrow_exception(copy_error);
    // Roots may alias.  Resolve all old IDs before changing any caller root.
    std::vector<Id> updated;
    updated.reserve(roots.size());
    for (Id* root : roots) updated.push_back(mapped[*root]);
    for (std::size_t i = 0; i < roots.size(); ++i) *roots[i] = updated[i];
    // Freeing millions of hash entries allocated by many workers was a large
    // serial tail. Release independent registries/caches concurrently while
    // the newly compacted DAG is already complete and workers are quiescent.
    if (workers > 1) {
      auto old_unique = std::move(unique_);
      auto old_contexts = std::move(contexts_);
      auto old_products = std::move(shared_products_);
      const std::size_t tasks = old_unique.size() + old_contexts.size() + old_products.size();
      std::atomic<std::size_t> next{0};
      auto release = [&]() {
        for (;;) {
          std::size_t job = next.fetch_add(1, std::memory_order_relaxed);
          if (job >= tasks) break;
          if (job < old_unique.size()) old_unique[job].reset();
          else if ((job -= old_unique.size()) < old_contexts.size()) old_contexts[job].reset();
          else old_products[job - old_contexts.size()].reset();
        }
      };
      std::vector<std::thread> cleaners;
      try {
        cleaners.reserve(workers - 1);
        for (unsigned i = 1; i < workers; ++i) cleaners.emplace_back(release);
      } catch (...) {
        // Releasing memory must still complete if another OS thread cannot
        // be created. The caller's roots already point into the fresh DAG.
      }
      release();
      for (auto& cleaner : cleaners) cleaner.join();
    }
    *this = std::move(fresh);
  }

 private:
  std::vector<std::uint8_t> alphabet_;
  NodeStore nodes_;
  std::vector<std::unique_ptr<Level>> unique_;
  std::size_t max_nodes_;
  std::function<void()> check_;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::array<std::unique_ptr<Products>, 3> shared_products_;
  bool concurrent_ = false;
  std::shared_ptr<std::atomic<bool>> worker_stop_ = std::make_shared<std::atomic<bool>>(false);
  Context& context() {
    return *contexts_[worker_manager == this ? worker_slot : 0];
  }
};

}  // namespace twixt_dfa
