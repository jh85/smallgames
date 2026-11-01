#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
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

// Node IDs are 64-bit values with a 40-bit ceiling (about 1.1e12 nodes).
// Nodes are stored packed in 16 bytes: three 40-bit children and the level.
using Id = std::uint64_t;
constexpr unsigned kIdBits = 40;
constexpr Id kMaxId = (Id{1} << kIdBits) - 1;  // IDs are strictly below this
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
    // Two independent mixes (not a dependent chain): c0 and c1 are 40-bit,
    // so folding c1 around c0 loses only collision-free spread, never
    // correctness (equality is always checked on the stored children).
    return hash64(c[0] ^ (c[1] << 40) ^ (c[1] >> 24)) ^ hash64(c[2] + 0x9e3779b97f4a7c15ULL);
  }
};

// Two node IDs as one memo key (product memos).  Never all zero for real
// keys, because memoized operands are nonterminal nodes (IDs of at least 2).
struct PairKey {
  std::uint64_t a = 0, b = 0;
  bool operator==(const PairKey& o) const { return a == o.a && b == o.b; }
  bool operator!=(const PairKey& o) const { return !(*this == o); }
};
inline std::uint64_t hash_key(std::uint64_t key) { return hash64(key); }
inline std::uint64_t hash_key(const PairKey& key) { return hash64(key.a ^ hash64(key.b + 0x9e3779b97f4a7c15ULL)); }
struct PairKeyHash {
  std::size_t operator()(const PairKey& key) const noexcept { return hash_key(key); }
};

// Five-byte little-endian ID helpers (40-bit IDs in packed arrays).  They
// use one unaligned 8-byte access, so every packed array keeps kPad bytes of
// tail padding; stores read-modify-write the 3 following bytes, which is
// safe because each packed array has a single writer at a time.
constexpr std::size_t kPad = 8;
constexpr std::uint64_t kId5Mask = (std::uint64_t{1} << 40) - 1;
inline __attribute__((always_inline)) Id load_id5(const std::uint8_t* p) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  return v & kId5Mask;
}
inline __attribute__((always_inline)) void store_id5(std::uint8_t* p, Id id) {
  std::uint64_t v;
  std::memcpy(&v, p, 8);
  v = (v & ~kId5Mask) | (id & kId5Mask);
  std::memcpy(p, &v, 8);
}

// Open-addressing map from nonzero 64-bit keys to 40-bit Id values (a zero
// key marks an empty slot): 8 + 5 = 13 bytes per slot.  Node-based maps
// spent 32-100 bytes per entry plus buckets.  Single-threaded; linear
// probing at load factor at most 0.7.
template <class Key>
class FlatTable {
 public:
  std::size_t size() const { return size_; }
  std::size_t bytes() const { return keys_.capacity() * sizeof(Key) + values_.capacity(); }
  // Returns true and sets value when key is present.
  bool find(const Key& key, Id& value) const {
    if (keys_.empty()) return false;
    for (std::size_t i = hash_key(key) & mask_;; i = (i + 1) & mask_) {
      if (keys_[i] == key) { value = load_id5(values_.data() + 5 * i); return true; }
      if (keys_[i] == Key{}) return false;
    }
  }
  // The key must be nonzero and absent; value must fit in 40 bits.
  void insert(const Key& key, Id value) {
    if ((size_ + 1) * 10 > keys_.size() * 7) grow();
    std::size_t i = hash_key(key) & mask_;
    while (keys_[i] != Key{}) i = (i + 1) & mask_;
    keys_[i] = key;
    store_id5(values_.data() + 5 * i, value);
    ++size_;
  }
  void release() {
    std::vector<Key>().swap(keys_);
    std::vector<std::uint8_t>().swap(values_);
    size_ = mask_ = 0;
  }
 private:
  void grow() {
    const std::size_t capacity = keys_.empty() ? 1024 : keys_.size() * 2;
    std::vector<Key> keys(capacity);
    std::vector<std::uint8_t> values(capacity * 5 + kPad);
    const std::size_t mask = capacity - 1;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
      if (keys_[i] == Key{}) continue;
      std::size_t j = hash_key(keys_[i]) & mask;
      while (keys[j] != Key{}) j = (j + 1) & mask;
      keys[j] = keys_[i];
      store_id5(values.data() + 5 * j, load_id5(values_.data() + 5 * i));
    }
    keys_.swap(keys);
    values_.swap(values);
    mask_ = mask;
  }
  std::vector<Key> keys_;
  std::vector<std::uint8_t> values_;
  std::size_t size_ = 0, mask_ = 0;
};
using FlatMap = FlatTable<std::uint64_t>;

