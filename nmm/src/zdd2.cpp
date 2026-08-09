#include "zdd2.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>
#include <unordered_map>
#include <functional>

static void* bigalloc(size_t bytes) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) throw std::runtime_error("mmap failed");
  return p;
}

void Pool2::init(u64 maxNodes, int slotsLog) {
  var_.resize(maxNodes);
  lo_.resize(maxNodes);
  hi_.resize(maxNodes);
  next_.resize(maxNodes);
  nbuckets = 1ull << slotsLog;
  buckets = (u32*)bigalloc(nbuckets * 4);
  locks = std::make_unique<std::mutex[]>(NSTRIPES);
  var_[0] = var_[1] = 255;
  lo_[0] = hi_[0] = lo_[1] = hi_[1] = 0;
  nnodes = 2;
}

void Pool2::releaseBuildStructures() {
  next_.clear();
  next_.shrink_to_fit();
  if (buckets) { munmap(buckets, nbuckets * 4); buckets = nullptr; }
  locks.reset();
}

// Chained hashing under striped bucket locks: simple and sequentially consistent per
// bucket (an earlier lock-free open-addressing variant rarely corrupted nodes under
// contention; the build-time cost of locking is negligible and a count-integrity gate
// verifies every build).
u32 Pool2::intern(u8 v, u32 lo, u32 hi) {
  if (hi == 0) return lo;
  u64 h = ((u64)v * 0x9E3779B97F4A7C15ull) ^ ((u64)lo * 0xC2B2AE3D27D4EB4Full) ^
          ((u64)hi * 0x165667B19E3779F9ull);
  u64 b = (h ^ (h >> 29)) & (nbuckets - 1);
  std::lock_guard<std::mutex> g(locks[b & (NSTRIPES - 1)]);
  for (u32 id = buckets[b]; id; id = next_[id])
    if (var_[id] == v && lo_[id] == lo && hi_[id] == hi) return id;
  u32 id = (u32)nnodes.fetch_add(1, std::memory_order_relaxed);
  if (id >= var_.size()) throw std::runtime_error("pool2 exhausted");
  var_[id] = v;
  lo_[id] = lo;
  hi_[id] = hi;
  next_[id] = buckets[b];
  buckets[b] = id;
  return id;
}

void Pool2::finalizeCounts() {
  u64 n = nnodes.load();
  std::vector<u64> cnt(n);
  cnt[0] = 0;
  cnt[1] = 1;
  for (u64 i = 2; i < n; ++i) cnt[i] = cnt[lo_[i]] + cnt[hi_[i]];
  cntlo_.assign(n, 0);
  for (u64 i = 2; i < n; ++i) cntlo_[i] = cnt[lo_[i]];
}

u64 Pool2::countAt(u32 root) const {
  if (root <= 1) return root;
  // cnt(root) = cntlo(root) + cnt(hi): walk hi chain
  u64 c = 0;
  u32 x = root;
  while (x > 1) { c += cntlo_[x]; x = hi_[x]; }
  return c + (x == 1 ? 1 : 0);
}

u64 Pool2::reachableNodes(u32 root) const {
  std::vector<u32> stack{root};
  std::vector<u8> seen(nnodes.load(), 0);
  u64 n = 0;
  while (!stack.empty()) {
    u32 x = stack.back();
    stack.pop_back();
    if (seen[x]) continue;
    seen[x] = 1;
    ++n;
    if (x > 1) { stack.push_back(lo_[x]); stack.push_back(hi_[x]); }
  }
  return n;
}

