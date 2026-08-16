#include "zdd.h"

#include <cstdio>
#include <cstring>

#include "board.h"

namespace amazons {

namespace {
constexpr char kWdlMagic[8] = {'A', 'Z', 'W', 'D', 'L', '0', '0', '1'};
}  // namespace

ZddManager::NodeId ZddManager::GetNode(uint32_t var, NodeId lo, NodeId hi) {
  if (hi == kEmpty) return lo;  // zero-suppression
  const NodeKey key{var, lo, hi};
  auto it = unique_.find(key);
  if (it != unique_.end()) return it->second;
  const NodeId id = static_cast<NodeId>(nodes_.size());
  nodes_.push_back(Node{var, lo, hi});
  unique_.emplace(key, id);
  return id;
}

ZddManager::NodeId ZddManager::Singleton(const std::vector<uint32_t>& vars) {
  NodeId n = kUnit;
  for (auto it = vars.rbegin(); it != vars.rend(); ++it)
    n = GetNode(*it, kEmpty, n);
  return n;
}

ZddManager::NodeId ZddManager::Union(NodeId a, NodeId b) {
  if (a > b) {
    const NodeId t = a;
    a = b;
    b = t;
  }
  return UnionRec(a, b);
}

ZddManager::NodeId ZddManager::UnionRec(NodeId a, NodeId b) {
  if (a == kEmpty) return b;
  if (b == kEmpty) return a;
  if (a == b) return a;
  if (a == kUnit) {
    if (b == kUnit) return kUnit;
    const Node nb = nodes_[b];  // by value: recursion may reallocate
    // The empty set joins the side of b's family not containing nb.var.
    return GetNode(nb.var, UnionRec(kUnit, nb.lo), nb.hi);
  }
  if (b == kUnit) {
    const Node na = nodes_[a];  // by value: recursion may reallocate
    return GetNode(na.var, UnionRec(na.lo, kUnit), na.hi);
  }
  const uint64_t ck = (static_cast<uint64_t>(a) << 32) | b;
  auto it = union_cache_.find(ck);
  if (it != union_cache_.end()) return it->second;

  const Node na = nodes_[a];  // by value: recursion may reallocate
  const Node nb = nodes_[b];
  NodeId r;
  if (na.var < nb.var) {
    r = GetNode(na.var, UnionRec(na.lo, b), na.hi);
  } else if (na.var > nb.var) {
    r = GetNode(nb.var, UnionRec(a, nb.lo), nb.hi);
  } else {
    r = GetNode(na.var, UnionRec(na.lo, nb.lo), UnionRec(na.hi, nb.hi));
  }
  union_cache_.emplace(ck, r);
  return r;
}

bool ZddManager::Contains(NodeId family,
                          const std::vector<uint32_t>& vars) const {
  NodeId n = family;
  size_t i = 0;
  while (n > kUnit) {
    const Node& nd = nodes_[n];
    if (i == vars.size() || vars[i] > nd.var) {
      n = nd.lo;  // sets not containing nd.var
    } else if (vars[i] == nd.var) {
      n = nd.hi;
      i++;
    } else {
      return false;  // vars[i] < nd.var: no set here contains vars[i]
    }
  }
  return n == kUnit && i == vars.size();
}

uint64_t ZddManager::Count(NodeId family) {  if (family == kEmpty) return 0;
  if (family == kUnit) return 1;
  auto it = count_cache_.find(family);
  if (it != count_cache_.end()) return it->second;
  const Node& nd = nodes_[family];
  const uint64_t cl = Count(nd.lo), ch = Count(nd.hi);
  uint64_t r = cl + ch;
  if (r < cl) r = UINT64_MAX;  // saturate
  count_cache_.emplace(family, r);
  return r;
}

bool ZddManager::Save(FILE* f) const {
  const uint64_t n = nodes_.size();
  if (std::fwrite(&n, sizeof n, 1, f) != 1) return false;
  for (const Node& nd : nodes_) {
    const uint32_t raw[3] = {nd.var, nd.lo, nd.hi};
    if (std::fwrite(raw, sizeof raw, 1, f) != 1) return false;
  }
  return true;
}

bool ZddManager::Load(FILE* f) {
  uint64_t n = 0;
  if (std::fread(&n, sizeof n, 1, f) != 1) return false;
  if (n < 2 || n > (uint64_t{1} << 31)) return false;
  nodes_.clear();
  nodes_.reserve(n);
  unique_.clear();
  union_cache_.clear();
  count_cache_.clear();
  for (uint64_t i = 0; i < n; i++) {
    uint32_t raw[3];
    if (std::fread(raw, sizeof raw, 1, f) != 1) return false;
    Node nd{raw[0], raw[1], raw[2]};
    if (i >= 2 && (nd.lo >= i || nd.hi >= i)) return false;  // DAG check
    const NodeId id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(nd);
    if (i >= 2) unique_.emplace(NodeKey{nd.var, nd.lo, nd.hi}, id);
  }
  return true;
}

std::vector<uint32_t> VerdictDb::Encode(const Position& pos) {
  std::vector<uint32_t> vars;
  const int n = pos.NumSquares();
  for (int sq = 0; sq < n; sq++) {
    const Bitboard b = uint64_t{1} << sq;
    const bool w = pos.queens[kWhite] & b;
    const bool bl = pos.queens[kBlack] & b;
    const bool bu = pos.burned & b;
    if (w || bu) vars.push_back(2 * sq);      // v0: white queen or burned
    if (bl || bu) vars.push_back(2 * sq + 1); // v1: black queen or burned
  }
  return vars;
}

void VerdictDb::InsertWin(const Position& canonical_pos) {
  const std::vector<uint32_t> vars = Encode(canonical_pos);
  const ZddManager::NodeId s = mgr_.Singleton(vars);
  const ZddManager::NodeId nw = mgr_.Union(wins_, s);
  if (nw != wins_) {
    wins_ = nw;
    num_wins_++;
  }
}

void VerdictDb::InsertLoss(const Position& canonical_pos) {
  const std::vector<uint32_t> vars = Encode(canonical_pos);
  const ZddManager::NodeId s = mgr_.Singleton(vars);
  const ZddManager::NodeId nl = mgr_.Union(losses_, s);
  if (nl != losses_) {
    losses_ = nl;
    num_losses_++;
  }
}

int VerdictDb::Probe(const Position& canonical_pos) const {
  probes_++;
  const std::vector<uint32_t> vars = Encode(canonical_pos);
  if (mgr_.Contains(wins_, vars)) {
    hits_++;
    return 1;
  }
  if (mgr_.Contains(losses_, vars)) {
    hits_++;
    return -1;
  }
  return 0;
}

bool VerdictDb::Save(const char* path, int w, int h) const {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  bool ok = std::fwrite(kWdlMagic, sizeof kWdlMagic, 1, f) == 1;
  const uint32_t dims[2] = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
  ok = ok && std::fwrite(dims, sizeof dims, 1, f) == 1;
  const uint64_t counts[2] = {num_wins_, num_losses_};
  ok = ok && std::fwrite(counts, sizeof counts, 1, f) == 1;
  const uint32_t roots[2] = {wins_, losses_};
  ok = ok && std::fwrite(roots, sizeof roots, 1, f) == 1;
  ok = ok && mgr_.Save(f);
  ok = ok && std::fclose(f) == 0;
  return ok;
}

bool VerdictDb::Load(const char* path, int w, int h) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  char magic[8];
  uint32_t dims[2];
  uint64_t counts[2];
  uint32_t roots[2];
  bool ok = std::fread(magic, sizeof magic, 1, f) == 1 &&
            std::memcmp(magic, kWdlMagic, sizeof magic) == 0 &&
            std::fread(dims, sizeof dims, 1, f) == 1 &&
            std::fread(counts, sizeof counts, 1, f) == 1 &&
            std::fread(roots, sizeof roots, 1, f) == 1;
  if (ok && (dims[0] != static_cast<uint32_t>(w) ||
             dims[1] != static_cast<uint32_t>(h))) {
    ok = false;  // encoding depends on board size
  }
  ZddManager mgr;
  if (ok) ok = mgr.Load(f);
  ok = ok && std::fclose(f) == 0;
  if (!ok) return false;
  if (roots[0] >= mgr.NodeCount() + 2 || roots[1] >= mgr.NodeCount() + 2)
    return false;
  mgr_ = std::move(mgr);
  wins_ = roots[0];
  losses_ = roots[1];
  num_wins_ = counts[0];
  num_losses_ = counts[1];
  return true;
}

}  // namespace amazons