// Product memo: operands x, y (40 bits each) and the result (40 bits) packed
// into one 16-byte slot; x >= 2 for every real key, so zero marks empty.
class PairMap {
 public:
  std::size_t size() const { return size_; }
  std::size_t bytes() const { return slots_.capacity() * sizeof(Slot); }
  // Returns true and sets value when (x, y) is present.
  bool find(const PairKey& key, Id& value) const {
    if (slots_.empty()) return false;
    const Slot want = pack(key.a, key.b, 0);
    for (std::size_t i = hash_key(key) & mask_;; i = (i + 1) & mask_) {
      const Slot& slot = slots_[i];
      if (slot.lo == 0 && slot.hi == 0) return false;
      if (slot.lo == want.lo && (slot.hi & 0xffffULL) == want.hi) {
        value = slot.hi >> 16;
        return true;
      }
    }
  }
  void insert(const PairKey& key, Id value) {
    if ((size_ + 1) * 10 > slots_.size() * 7) grow();
    std::size_t i = hash_key(key) & mask_;
    while (slots_[i].lo != 0 || slots_[i].hi != 0) i = (i + 1) & mask_;
    slots_[i] = pack(key.a, key.b, value);
    ++size_;
  }
  void release() {
    std::vector<Slot>().swap(slots_);
    size_ = mask_ = 0;
  }
 private:
  struct Slot { std::uint64_t lo = 0, hi = 0; };
  static Slot pack(Id x, Id y, Id value) { return Slot{x | (y << 40), (y >> 24) | (value << 16)}; }
  void grow() {
    const std::size_t capacity = slots_.empty() ? 1024 : slots_.size() * 2;
    std::vector<Slot> slots(capacity);
    const std::size_t mask = capacity - 1;
    for (const Slot& slot : slots_) {
      if (slot.lo == 0 && slot.hi == 0) continue;
      const Id x = slot.lo & PackedNode_mask(), y = (slot.lo >> 40) | ((slot.hi & 0xffffULL) << 24);
      std::size_t j = hash_key(PairKey{x, y}) & mask;
      while (slots[j].lo != 0 || slots[j].hi != 0) j = (j + 1) & mask;
      slots[j] = slot;
    }
    slots_.swap(slots);
    mask_ = mask;
  }
  static constexpr Id PackedNode_mask() { return (Id{1} << kIdBits) - 1; }
  std::vector<Slot> slots_;
  std::size_t size_ = 0, mask_ = 0;
};

// Membership bitmap over an ID range [base, base + count).
class IdBitmap {
 public:
  IdBitmap(Id base, std::size_t count) : base_(base), bits_((count + 63) / 64) {}
  bool test(Id id) const { const std::size_t i = id - base_; return (bits_[i >> 6] >> (i & 63)) & 1ULL; }
  // Returns true when the bit was newly set.
  bool set(Id id) {
    const std::size_t i = id - base_;
    const std::uint64_t bit = 1ULL << (i & 63);
    if (bits_[i >> 6] & bit) return false;
    bits_[i >> 6] |= bit;
    return true;
  }
  // set() for concurrent marking; every access in that phase must use it.
  bool set_atomic(Id id) {
    const std::size_t i = id - base_;
    const std::uint64_t bit = 1ULL << (i & 63);
    if (__atomic_load_n(&bits_[i >> 6], __ATOMIC_RELAXED) & bit) return false;
    return (__atomic_fetch_or(&bits_[i >> 6], bit, __ATOMIC_RELAXED) & bit) == 0;
  }
  std::size_t words() const { return bits_.size(); }
  std::uint64_t word(std::size_t w) const { return bits_[w]; }
  Id base() const { return base_; }
 private:
  Id base_;
  std::vector<std::uint64_t> bits_;
};

// Rank of set bits: the k-th set ID (in increasing order) has rank k.
class IdRank {
 public:
  explicit IdRank(const IdBitmap& bits) : bits_(&bits), prefix_(bits.words() + 1) {
    for (std::size_t w = 0; w < bits.words(); ++w) prefix_[w + 1] = prefix_[w] + __builtin_popcountll(bits.word(w));
  }
  std::uint64_t count() const { return prefix_.back(); }
  std::uint64_t rank(Id id) const {
    const std::size_t i = id - bits_->base();
    return prefix_[i >> 6] + __builtin_popcountll(bits_->word(i >> 6) & ((1ULL << (i & 63)) - 1));
  }
  std::uint64_t prefix(std::size_t word) const { return prefix_[word]; }
 private:
  const IdBitmap* bits_;
  std::vector<std::uint64_t> prefix_;
};

// Unpacked node value.  The store keeps nodes packed (PackedNode) and returns
// them by value, so a reference into the store can never dangle.
struct Node {
  Children next{};
  std::uint32_t level = 0;
};

#define TWIXT_INLINE inline __attribute__((always_inline))
struct PackedNode {
  std::uint64_t lo = 0, hi = 0;
  static constexpr std::uint64_t kMask = (std::uint64_t{1} << kIdBits) - 1;
  TWIXT_INLINE unsigned level() const { return static_cast<unsigned>(hi >> 56); }
  TWIXT_INLINE bool operator==(const PackedNode& o) const { return lo == o.lo && hi == o.hi; }
  TWIXT_INLINE static PackedNode pack(const Node& n) {
    if (n.level > 255 || n.next[0] > kMask || n.next[1] > kMask || n.next[2] > kMask) {
      throw std::logic_error("DFA node exceeds the packed 40-bit/8-bit layout");
    }
    return PackedNode{n.next[0] | (n.next[1] << 40),
                      (n.next[1] >> 24) | (n.next[2] << 16) | (static_cast<std::uint64_t>(n.level) << 56)};
  }
  TWIXT_INLINE Node unpack() const {
    Node n;
    n.next[0] = lo & kMask;
    n.next[1] = (lo >> 40) | ((hi & 0xffffULL) << 24);
    n.next[2] = (hi >> 16) & kMask;
    n.level = static_cast<std::uint32_t>(hi >> 56);
    return n;
  }
};
static_assert(sizeof(PackedNode) == 16, "packed DFA nodes must stay 16 bytes");

