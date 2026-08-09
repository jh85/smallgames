#include "zdd1.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace {
struct Builder {
  int m, lo, hi, D;
  std::unordered_map<u32, u32> memo;   // (d, nw, nb, f) -> node
  std::unordered_map<u64, u32> uniq;   // (var, lo, hi) -> node
  Zdd1* z;

  u32 rec(int d, int nw, int nb, int f) {
    if (d == D) return (nw >= lo && nw <= hi && nb >= lo && nb <= hi) ? 1u : 0u;
    u32 key = (u32)d << 12 | (u32)nw << 7 | (u32)nb << 2 | (u32)f;
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    int point = d / 2, rem = m - point;   // points not yet fully decided (incl. current)
    bool whiteItem = (d % 2 == 0);
    // 0-branch
    u32 l = rec(d + 1, nw, nb, whiteItem ? f : 0);
    // 1-branch: place a piece of this item's color here
    u32 h = 0;
    {
      int nw2 = nw + (whiteItem ? 1 : 0), nb2 = nb + (whiteItem ? 0 : 1);
      bool ok = !(!whiteItem && f);                    // overlap
      if (nw2 > hi || nb2 > hi) ok = false;
      // feasibility: enough room left for both colors to reach lo
      int remAfter = rem - 1;                          // points after this one
      int needW = lo - nw2, needB = lo - nb2;
      if (needW < 0) needW = 0;
      if (needB < 0) needB = 0;
      if (whiteItem) {
        // black may still use the current point? no: overlap. current point now occupied
        if (needW + needB > remAfter) ok = false;
      } else {
        if (needW + needB > remAfter) ok = false;
      }
      if (ok) h = rec(d + 1, nw2, nb2, whiteItem ? 1 : 0);
    }
    u32 res;
    if (h == 0) res = l;
    else {
      u64 hk = (u64)d << 56 | (u64)l << 28 | h;
      auto it2 = uniq.find(hk);
      if (it2 != uniq.end()) res = it2->second;
      else {
        res = (u32)z->var_.size();
        z->var_.push_back((u8)d);
        z->lo_.push_back(l);
        z->hi_.push_back(h);
        uniq.emplace(hk, res);
      }
    }
    memo.emplace(key, res);
    return res;
  }
};
}  // namespace

void Zdd1::build(int points, int loBound, int hiBound) {
  m = points; lo = loBound; hi = hiBound;
  var_.assign(2, (u8)(2 * m));
  lo_.assign(2, 0);
  hi_.assign(2, 0);
  Builder B{m, lo, hi, 2 * m, {}, {}, this};
  root = B.rec(0, 0, 0, 0);
  // counts (children created before parents -> increasing ids are topological)
  std::vector<u64> cnt(var_.size());
  cnt[0] = 0; cnt[1] = 1;
  for (u32 i = 2; i < (u32)var_.size(); ++i) cnt[i] = cnt[lo_[i]] + cnt[hi_[i]];
  cntlo_.assign(var_.size(), 0);
  for (u32 i = 2; i < (u32)var_.size(); ++i) cntlo_[i] = cnt[lo_[i]];
  total = cnt[root];
}

u64 Zdd1::rank(u32 w, u32 b) const {
  u32 n = root;
  u64 k = 0;
  while (n > 1) {
    int d = var_[n], p = d / 2;
    u32 bit = (d % 2 == 0) ? (w >> p) & 1 : (b >> p) & 1;
    if (bit) { k += cntlo_[n]; n = hi_[n]; }
    else n = lo_[n];
  }
  return n == 1 ? k : UINT64_MAX;
}

void Zdd1::unrank(u64 idx, u32& w, u32& b) const {
  w = b = 0;
  u32 n = root;
  while (n > 1) {
    int d = var_[n], p = d / 2;
    if (cntlo_[n] <= idx) {
      idx -= cntlo_[n];
      if (d % 2 == 0) w |= 1u << p; else b |= 1u << p;
      n = hi_[n];
    } else n = lo_[n];
  }
  assert(n == 1 && idx == 0);
}