// Zero-suppressed semantics: a skipped level means that bit MUST be 0; queries with a
// 1-bit at a skipped level are not members (critical for out-of-index detection).
static inline bool skippedBitsSet(u64 rank1, int totBits, int lastVar, int v) {
  if (v <= lastVar + 1) return false;
  int hiBit = totBits - 1 - (lastVar + 1);
  int loBit = totBits - v;
  u64 mask = (hiBit >= 63 ? ~0ull : ((1ull << (hiBit + 1)) - 1)) & ~((1ull << loBit) - 1);
  return (rank1 & mask) != 0;
}
bool Sel::contains(u64 rank1) const {
  u32 n = root;
  int lastVar = -1;
  while (n > 1) {
    int v = P->var_[n];
    if (skippedBitsSet(rank1, totBits, lastVar, v)) return false;
    lastVar = v;
    n = ((rank1 >> (totBits - 1 - v)) & 1) ? P->hi_[n] : P->lo_[n];
  }
  if (n == 0) return false;
  if (lastVar + 1 < totBits && (rank1 & ((1ull << (totBits - 1 - lastVar)) - 1))) return false;
  return true;
}
u64 Sel::selRank(u64 rank1) const {
  u32 n = root;
  u64 k = 0;
  int lastVar = -1;
  while (n > 1) {
    int v = P->var_[n];
    if (skippedBitsSet(rank1, totBits, lastVar, v)) return UINT64_MAX;
    lastVar = v;
    if ((rank1 >> (totBits - 1 - v)) & 1) { k += P->cntlo_[n]; n = P->hi_[n]; }
    else n = P->lo_[n];
  }
  if (n == 0) return UINT64_MAX;
  if (lastVar + 1 < totBits && (rank1 & ((1ull << (totBits - 1 - lastVar)) - 1)))
    return UINT64_MAX;
  return k;
}
u64 Sel::selUnrank(u64 k) const {
  u32 n = root;
  u64 r = 0;
  while (n > 1) {
    int bit = totBits - 1 - P->var_[n];
    if (P->cntlo_[n] <= k) { k -= P->cntlo_[n]; r |= 1ull << bit; n = P->hi_[n]; }
    else n = P->lo_[n];
  }
  assert(n == 1 && k == 0);
  return r;
}

// ------------------------------------------------------------------------------------------
namespace {
struct CodeLut {
  // codeLut[sym][color][byteIdx][byteVal] -> contribution to interleaved code of the
  // permuted configuration (MSB-first, white bit above black bit per point)
  std::vector<std::array<std::array<std::array<u64, 256>, 4>, 2>> t;
  void build(const Board& bd, const std::vector<std::array<u8, 32>>& perms) {
    int m = bd.m, S = (int)perms.size();
    t.resize(S);
    for (int s = 0; s < S; ++s)
      for (int color = 0; color < 2; ++color)
        for (int by = 0; by < 4; ++by)
          for (int v = 0; v < 256; ++v) {
            u64 c = 0;
            for (int bit = 0; bit < 8; ++bit)
              if (v >> bit & 1) {
                int p = by * 8 + bit;
                if (p < m) {
                  int q = perms[s][p];
                  c |= 1ull << (2 * (m - 1 - q) + (color == 0 ? 1 : 0));
                }
              }
            t[s][color][by][v] = c;
          }
  }
  inline u64 code(int s, u32 w, u32 b) const {
    const auto& tw = t[s][0];
    const auto& tb = t[s][1];
    return tw[0][w & 255] | tw[1][(w >> 8) & 255] | tw[2][(w >> 16) & 255] |
           tw[3][(w >> 24) & 255] | tb[0][b & 255] | tb[1][(b >> 8) & 255] |
           tb[2][(b >> 16) & 255] | tb[3][(b >> 24) & 255];
  }
};

struct TrieBuilder {
  Pool2* P;
  int totBits, blockBits;
  // build sparse subtree over sorted offsets in [0, 2^bits), varBase = first var index
  u32 build(const u32* idx, u32 n, int bits, int varBase, u32 offBase) {
    if (n == 0) return 0;
    if (bits == 0) return 1;
    u32 mid = offBase + (1u << (bits - 1));
    // binary search split
    u32 lo = 0, hi = n;
    while (lo < hi) {
      u32 md = (lo + hi) / 2;
      if (idx[md] < mid) lo = md + 1;
      else hi = md;
    }
    u32 l = build(idx, lo, bits - 1, varBase + 1, offBase);
    u32 h = build(idx + lo, n - lo, bits - 1, varBase + 1, mid);
    return P->intern((u8)varBase, l, h);
  }
};
}  // namespace