// Registry hash of a node, computed on its packed form (children and level),
// so probing and rehashing never unpack.  Two independent mixes.
TWIXT_INLINE std::uint64_t node_hash(const PackedNode& p) {
  return hash64(p.lo) ^ hash64(p.hi + 0x9e3779b97f4a7c15ULL);
}

struct Limit : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Diagnostic snapshot; take it only while workers are quiescent.
struct Census {
  std::size_t slots = 0, unused_slots = 0, blocks = 0;
  std::size_t unique_entries = 0, unique_buckets = 0, unique_bytes = 0;
  std::size_t filter_bytes = 0, filter_used_bytes = 0;
  std::array<std::size_t, 3> apply_entries{}, apply_buckets{};
  std::size_t product_entries = 0, product_buckets = 0;
  std::uint64_t created = 0, discarded = 0, reserved_tail = 0;
};

// Stable addresses let workers traverse old nodes while other workers append.
// Blocks are allocated lazily; publication through an intern bucket's mutex
// happens after the node has been written. GC runs only after workers join.
class NodeStore {
 public:
  static constexpr unsigned kBlockBits = 18;  // 4 MiB blocks
  static constexpr std::size_t kBlockSize = 1ULL << kBlockBits;
  static constexpr std::size_t kBlocks = 1ULL << (kIdBits - kBlockBits);
  NodeStore() : blocks_(new std::atomic<PackedNode*>[kBlocks]) {
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
  TWIXT_INLINE const PackedNode& packed(std::size_t id) const {
    return blocks_[id >> kBlockBits].load(std::memory_order_acquire)[id & (kBlockSize - 1)];
  }
  TWIXT_INLINE Node operator[](std::size_t id) const { return packed(id).unpack(); }
  TWIXT_INLINE unsigned level(std::size_t id) const { return packed(id).level(); }
  Node at(std::size_t id) const {
    if (id >= size()) throw std::out_of_range("DFA node ID");
    return (*this)[id];
  }
  Id append(const Node& node, std::size_t cap = kMaxId) {
    const auto range = claim(1, cap);
    store(range.first, node);
    return static_cast<Id>(range.first);
  }
  std::pair<std::size_t, std::size_t> claim(std::size_t count, std::size_t cap) {
    std::size_t id = size();
    for (;;) {
      if (id >= cap || id >= kMaxId) {
        throw Limit("DFA node pool cap reached (includes intermediate results)");
      }
      const std::size_t available = std::min<std::size_t>(cap, kMaxId) - id;
      const std::size_t take = std::min(count, available);
      for (std::size_t block = id >> kBlockBits; block <= (id + take - 1) >> kBlockBits; ++block) ensure(block);
      if (size_.compare_exchange_weak(id, id + take, std::memory_order_relaxed)) return {id, id + take};
    }
  }
  void store(std::size_t id, const Node& node) { store_packed(id, PackedNode::pack(node)); }
  void store_packed(std::size_t id, const PackedNode& node) {
    const std::size_t block = id >> kBlockBits;
    ensure(block);
    blocks_[block].load(std::memory_order_relaxed)[id & (kBlockSize - 1)] = node;
  }
  // Make [size, first) unused (test ID bases); blocks there are never allocated.
  void skip_to(std::size_t first) {
    if (first >= kMaxId) throw std::logic_error("DFA ID base beyond the 40-bit ceiling");
    if (first > size()) size_.store(first, std::memory_order_relaxed);
  }
  // A compacted store holds terminals plus [first, first + count).
  void reserve_compact(std::size_t first, std::size_t count) {
    if (size() != first || first + count >= kMaxId) throw std::logic_error("invalid compacted node reservation");
    size_.store(first + count, std::memory_order_relaxed);
  }
  void push_back(const Node& node) { (void)append(node); }
  std::size_t allocated_blocks() const {
    std::size_t count = 0;
    for (std::size_t i = 0; i < kBlocks; ++i) count += blocks_[i].load(std::memory_order_relaxed) != nullptr;
    return count;
  }
 private:
  void ensure(std::size_t block) {
    if (blocks_[block].load(std::memory_order_acquire) != nullptr) return;
    std::lock_guard<std::mutex> guard(allocation_);
    if (blocks_[block].load(std::memory_order_relaxed) == nullptr) {
      blocks_[block].store(new PackedNode[kBlockSize], std::memory_order_release);
    }
  }
  void clear() noexcept {
    if (!blocks_) return;
    for (std::size_t i = 0; i < kBlocks; ++i) delete[] blocks_[i].load(std::memory_order_relaxed);
  }
  std::unique_ptr<std::atomic<PackedNode*>[]> blocks_;
  std::atomic<std::size_t> size_{0};
  std::mutex allocation_;
};

// Exact memo for variable-length ID tuples: each tuple stored once in an
// arena (5 bytes per 40-bit ID), a 16-byte entry (arena offset, value, hash
// tag; the length is implied by the next entry's offset), and a 4-byte
// open-addressing index slot.  Single-threaded.
class TupleMap {
 public:
  std::size_t size() const { return entries_.size(); }
  std::size_t bytes() const {
    return slots_.capacity() * sizeof(std::uint32_t) + entries_.capacity() * sizeof(Entry) + arena_.capacity();
  }
  static std::uint64_t hash(const std::vector<Id>& ids) {
    std::uint64_t h = ids.size();
    for (Id id : ids) h = hash64(h ^ id);
    return h;
  }
  bool find(const std::vector<Id>& ids, std::uint64_t h, Id& value) const {
    if (slots_.empty()) return false;
    for (std::size_t i = (h >> 16) & mask_;; i = (i + 1) & mask_) {
      const std::uint32_t slot = slots_[i];
      if (slot == 0) return false;
      if (matches(slot - 1, ids, h)) { value = entries_[slot - 1].value(); return true; }
    }
  }
  // The tuple must be absent.
  void insert(const std::vector<Id>& ids, std::uint64_t h, Id value) {
    if (entries_.size() >= std::numeric_limits<std::uint32_t>::max() - 1) {
      throw Limit("n-ary union memo overflow");
    }
    if ((entries_.size() + 1) * 10 > slots_.size() * 7) rehash();
    entries_.push_back(Entry::make(used_ / 5, value, static_cast<std::uint32_t>(h)));
    const std::size_t at = used_;
    used_ += 5 * ids.size();
    if (arena_.size() < used_ + kPad) arena_.resize(std::max(used_ + kPad, 2 * arena_.size()));
    for (std::size_t k = 0; k < ids.size(); ++k) store_id5(arena_.data() + at + 5 * k, ids[k]);
    place(static_cast<std::uint32_t>(entries_.size()), h);
  }
 private:
  // offset (ID index into the arena) and value, 40 bits each, plus a tag.
  struct Entry {
    std::uint64_t packed;  // offset | value << 40 (value high bits in `rest`)
    std::uint32_t rest;    // value >> 24
    std::uint32_t tag;
    static Entry make(std::uint64_t offset, Id value, std::uint32_t tag) {
      if (offset >= (std::uint64_t{1} << 40)) throw Limit("n-ary union memo arena overflow");
      return Entry{offset | (value << 40), static_cast<std::uint32_t>(value >> 24), tag};
    }
    std::uint64_t offset() const { return packed & ((std::uint64_t{1} << 40) - 1); }
    Id value() const { return (packed >> 40) | (static_cast<Id>(rest) << 24); }
  };
  std::size_t length(std::size_t entry) const {
    const std::size_t end = entry + 1 < entries_.size() ? entries_[entry + 1].offset() : used_ / 5;
    return end - entries_[entry].offset();
  }
  bool matches(std::size_t entry, const std::vector<Id>& ids, std::uint64_t h) const {
    if (entries_[entry].tag != static_cast<std::uint32_t>(h) || length(entry) != ids.size()) return false;
    const std::uint8_t* p = arena_.data() + 5 * entries_[entry].offset();
    for (std::size_t k = 0; k < ids.size(); ++k) if (load_id5(p + 5 * k) != ids[k]) return false;
    return true;
  }
  void place(std::uint32_t slot, std::uint64_t h) {
    std::size_t i = (h >> 16) & mask_;
    while (slots_[i] != 0) i = (i + 1) & mask_;
    slots_[i] = slot;
  }
  void rehash() {
    const std::size_t capacity = slots_.empty() ? 1024 : slots_.size() * 2;
    std::vector<std::uint32_t>(capacity).swap(slots_);
    mask_ = capacity - 1;
    std::vector<Id> ids;
    for (std::size_t e = 0; e < entries_.size(); ++e) {
      ids.resize(length(e));
      const std::uint8_t* p = arena_.data() + 5 * entries_[e].offset();
      for (std::size_t k = 0; k < ids.size(); ++k) ids[k] = load_id5(p + 5 * k);
      place(static_cast<std::uint32_t>(e + 1), hash(ids));
    }
  }
  std::vector<std::uint32_t> slots_;
  std::vector<Entry> entries_;
  std::vector<std::uint8_t> arena_;  // used_ bytes of IDs plus padding
  std::size_t used_ = 0, mask_ = 0;
};

// Intern registry for one (level, shard).  Open addressing over node IDs:
// the key (children) lives in the node store, so a slot holds just a 40-bit
// ID in five bytes, about 7-14 bytes per node at load 0.35-0.7.  A
// node-based hash map spent a 32-byte allocation per node plus 8-16 bytes of
// buckets.  Callers hold the shard mutex; every inserted node is written first.
class IdTable {
 public:
  static constexpr unsigned kSlotBytes = 5;
  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }
  std::size_t bytes() const { return slots_.capacity(); }
  Id find(const NodeStore& nodes, const PackedNode& want, std::uint64_t hash) const {
    if (capacity_ == 0) return 0;
    for (std::size_t i = (hash >> kShardBits) & mask_;; i = (i + 1) & mask_) {
      const Id id = get(i);
      if (id == 0 || nodes.packed(id) == want) return id;
    }
  }
  void insert(const NodeStore& nodes, Id id, std::uint64_t hash) {
    if ((size_ + 1) * 10 > capacity_ * 7) rehash(nodes, std::max<std::size_t>(16, capacity_ * 2));
    place(id, hash);
    ++size_;
  }
  void reserve(const NodeStore& nodes, std::size_t count) {
    std::size_t capacity = 16;
    while (capacity * 7 < count * 10) capacity *= 2;
    if (capacity > capacity_) rehash(nodes, capacity);
  }
  static constexpr unsigned kShardBits = 10;  // 1024 lock shards per level: level-synchronous replays intern one level from all threads
 private:
  TWIXT_INLINE Id get(std::size_t i) const { return load_id5(slots_.data() + kSlotBytes * i); }
  TWIXT_INLINE void put(std::size_t i, Id id) { store_id5(slots_.data() + kSlotBytes * i, id); }
  void place(Id id, std::uint64_t hash) {
    std::size_t i = (hash >> kShardBits) & mask_;
    while (get(i) != 0) i = (i + 1) & mask_;
    put(i, id);
  }
  void rehash(const NodeStore& nodes, std::size_t capacity) {
    std::vector<std::uint8_t> old(capacity * kSlotBytes + kPad);
    old.swap(slots_);
    const std::size_t old_capacity = capacity_;
    capacity_ = capacity;
    mask_ = capacity - 1;
    for (std::size_t i = 0; i < old_capacity; ++i) {
      const Id id = load_id5(old.data() + kSlotBytes * i);
      if (id != 0) place(id, node_hash(nodes.packed(id)));
    }
  }
  std::vector<std::uint8_t> slots_;
  std::size_t capacity_ = 0, size_ = 0, mask_ = 0;
};
static_assert(kIdBits == 8 * IdTable::kSlotBytes, "registry slots hold exactly one 40-bit node ID");