void Zdd1Iter::initAt(const Zdd1& zz, u64 idx) {
  z = &zz;
  sp = 0; w = b = 0;
  u32 n = z->root;
  while (n > 1) {
    nstack[sp] = n;
    int d = z->var_[n], p = d / 2;
    if (z->cntlo_[n] <= idx) {
      idx -= z->cntlo_[n];
      br[sp] = 1;
      if (d % 2 == 0) w |= 1u << p; else b |= 1u << p;
      n = z->hi_[n];
    } else { br[sp] = 0; n = z->lo_[n]; }
    ++sp;
  }
  assert(n == 1 && idx == 0);
}

bool Zdd1Iter::next() {
  while (sp) {
    --sp;
    u32 n = nstack[sp];
    int d = z->var_[n], p = d / 2;
    if (br[sp]) {
      if (d % 2 == 0) w &= ~(1u << p); else b &= ~(1u << p);
    } else {
      br[sp] = 1;
      if (d % 2 == 0) w |= 1u << p; else b |= 1u << p;
      ++sp;
      u32 mnode = z->hi_[n];
      while (mnode > 1) {
        nstack[sp] = mnode;
        int d2 = z->var_[mnode], p2 = d2 / 2;
        if (z->lo_[mnode] == 0) {
          br[sp] = 1;
          if (d2 % 2 == 0) w |= 1u << p2; else b |= 1u << p2;
          mnode = z->hi_[mnode];
        } else { br[sp] = 0; mnode = z->lo_[mnode]; }
        ++sp;
      }
      assert(mnode == 1);
      return true;
    }
  }
  return false;
}

void Zdd1::save(const std::string& path, u64 boardHash) const {
  FILE* f = fopen((path + ".tmp").c_str(), "wb");
  if (!f) throw std::runtime_error("cannot write " + path);
  u64 hdr[8] = {0x5A44443100000001ull, boardHash, (u64)m, ((u64)lo << 8) | (u64)hi,
                var_.size(), root, total, 0};
  for (u32 i = 2; i < (u32)var_.size(); ++i)
    hdr[7] ^= (u64)var_[i] * 0x9E3779B97F4A7C15ull + lo_[i] + ((u64)hi_[i] << 32);
  fwrite(hdr, 8, 8, f);
  fwrite(var_.data(), 1, var_.size(), f);
  fwrite(lo_.data(), 4, lo_.size(), f);
  fwrite(hi_.data(), 4, hi_.size(), f);
  fwrite(cntlo_.data(), 8, cntlo_.size(), f);
  fclose(f);
  if (rename((path + ".tmp").c_str(), path.c_str()))
    throw std::runtime_error("rename failed");
}

bool Zdd1::load(const std::string& path, u64 boardHash) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  u64 hdr[8];
  if (fread(hdr, 8, 8, f) != 8 || hdr[0] != 0x5A44443100000001ull || hdr[1] != boardHash) {
    fclose(f);
    return false;
  }
  m = (int)hdr[2]; lo = (int)(hdr[3] >> 8); hi = (int)(hdr[3] & 255);
  size_t n = hdr[4];
  var_.resize(n); lo_.resize(n); hi_.resize(n); cntlo_.resize(n);
  root = (u32)hdr[5]; total = hdr[6];
  if (fread(var_.data(), 1, n, f) != n) throw std::runtime_error("zdd1 read");
  if (fread(lo_.data(), 4, n, f) != n) throw std::runtime_error("zdd1 read");
  if (fread(hi_.data(), 4, n, f) != n) throw std::runtime_error("zdd1 read");
  if (fread(cntlo_.data(), 8, n, f) != n) throw std::runtime_error("zdd1 read");
  fclose(f);
  u64 chk = 0;
  for (u32 i = 2; i < (u32)var_.size(); ++i)
    chk ^= (u64)var_[i] * 0x9E3779B97F4A7C15ull + lo_[i] + ((u64)hi_[i] << 32);
  if (chk != hdr[7]) throw std::runtime_error("zdd1 checksum mismatch");
  return true;
}