SweepResult sweepAndBuild(const Board& bd, const Zdd1& z1, int nMin, int nMax,
                          bool paperFilter, bool buildZdd, Forest2* out, int threads) {
  CodeLut lut, lutMill;
  lut.build(bd, bd.perms);
  bool haveMill = paperFilter && bd.spec.flying && bd.millPerms.size() > bd.perms.size() && nMin <= 3;  // phase-2/3 only: placement 3-3 must keep the board group
  if (haveMill) lutMill.build(bd, bd.millPerms);
  int S = (int)bd.perms.size();
  int SM = (int)bd.millPerms.size();
  int N = bd.spec.pieces;
  int nSub = nMax - nMin + 1;
  int nStreams = nSub * nSub + 1;   // subsets + global
  int totBits = 1;
  while ((1ull << totBits) < z1.total) ++totBits;
  int blockBits = std::min(20, totBits);
  u64 nBlocks = (z1.total + (1ull << blockBits) - 1) >> blockBits;

  SweepResult res;
  res.tally.assign(N + 1, std::vector<u64>(N + 1, 0));

  std::vector<std::vector<u32>> blockRoots;
  if (buildZdd) {
    assert(out);
    out->bd = &bd;
    out->z1 = &z1;
    out->totBits = totBits;
    out->nMin = nMin;
    out->nMax = nMax;
    out->paperFilter = paperFilter;
    u64 maxNodes = z1.total > (1ull << 33) ? 1500000000ull : 80000000ull;
    int slotsLog = z1.total > (1ull << 33) ? 31 : 27;
    out->pool.init(maxNodes, slotsLog);
    blockRoots.assign(nStreams, std::vector<u32>(nBlocks, 0));
  }

  std::atomic<u64> blockCtr{0};
  std::vector<std::thread> th;
  std::vector<std::vector<std::vector<u64>>> tallies(
      threads, std::vector<std::vector<u64>>(N + 1, std::vector<u64>(N + 1, 0)));
  for (int t = 0; t < threads; ++t)
    th.emplace_back([&, t] {
      std::unordered_map<u32, int> hsMemo;
      std::vector<std::vector<u32>> accepted(nStreams);
      Zdd1Iter it;
      TrieBuilder tb{buildZdd ? &out->pool : nullptr, totBits, blockBits};
      for (;;) {
        u64 blk = blockCtr.fetch_add(1);
        if (blk >= nBlocks) break;
        u64 lo = blk << blockBits;
        u64 hi = std::min(z1.total, lo + ((u64)1 << blockBits));
        for (auto& v : accepted) v.clear();
        it.initAt(z1, lo);
        for (u64 i = lo; i < hi; ++i) {
          u32 w = it.w, b = it.b;
          int pw = __builtin_popcount(w), pb = __builtin_popcount(b);
          bool flying33 = haveMill && pw == 3 && pb == 3;
          const CodeLut& L = flying33 ? lutMill : lut;
          int nOps = flying33 ? SM : S;
          u64 c0 = L.code(0, w, b);
          bool canon = true;
          for (int s = 1; s < nOps && canon; ++s)
            if (L.code(s, w, b) < c0) canon = false;
          if (canon) {
            bool ok = true;
            if (paperFilter) {
              u32 fw = 0, fb = 0;
              for (size_t mi = 0; mi < bd.millMask.size(); ++mi) {
                u32 mk = bd.millMask[mi];
                if ((w & mk) == mk) fw |= 1u << mi;
                if ((b & mk) == mk) fb |= 1u << mi;
              }
              auto hs = [&](u32 fullMask, u32 pieces) -> int {
                if (!fullMask) return 0;
                auto itm = hsMemo.find(fullMask);
                if (itm != hsMemo.end()) return itm->second;
                int v = bd.minMillEvents(pieces);   // = min hitting set of fullMask's mills
                hsMemo.emplace(fullMask, v);
                return v;
              };
              if (pb + hs(fw, w) > N || pw + hs(fb, b) > N) ok = false;
            }
            if (ok) {
              ++tallies[t][pw][pb];
              if (buildZdd) {
                accepted[out->sidx(pw, pb)].push_back((u32)(i - lo));
                accepted[nSub * nSub].push_back((u32)(i - lo));
              }
            }
          }
          if (i + 1 < hi) it.next();
        }
        if (buildZdd) {
          int upperBits = totBits - blockBits;
          for (int st = 0; st < nStreams; ++st)
            if (!accepted[st].empty())
              blockRoots[st][blk] = tb.build(accepted[st].data(), (u32)accepted[st].size(),
                                             blockBits, upperBits, 0);
        }
      }
    });
  for (auto& x : th) x.join();
  for (int t = 0; t < threads; ++t)
    for (int w = 0; w <= N; ++w)
      for (int b = 0; b <= N; ++b) res.tally[w][b] += tallies[t][w][b];
  for (int w = 0; w <= N; ++w)
    for (int b = 0; b <= N; ++b) res.totalAccepted += res.tally[w][b];

  if (buildZdd) {
    // combine block roots bottom-up over the upper bits (sparse, single-threaded is fine)
    int upperBits = totBits - blockBits;
    out->subsetRoot.assign(nSub * nSub, 0);
    out->subsetCount.assign(nSub * nSub, 0);
    for (int st = 0; st < nStreams; ++st) {
      std::function<u32(u64, int, int)> up = [&](u64 base, int bits, int varBase) -> u32 {
        if (bits == 0) return base < nBlocks ? blockRoots[st][base] : 0;
        u32 l = up(base * 2, bits - 1, varBase + 1);
        u32 h = up(base * 2 + 1, bits - 1, varBase + 1);
        return out->pool.intern((u8)varBase, l, h);
      };
      u32 root = up(0, upperBits, 0);
      if (st < nSub * nSub) out->subsetRoot[st] = root;
      else out->globalRoot = root;
    }
    out->pool.finalizeCounts();
    u64 fSum = 0;
    for (int st = 0; st < nSub * nSub; ++st) {
      out->subsetCount[st] = out->pool.countAt(out->subsetRoot[st]);
      fSum += out->subsetCount[st];
    }
    out->globalCount = out->pool.countAt(out->globalRoot);
    // hard integrity gate: the compressed index must agree with the sweep tally exactly
    bool ok = fSum == res.totalAccepted && out->globalCount == res.totalAccepted;
    for (int w = nMin; w <= nMax && ok; ++w)
      for (int b = nMin; b <= nMax && ok; ++b)
        if (out->subsetCount[out->sidx(w, b)] != res.tally[w][b]) ok = false;
    if (!ok)
      throw std::runtime_error("ZDD2 build integrity check FAILED (count mismatch)");
    out->pool.releaseBuildStructures();
  }
  return res;
}