inline thread_local const void* worker_manager = nullptr;
inline thread_local unsigned worker_slot = 0;
// Memo composition of this thread's last unite_many() (diagnostics only).
inline thread_local std::size_t union_memo_pairs = 0, union_memo_trie = 0, union_memo_bytes = 0;

class Manager {
  static constexpr unsigned kShards = 1U << IdTable::kShardBits;
  struct Bucket {
    std::mutex mutex;
    IdTable nodes;
  };
  struct Level { std::array<Bucket, kShards> buckets; };
  struct Context {
    std::uint64_t work = 0;
    std::array<PairMap, 3> apply_cache;
    std::size_t next_node = 0, end_node = 0;
    std::uint64_t created = 0, discarded = 0;  // diagnostics only
  };
  struct ProductBucket {
    std::mutex mutex;
    std::unordered_map<PairKey, Id, PairKeyHash> values;
  };
  using Products = std::array<ProductBucket, 256>;
 public:
  // `base` is the first nonterminal ID (2 normally).  A large base is for
  // tests only: it makes every node ID exceed 32 bits on tiny workloads.
  explicit Manager(std::vector<std::uint8_t> alphabet,
                   std::size_t max_nodes = 5'000'000,
                   std::function<void()> check = {}, Id base = 2)
      : alphabet_(std::move(alphabet)), unique_(alphabet_.size()),
        max_nodes_(max_nodes), check_(std::move(check)), base_(base) {
    if (alphabet_.empty() || alphabet_.size() >= 255) throw std::invalid_argument("bad DFA schema length");
    for (auto n : alphabet_) {
      if (n < 2 || n > 3) throw std::invalid_argument("bad DFA alphabet");
    }
    if (base_ < 2) throw std::invalid_argument("DFA ID base below 2");
    for (auto& level : unique_) level = std::make_unique<Level>();
    contexts_.push_back(std::make_unique<Context>());
    nodes_.push_back(Node{{}, length()});
    nodes_.push_back(Node{{}, length()});
    nodes_.skip_to(base_);
  }

