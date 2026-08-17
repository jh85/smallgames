#include "index.h"

#include <algorithm>
#include <stdexcept>

namespace fan {

uint64_t gBinom[kMaxPoints + 2][kMaxPoints + 2];

void InitBinom() {
  for (int n = 0; n <= kMaxPoints + 1; n++) {
    gBinom[n][0] = 1;
    for (int k = 1; k <= n; k++) gBinom[n][k] = gBinom[n - 1][k - 1] + gBinom[n - 1][k];
  }
}

// Colex rank: for sorted elements p_1 < ... < p_k, rank = sum_i C(p_i, i).
uint64_t RankSet(uint64_t set, int k) {
  uint64_t rank = 0;
  int i = 0;
  while (set) {
    const int p = __builtin_ctzll(set);
    set &= set - 1;
    i++;
    rank += gBinom[p][i];  // C(p, i) = 0 when p < i
  }
  (void)k;
  return rank;
}

uint64_t UnrankSet(uint64_t rank, int k, int n) {
  uint64_t set = 0;
  int p = n - 1;
  for (int i = k; i >= 1; i--) {
    while (gBinom[p][i] > rank) p--;
    rank -= gBinom[p][i];
    set |= 1ull << p;
    p--;
  }
  return set;
}

// Rank of `set` (k elements) within the sequence of points not in `block`.
uint64_t RankRestricted(uint64_t set, uint64_t block, int k) {
  uint64_t rank = 0;
  int j = 0;  // 1-based order among set elements
  uint64_t bits = set;
  while (bits) {
    const int p = __builtin_ctzll(bits);
    bits &= bits - 1;
    // 0-based index of p among available (non-blocked) points.
    const int ai = p - __builtin_popcountll(block & ((1ull << p) - 1));
    j++;
    rank += gBinom[ai][j];
  }
  (void)k;
  return rank;
}

uint64_t UnrankRestricted(uint64_t rank, uint64_t block, int k, int n) {
  int pts[kMaxPoints];
  int m = 0;
  for (int p = 0; p < n; p++) {
    if (block >> p & 1) continue;
    pts[m++] = p;
  }
  uint64_t chosen = 0;
  int ai = m - 1;
  for (int i = k; i >= 1; i--) {
    while (gBinom[ai][i] > rank) ai--;
    rank -= gBinom[ai][i];
    chosen |= 1ull << pts[ai];
    ai--;
  }
  return chosen;
}

// ---------------------------------------------------------------------------
// Indexer
// ---------------------------------------------------------------------------

void Indexer::InitSym(int sym_mode) {
  if (sym_mode == 4) {
    if (!geom.square)
      throw std::runtime_error("D4 fold requires a square board");
    for (int t = 0; t < 8; t++)
      if (!geom.sym_ok[t])
        throw std::runtime_error(
            "D4 transforms are not all board automorphisms "
            "(odd side length required)");
    nsym_ = 8;
    for (int t = 0; t < 8; t++) sym_ids_[t] = t;
    fd_mask_ = geom.fd_mask;
  } else if (sym_mode == 2) {
    const int d2[4] = {0, 2, 4, 5};
    for (int i = 1; i < 4; i++)
      if (!geom.sym_ok[d2[i]])
        throw std::runtime_error(
            "D2 fold requires both side lengths to be odd");
    if (!geom.fd2_mask)
      throw std::runtime_error("D2 fold: no fundamental domain");
    nsym_ = 4;
    for (int i = 0; i < 4; i++) sym_ids_[i] = d2[i];
    fd_mask_ = geom.fd2_mask;
  } else if (sym_mode != 0) {
    throw std::runtime_error("unknown symmetry mode (use 0, 2 or 4)");
  }
}

void Indexer::InitLayers() {
  const int n = geom.N;
  for (int a = 0; a <= n; a++)
    for (int b = a; b <= n; b++) {
      if (a + b > n) continue;
      Layer l;
      l.a = a;
      l.b = b;
      l.sym = nsym_ > 1 && a >= 1;
      if (!l.sym) {
        l.sub_size[0] = gBinom[n][a] * gBinom[n - a][b];
        l.sub_size[1] = (a == b) ? 0 : gBinom[n][b] * gBinom[n - b][a];
      } else {
        for (int sub = 0; sub < 2; sub++) {
          const int wc = sub == 0 ? a : b;
          const int bc = sub == 0 ? b : a;
          if (sub == 1 && a == b) break;
          uint64_t off = 0;
          for (int m = 0; m < n; m++) {
            if (!(fd_mask_ >> m & 1)) continue;
            l.fd_off[sub][m] = off;
            off += gBinom[n - 1 - m][wc - 1] * gBinom[n - wc][bc];
          }
          l.sub_size[sub] = off;
        }
      }
      l.total = l.sub_size[0] + l.sub_size[1];
      layer_id_[a][b] = static_cast<int>(layers_.size());
      layers_.push_back(l);
    }
}

uint64_t Indexer::IndexOf(const Layer& l, uint64_t white,
                          uint64_t black) const {
  const int n = geom.N;
  const int wc = __builtin_popcountll(white);
  const int bc = __builtin_popcountll(black);
  const int sub = (wc == l.a) ? 0 : 1;
  const uint64_t base = sub == 0 ? 0 : l.sub_size[0];
  if (!l.sym) {
    return base + RankSet(white, wc) * gBinom[n - wc][bc] +
           RankRestricted(black, white, bc);
  }
  const int m = __builtin_ctzll(white);
  const uint64_t rest = white & ~(1ull << m);
  // Rank of the remaining white stones over points > m (shifted colex).
  uint64_t rank_rest = 0;
  uint64_t bits = rest;
  int i = 0;
  while (bits) {
    const int p = __builtin_ctzll(bits);
    bits &= bits - 1;
    i++;
    rank_rest += gBinom[p - m - 1][i];
  }
  return base + l.fd_off[sub][m] + rank_rest * gBinom[n - wc][bc] +
         RankRestricted(black, white, bc);
}

void Indexer::BoardOf(const Layer& l, uint64_t idx, uint64_t* white,
                      uint64_t* black) const {
  const int n = geom.N;
  int sub = 0;
  if (idx >= l.sub_size[0]) {
    sub = 1;
    idx -= l.sub_size[0];
  }
  const int wc = sub == 0 ? l.a : l.b;
  const int bc = sub == 0 ? l.b : l.a;
  if (!l.sym) {
    const uint64_t wr = idx / gBinom[n - wc][bc];
    const uint64_t br = idx % gBinom[n - wc][bc];
    *white = UnrankSet(wr, wc, n);
    *black = UnrankRestricted(br, *white, bc, n);
    return;
  }
  // Locate the first-white-stone block: largest m in FD with fd_off <= idx.
  int m = -1;
  for (int p = 0; p < n; p++) {
    if (!(fd_mask_ >> p & 1)) continue;
    if (l.fd_off[sub][p] <= idx) m = p;
  }
  if (m < 0) throw std::runtime_error("BoardOf: bad sym-fold index");
  uint64_t rem = idx - l.fd_off[sub][m];
  const uint64_t wr = rem / gBinom[n - wc][bc];
  const uint64_t br = rem % gBinom[n - wc][bc];
  // Unrank the remaining white stones over points > m (shifted colex).
  uint64_t rest = 0;
  uint64_t r = wr;
  int p = n - 1;
  for (int i = wc - 1; i >= 1; i--) {
    while (gBinom[p - m - 1][i] > r) p--;
    r -= gBinom[p - m - 1][i];
    rest |= 1ull << p;
    p--;
  }
  *white = rest | (1ull << m);
  *black = UnrankRestricted(br, *white, bc, n);
}

void Indexer::Canon(uint64_t* white, uint64_t* black) const {
  const uint64_t w0 = *white, b0 = *black;
  uint64_t best_w = w0, best_b = b0;
  int best_m = __builtin_ctzll(w0);
  for (int i = 1; i < nsym_; i++) {
    const int g = sym_ids_[i];
    uint64_t wt = 0, bt = 0;
    uint64_t bits = w0;
    while (bits) {
      const int p = __builtin_ctzll(bits);
      bits &= bits - 1;
      wt |= 1ull << geom.perm[g][p];
    }
    bits = b0;
    while (bits) {
      const int p = __builtin_ctzll(bits);
      bits &= bits - 1;
      bt |= 1ull << geom.perm[g][p];
    }
    const int m = __builtin_ctzll(wt);
    if (m < best_m ||
        (m == best_m && (wt < best_w || (wt == best_w && bt < best_b)))) {
      best_m = m;
      best_w = wt;
      best_b = bt;
    }
  }
  *white = best_w;
  *black = best_b;
}

bool Indexer::IsRep(uint64_t white, uint64_t black) const {
  const int m = __builtin_ctzll(white);
  if (!(fd_mask_ >> m & 1)) return false;
  uint64_t w = white, b = black;
  Canon(&w, &b);
  return w == white && b == black;
}

}  // namespace fan