void Forest2::save(const std::string& path) const {
  FILE* f = fopen((path + ".tmp").c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + path);
  u64 n = pool.nnodes.load();
  u64 hdr[8] = {0x5A44443200000001ull, bd->boardHash(), (u64)totBits,
                ((u64)nMin << 16) | ((u64)nMax << 8) | (paperFilter ? 1 : 0), n,
                globalRoot, globalCount, z1->total};
  fwrite(hdr, 8, 8, f);
  u32 ns = (u32)subsetRoot.size();
  fwrite(&ns, 4, 1, f);
  fwrite(subsetRoot.data(), 4, ns, f);
  fwrite(subsetCount.data(), 8, ns, f);
  fwrite(pool.var_.data(), 1, n, f);
  fwrite(pool.lo_.data(), 4, n, f);
  fwrite(pool.hi_.data(), 4, n, f);
  fwrite(pool.cntlo_.data(), 8, n, f);
  fclose(f);
  if (rename((path + ".tmp").c_str(), path.c_str())) throw std::runtime_error("rename");
}

bool Forest2::load(const std::string& path, const Board& b, const Zdd1& z) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  u64 hdr[8];
  if (fread(hdr, 8, 8, f) != 8 || hdr[0] != 0x5A44443200000001ull ||
      hdr[1] != b.boardHash() || hdr[7] != z.total) {
    fclose(f);
    return false;
  }
  bd = &b;
  z1 = &z;
  totBits = (int)hdr[2];
  nMin = (int)(hdr[3] >> 16);
  nMax = (int)((hdr[3] >> 8) & 255);
  paperFilter = hdr[3] & 1;
  u64 n = hdr[4];
  globalRoot = (u32)hdr[5];
  globalCount = hdr[6];
  u32 ns;
  if (fread(&ns, 4, 1, f) != 1) throw std::runtime_error("zdd2 read");
  subsetRoot.resize(ns);
  subsetCount.resize(ns);
  if (fread(subsetRoot.data(), 4, ns, f) != ns) throw std::runtime_error("zdd2 read");
  if (fread(subsetCount.data(), 8, ns, f) != ns) throw std::runtime_error("zdd2 read");
  pool.var_.resize(n);
  pool.lo_.resize(n);
  pool.hi_.resize(n);
  pool.cntlo_.resize(n);
  if (fread(pool.var_.data(), 1, n, f) != n) throw std::runtime_error("zdd2 read");
  if (fread(pool.lo_.data(), 4, n, f) != n) throw std::runtime_error("zdd2 read");
  if (fread(pool.hi_.data(), 4, n, f) != n) throw std::runtime_error("zdd2 read");
  if (fread(pool.cntlo_.data(), 8, n, f) != n) throw std::runtime_error("zdd2 read");
  fclose(f);
  pool.nnodes = n;
  return true;
}