  std::uint32_t length() const { return static_cast<std::uint32_t>(alphabet_.size()); }
  std::size_t size() const { return nodes_.size(); }
  Id base() const { return base_; }
  const NodeStore& nodes() const { return nodes_; }
  const std::vector<std::uint8_t>& alphabet() const { return alphabet_; }
  TWIXT_INLINE Node node(Id id) const {
    if ((id >= 2 && id < base_) || id >= nodes_.size()) throw std::out_of_range("DFA node ID");
    return nodes_[id];
  }
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
    // Product memos are per-worker scratch; release() also frees the table.
    for (auto& memo : context().apply_cache) memo.release();
  }
  // Filters keep per-call sparse memos now; no scratch outlives a call.
  std::size_t filter_cache_bytes() const { return 0; }
  std::uint64_t created_here() { return context().created; }
  std::size_t filter_bytes_here() { return 0; }
  Census census(bool scan_unused) const {
    Census c;
    c.slots = nodes_.size();
    c.blocks = nodes_.allocated_blocks();
    if (scan_unused) {
      for (std::size_t id = base_; id < c.slots; ++id) c.unused_slots += nodes_[id].next == Children{};
    }
    for (const auto& level : unique_) {
      if (!level) continue;
      for (const auto& bucket : level->buckets) {
        c.unique_entries += bucket.nodes.size();
        c.unique_buckets += bucket.nodes.capacity();
        c.unique_bytes += bucket.nodes.bytes();
      }
    }
    for (const auto& ctx : contexts_) {
      for (unsigned op = 0; op < 3; ++op) {
        c.apply_entries[op] += ctx->apply_cache[op].size();
        c.apply_buckets[op] += ctx->apply_cache[op].bytes() / 16;
      }
      c.created += ctx->created;
      c.discarded += ctx->discarded;
      if (ctx->next_node < ctx->end_node) c.reserved_tail += ctx->end_node - ctx->next_node;
    }
    for (const auto& products : shared_products_) {
      if (!products) continue;
      for (const auto& bucket : *products) {
        c.product_entries += bucket.values.size();
        c.product_buckets += bucket.values.bucket_count();
      }
    }
    return c;
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
    const PackedNode want = PackedNode::pack(Node{children, level});
    const std::uint64_t hash = node_hash(want);
    auto& bucket = unique_[level]->buckets[hash % kShards];
    std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
    if (concurrent_) guard.lock();
    if (const Id found = bucket.nodes.find(nodes_, want, hash)) return found;
    const std::size_t cap = pool_cap();
    if (!concurrent_ && nodes_.size() >= cap) {
      throw Limit("DFA node pool cap reached (includes intermediate results)");
    }
    for (unsigned symbol = 0; symbol < 3; ++symbol) {
      const Id child = children[symbol];
      if (symbol >= alphabet_[level] && child != 0) {
        throw std::logic_error("DFA symbol outside alphabet");
      }
      if (child != 0 && (child >= nodes_.size() || (child >= 2 && child < base_) ||
                        nodes_.level(child) != level + 1 ||
                        (child == 1 && level + 1 != length()))) {
        throw std::logic_error("DFA transition skips a level");
      }
    }
    Id id = 0;
    if (concurrent_) {
      auto& ctx = context();
      const Id latest_child = std::max({children[0], children[1], children[2]});
      // Reserve a few IDs per worker rather than bouncing a shared counter
      // for every node. Discard a stale range when another worker supplied a
      // later child: every published child must still precede its parent.
      if (ctx.next_node >= ctx.end_node || ctx.next_node <= latest_child) {
        if (ctx.next_node < ctx.end_node) ctx.discarded += ctx.end_node - ctx.next_node;
        const auto range = nodes_.claim(64, cap);
        ctx.next_node = range.first; ctx.end_node = range.second;
      }
      id = static_cast<Id>(ctx.next_node++);
      nodes_.store_packed(id, want);
      ++ctx.created;
    } else {
      id = nodes_.append(Node{children, level}, cap);
      ++context().created;
    }
    bucket.nodes.insert(nodes_, id, hash);
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
    // Reuse small memos across successive calls; free (not merely clear)
    // a large one so a single huge product does not pin its table.
    if (memo.size() > (concurrent_ ? 32'768U : 2'000'000U)) memo.release();
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
      const PairKey key{x, y};
      if (Id known_value = 0; memo.find(key, known_value)) return known_value;
      if (shared != nullptr) {
        auto& bucket = (*shared)[hash_key(key) % shared->size()];
        std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
        if (concurrent_) guard.lock();
        const auto known = bucket.values.find(key);
        if (known != bucket.values.end()) {
          memo.insert(key, known->second);
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
      memo.insert(key, result);
      if (shared != nullptr) {
        auto& bucket = (*shared)[hash_key(key) % shared->size()];
        std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
        if (concurrent_) guard.lock();
        if (bucket.values.size() < 4'096) bucket.values.emplace(key, result);
      }
      return result;
    };
    return visit(a, b);
  }

  // Union of several languages in ONE product traversal over sorted tuples
  // of distinct nonzero IDs, so none of a reduction tree's pairwise
  // intermediate unions is interned (they were garbage as soon as the final
  // union existed).  Pairs are memoized in a flat 64-bit table and longer
  // tuples exactly in a TupleMap.
  Id unite_many(std::vector<Id> roots) {
    checkpoint();
    auto canonical = [](std::vector<Id>& ids) {
      ids.erase(std::remove(ids.begin(), ids.end(), 0), ids.end());
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };
    canonical(roots);
    if (roots.empty()) return 0;
    if (roots.size() == 1) return roots[0];
    PairMap pairs;
    TupleMap tuples;
    std::function<Id(const std::vector<Id>&)> visit = [&](const std::vector<Id>& ids) -> Id {
      const bool pair = ids.size() == 2;
      const PairKey pair_key = pair ? PairKey{ids[0], ids[1]} : PairKey{};
      const std::uint64_t tuple_hash = pair ? 0 : TupleMap::hash(ids);
      if (Id known_value = 0; pair ? pairs.find(pair_key, known_value) : tuples.find(ids, tuple_hash, known_value)) {
        return known_value;
      }
      const unsigned level = nodes_[ids[0]].level;
      for (Id id : ids) {
        if (id < 2 || nodes_[id].level != level) throw std::logic_error("incompatible DFA suffixes");
      }
      Children out{};
      std::vector<Id> child;
      for (unsigned c = 0; c < alphabet_[level]; ++c) {
        child.clear();
        for (Id id : ids) {
          if (nodes_[id].next[c] != 0) child.push_back(nodes_[id].next[c]);
        }
        canonical(child);
        out[c] = child.empty() ? 0 : child.size() == 1 ? child[0] : visit(child);
      }
      const Id result = intern(level, out);
      if (pair) pairs.insert(pair_key, result);
      else tuples.insert(ids, tuple_hash, result);
      return result;
    };
    const Id result = visit(roots);
    union_memo_pairs = pairs.size();
    union_memo_trie = tuples.size();
    union_memo_bytes = pairs.bytes() + tuples.bytes();
    return result;
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
    // An unrestricted cube without a rename is the identity.
    if (last < 0) return root;
    // A sparse per-call memo: its size follows the nodes visited, not the
    // largest node ID (a dense per-worker array grew with the whole pool).
    FlatMap memo;
    std::function<Id(Id)> visit = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (Id known = 0; memo.find(id, known)) return known;
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((allowed[old.level] & (1U << c)) == 0) continue;
        if (static_cast<int>(old.level) == variable && c != before) continue;
        const unsigned out = static_cast<int>(old.level) == variable ? after : c;
        children[out] = visit(old.next[c]);
      }
      const Id result = intern(old.level, children);
      memo.insert(id, result);
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
    FlatMap selected;
    int last = -1;
    for (unsigned i = variable + 1; i < length(); ++i) {
      if (predicate[i] != ((1U << alphabet_[i]) - 1)) last = static_cast<int>(i);
    }
    std::function<Id(Id)> select = [&](Id id) -> Id {
      if (id < 2 || static_cast<int>(nodes_[id].level) > last) return id;
      if (Id known = 0; selected.find(id, known)) return known;
      const Node old = nodes_[id];
      Children children{};
      for (unsigned c = 0; c < alphabet_[old.level]; ++c) {
        if ((predicate[old.level] & (1U << c)) != 0) children[c] = select(old.next[c]);
      }
      const Id result = intern(old.level, children);
      selected.insert(id, result);
      return result;
    };
    FlatMap memo;
    std::function<Id(Id, bool)> transform = [&](Id id, bool matches) -> Id {
      if (id == 0) return 0;
      if (id == 1 || nodes_[id].level > variable) throw std::logic_error("missing DFA assignment variable");
      const std::uint64_t key = (static_cast<std::uint64_t>(id) << 1) | matches;
      if (Id known = 0; memo.find(key, known)) return known;
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
      memo.insert(key, result);
      return result;
    };
    return transform(root, true);
  }

  std::uint64_t count(Id root) const {
    // Dense over [base, root]: in a compacted pool the counted layer is a
    // large part of it, and counts need full 64 bits.
    constexpr std::uint64_t kUnknown = std::numeric_limits<std::uint64_t>::max();
    std::vector<std::uint64_t> memo(root >= base_ ? root - base_ + 1 : 0, kUnknown);
    std::function<std::uint64_t(Id)> visit = [&](Id id) -> std::uint64_t {
      if (id < 2) return id;
      if (memo[id - base_] != kUnknown) return memo[id - base_];
      std::uint64_t result = 0;
      for (Id child : nodes_[id].next) {
        const std::uint64_t n = visit(child);
        if (n > std::numeric_limits<std::uint64_t>::max() - result) {
          throw std::overflow_error("represented position count exceeds uint64");
        }
        result += n;
      }
      memo[id - base_] = result;
      return result;
    };
    return visit(root);
  }

  std::uint64_t reachable_nodes(Id root) const {
    if (root == 0) return 0;
    if (root == 1) return 1;
    IdBitmap seen(base_, nodes_.size() - base_);
    std::uint64_t result = 0;
    bool accept = false;
    std::vector<Id> stack{root};
    seen.set(root);
    while (!stack.empty()) {
      const Id id = stack.back();
      stack.pop_back();
      ++result;
      for (Id child : nodes_[id].next) {
        if (child == 1) accept = true;
        else if (child >= 2 && seen.set(child)) stack.push_back(child);
      }
    }
    return result + (accept ? 1 : 0);
  }

  // Keep only nodes accessible from the supplied live roots.  Copying in
  // postorder also gives a portable topological ordering for serialization.
  //
  // Memory order: the old intern registry, operation caches and scratch are
  // never read while copying, so they are released right after marking; the
  // fresh registry is built only after the old node store has been freed.
  // The peak is max(old nodes + mark/renumber arrays + fresh nodes,
  // fresh nodes + fresh registry) rather than both graphs plus both
  // registries.  An exception after marking leaves the manager unusable,
  // which is acceptable: every caller treats a collection failure as fatal.
  void collect(const std::vector<Id*>& roots, unsigned workers = 1) {
    // Live set as a bitmap and renumbering by rank: about 0.14 bytes per
    // slot, instead of a byte flag plus a remapped ID per slot.
    IdBitmap live(base_, nodes_.size() - base_);
    std::vector<std::size_t> per_level(length());
    std::size_t live_count = 0;
    {
      // Level by level (every arc goes from level L to L + 1), marking each
      // level's children in parallel; only two level lists are live at once.
      std::vector<std::vector<Id>> by_level(length());
      for (Id* root : roots) {
        if (*root >= 2 && live.set(*root)) by_level.at(nodes_.level(*root)).push_back(*root);
      }
      for (unsigned level = 0; level < length(); ++level) {
        std::vector<Id>& here = by_level[level];
        per_level[level] = here.size();
        live_count += here.size();
        // run_parallel hands out 1024-node blocks; one result list per block.
        std::vector<std::vector<Id>> found((here.size() + 1023) / 1024);
        run_parallel(workers, here.size(), [&](std::size_t begin, std::size_t end) {
          std::vector<Id>& out = found[begin / 1024];
          for (std::size_t i = begin; i < end; ++i) {
            for (Id child : nodes_[here[i]].next) {
              if (child >= 2 && live.set_atomic(child)) out.push_back(child);
            }
          }
        });
        std::vector<Id>().swap(here);
        for (auto& f : found) {
          if (f.empty()) continue;
          if (level + 1 == length()) throw std::logic_error("collect: transition beyond the last level");
          by_level[level + 1].insert(by_level[level + 1].end(), f.begin(), f.end());
          std::vector<Id>().swap(f);
        }
        checkpoint();
      }
    }
    release_registry_and_caches(workers);
    const IdRank rank(live);
    auto mapped = [&](Id id) -> Id { return id < 2 ? id : base_ + rank.rank(id); };
    NodeStore fresh;
    fresh.push_back(Node{{}, length()});
    fresh.push_back(Node{{}, length()});
    fresh.skip_to(base_);
    fresh.reserve_compact(base_, live_count);
    // Copy word ranges of the bitmap in parallel; every new ID is known.
    run_parallel(workers, live.words(), [&](std::size_t begin, std::size_t end) {
      for (std::size_t w = begin; w < end; ++w) {
        std::uint64_t next_id = base_ + rank.prefix(w);
        for (std::uint64_t word = live.word(w); word != 0; word &= word - 1) {
          const Id old_id = base_ + w * 64 + static_cast<unsigned>(__builtin_ctzll(word));
          const Node old = nodes_[old_id];
          Children children{};
          for (unsigned c = 0; c < 3; ++c) children[c] = mapped(old.next[c]);
          fresh.store(next_id++, Node{children, old.level});
        }
      }
    });
    // Roots may alias.  Resolve all old IDs before changing any caller root.
    std::vector<Id> updated;
    updated.reserve(roots.size());
    for (Id* root : roots) updated.push_back(mapped(*root));
    for (std::size_t i = 0; i < roots.size(); ++i) *roots[i] = updated[i];
    nodes_ = std::move(fresh);  // frees the old blocks
    for (auto& level : unique_) level = std::make_unique<Level>();
    for (unsigned i = 0; i < length(); ++i) {
      for (auto& bucket : unique_[i]->buckets) bucket.nodes.reserve(nodes_, per_level[i] / kShards);
    }
    run_parallel(workers, live_count, [&](std::size_t begin, std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        const Id id = base_ + i;
        const PackedNode& node = nodes_.packed(id);
        const std::uint64_t hash = node_hash(node);
        auto& bucket = unique_[node.level()]->buckets[hash % kShards];
        std::unique_lock<std::mutex> guard(bucket.mutex, std::defer_lock);
        if (workers > 1) guard.lock();
        bucket.nodes.insert(nodes_, id, hash);
      }
    });
  }

 private:
  // Run body(begin, end) over [0, count) in 1024-item chunks on up to
  // `workers` threads; rethrow the first error after all threads finish.
  template <class Body>
  void run_parallel(unsigned workers, std::size_t count, Body body) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> stop{false};
    std::exception_ptr error;
    std::mutex error_mutex;
    auto run = [&]() {
      try {
        while (!stop.load(std::memory_order_relaxed)) {
          const std::size_t begin = next.fetch_add(1024, std::memory_order_relaxed);
          if (begin >= count) break;
          checkpoint();
          body(begin, std::min(begin + 1024, count));
        }
      } catch (...) {
        std::lock_guard<std::mutex> guard(error_mutex);
        if (!error) error = std::current_exception();
        stop.store(true, std::memory_order_relaxed);
      }
    };
    std::vector<std::thread> threads;
    if (workers > 1) {
      try {
        threads.reserve(workers - 1);
        for (unsigned i = 1; i < workers; ++i) threads.emplace_back(run);
      } catch (...) {}  // complete with the threads that could be created
    }
    run();
    for (auto& thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
  }

  // Freeing millions of hash entries allocated by many workers was a large
  // serial tail. Release independent registries/caches concurrently while
  // workers are quiescent. Leaves empty levels and one fresh context.
  void release_registry_and_caches(unsigned workers) {
    auto old_unique = std::move(unique_);
    auto old_contexts = std::move(contexts_);
    auto old_products = std::move(shared_products_);
    unique_.clear();
    unique_.resize(length());
    contexts_.clear();
    contexts_.push_back(std::make_unique<Context>());
    for (auto& products : shared_products_) products.reset();
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
    if (workers > 1) {
      try {
        cleaners.reserve(workers - 1);
        for (unsigned i = 1; i < workers; ++i) cleaners.emplace_back(release);
      } catch (...) {
        // Releasing memory must still complete if another OS thread cannot
        // be created.
      }
    }
    release();
    for (auto& cleaner : cleaners) cleaner.join();
  }

  // Slot cap: --dfa-max-nodes counts slots from 0 (plus any test ID base).
  std::size_t pool_cap() const {
    return max_nodes_ == 0 ? kMaxId : std::min<std::size_t>(kMaxId, max_nodes_ + (base_ - 2));
  }

  std::vector<std::uint8_t> alphabet_;
  NodeStore nodes_;
  std::vector<std::unique_ptr<Level>> unique_;
  std::size_t max_nodes_;
  std::function<void()> check_;
  Id base_ = 2;
  std::vector<std::unique_ptr<Context>> contexts_;
  std::array<std::unique_ptr<Products>, 3> shared_products_;
  bool concurrent_ = false;
  std::shared_ptr<std::atomic<bool>> worker_stop_ = std::make_shared<std::atomic<bool>>(false);
  Context& context() {
    return *contexts_[worker_manager == this ? worker_slot : 0];
  }
};

}  // namespace twixt_dfa
