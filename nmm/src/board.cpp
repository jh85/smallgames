#include "board.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <stdexcept>

GameSpec gameSpec(int id, int sevenVariant) {
  switch (id) {
    case 3:  return {3,  'a', 1, true,  false, 3,  false, false, 8,  8,  16, 0};
    case 5:  return {5,  'b', 2, false, false, 5,  false, false, 16, 8,  20, 0};
    case 6:  return {6,  'b', 2, false, false, 6,  false, false, 16, 8,  20, 0};
    case 7:  return {7,  'c', 2, true,  false, 7,  true,  false, 8,  -1, 24, sevenVariant};
    case 9:  return {9,  'd', 3, false, false, 9,  true,  false, 16, 16, 32, 0};
    case 11: return {11, 'e', 3, false, true,  11, true,  false, 16, 20, 40, 0};
    case 12: return {12, 'e', 3, false, true,  12, true,  true,  16, 20, 40, 0};
    case 16: return {16, 'f', 4, false, true,  16, true,  true,  16, 32, 56, 0};
    default: throw std::runtime_error("unknown game");
  }
}

static const char* DIRN[8] = {"NW", "N", "NE", "E", "SE", "S", "SW", "W"};

Board buildBoard(const GameSpec& spec) {
  Board bd;
  bd.spec = spec;
  int R = spec.rings;
  bd.m = R * 8 + (spec.center ? 1 : 0);
  int ctr = R * 8;
  bd.adj.assign(bd.m, 0);
  auto addEdge = [&](int a, int b) { bd.adj[a] |= 1u << b; bd.adj[b] |= 1u << a; };
  for (int r = 0; r < R; ++r) {
    for (int d = 0; d < 8; ++d) addEdge(r * 8 + d, r * 8 + (d + 1) % 8);
    bd.names.resize(bd.m);
  }
  for (int r = 0; r + 1 < R; ++r)
    for (int d = 0; d < 8; ++d)
      if (spec.allSpokes || (d & 1)) addEdge(r * 8 + d, (r + 1) * 8 + d);
  if (spec.center) {
    if (spec.boardType == 'a')
      for (int d = 0; d < 8; ++d) addEdge(d, ctr);          // 3MM: center on all 8 lines
    else
      for (int d = 1; d < 8; d += 2) addEdge((R - 1) * 8 + d, ctr);  // 7MM: inner mids
  }
  // names
  bd.names.assign(bd.m, "");
  for (int r = 0; r < R; ++r)
    for (int d = 0; d < 8; ++d)
      bd.names[r * 8 + d] = std::string(1, 'A' + r) + DIRN[d];
  if (spec.center) bd.names[ctr] = "CTR";

  // mills
  auto addMill = [&](int a, int b, int c) {
    bd.millPts.push_back({(u8)a, (u8)b, (u8)c});
    bd.millMask.push_back((1u << a) | (1u << b) | (1u << c));
  };
  for (int r = 0; r < R; ++r)
    for (int d = 0; d < 8; d += 2)
      addMill(r * 8 + d, r * 8 + d + 1, r * 8 + (d + 2) % 8);   // ring sides
  if (spec.boardType == 'a') {
    for (int d = 0; d < 4; ++d) addMill(d, ctr, d + 4);         // lines through center
  } else if (spec.boardType == 'c') {
    // 7MM candidate center-mill sets (variant decided against paper Table 7)
    if (spec.sevenMillVariant == 0 || spec.sevenMillVariant == 1)
      for (int d = 1; d < 8; d += 2) addMill(0 * 8 + d, 1 * 8 + d, ctr);   // spoke+center
    if (spec.sevenMillVariant == 1 || spec.sevenMillVariant == 2)
      for (int d = 1; d < 4; d += 2) addMill(1 * 8 + d, ctr, 1 * 8 + d + 4); // through-center
  } else if (!spec.center) {
    for (int d = 0; d < 8; ++d) {
      if (!spec.allSpokes && !(d & 1)) continue;
      for (int r = 0; r + 2 < R; ++r)
        addMill(r * 8 + d, (r + 1) * 8 + d, (r + 2) * 8 + d);   // spoke mills (3 in a row)
    }
  }
  bd.millsOfPoint.assign(bd.m, 0);
  for (int i = 0; i < (int)bd.millMask.size(); ++i)
    for (int p = 0; p < bd.m; ++p)
      if (bd.millMask[i] >> p & 1) bd.millsOfPoint[p] |= 1u << i;

  // symmetries: candidates = dihedral(8) x ringflip(2); keep adjacency+mill preserving
  static const int RF[8] = {2, 1, 0, 7, 6, 5, 4, 3};
  auto sortedMills = [&](const std::vector<u32>& mm) {
    std::vector<u32> v = mm;
    std::sort(v.begin(), v.end());
    return v;
  };
  std::vector<u32> millRef = sortedMills(bd.millMask);
  for (int flip = 0; flip < 2; ++flip)
    for (int k = 0; k < 8; ++k) {
      std::array<u8, 32> pm{};
      for (int p = 0; p < bd.m; ++p) {
        if (p == ctr && spec.center) { pm[p] = (u8)p; continue; }
        int r = p / 8, d = p % 8;
        int d2 = (k < 4) ? (d + 2 * k) & 7 : (RF[d] + 2 * (k - 4)) & 7;
        int r2 = flip ? R - 1 - r : r;
        pm[p] = (u8)(r2 * 8 + d2);
      }
      // preservation checks
      bool ok = true;
      for (int p = 0; p < bd.m && ok; ++p) {
        u32 a2 = 0;
        for (int q = 0; q < bd.m; ++q)
          if (bd.adj[p] >> q & 1) a2 |= 1u << pm[q];
        if (a2 != bd.adj[pm[p]]) ok = false;
      }
      if (ok) {
        std::vector<u32> mm;
        for (u32 mk : bd.millMask) {
          u32 t = 0;
          for (int p = 0; p < bd.m; ++p)
            if (mk >> p & 1) t |= 1u << pm[p];
          mm.push_back(t);
        }
        if (sortedMills(mm) != millRef) ok = false;
      }
      if (ok && !(flip && R == 1)) bd.perms.push_back(pm);   // R==1: flip duplicates identity set
    }
  // dedupe (safety) and put identity first
  std::sort(bd.perms.begin(), bd.perms.end());
  bd.perms.erase(std::unique(bd.perms.begin(), bd.perms.end()), bd.perms.end());
  for (size_t i = 0; i < bd.perms.size(); ++i) {
    bool ident = true;
    for (int p = 0; p < bd.m; ++p)
      if (bd.perms[i][p] != p) ident = false;
    if (ident) { std::swap(bd.perms[0], bd.perms[i]); break; }
  }
  // mill-hypergraph-preserving group (for 3-3 flying subsets), by backtracking
  {
    std::vector<u32> millSetSorted = sortedMills(bd.millMask);
    std::vector<int> map(bd.m, -1), used(bd.m, 0);
    std::function<void(int)> rec = [&](int p) {
      if (p == bd.m) {
        std::vector<u32> mm;
        for (u32 mk : bd.millMask) {
          u32 t = 0;
          for (int q = 0; q < bd.m; ++q)
            if (mk >> q & 1) t |= 1u << map[q];
          mm.push_back(t);
        }
        std::sort(mm.begin(), mm.end());
        if (mm == millSetSorted) {
          std::array<u8, 32> pm{};
          for (int q = 0; q < bd.m; ++q) pm[q] = (u8)map[q];
          bd.millPerms.push_back(pm);
        }
        return;
      }
      for (int q = 0; q < bd.m; ++q) {
        if (used[q] || __builtin_popcount(bd.millsOfPoint[q]) !=
                           __builtin_popcount(bd.millsOfPoint[p]))
          continue;
        map[p] = q;
        used[q] = 1;
        bool ok = true;
        for (u32 mk : bd.millMask) {
          if (!(mk >> p & 1)) continue;
          u32 t = 0;
          bool complete = true;
          for (int r = 0; r < bd.m; ++r)
            if (mk >> r & 1) {
              if (map[r] < 0) { complete = false; break; }
              t |= 1u << map[r];
            }
          if (complete &&
              !std::binary_search(millSetSorted.begin(), millSetSorted.end(), t)) {
            ok = false;
            break;
          }
        }
        if (ok) rec(p + 1);
        used[q] = 0;
        map[p] = -1;
      }
    };
    rec(0);
    std::sort(bd.millPerms.begin(), bd.millPerms.end());
    for (size_t i = 0; i < bd.millPerms.size(); ++i) {
      bool ident = true;
      for (int p = 0; p < bd.m; ++p)
        if (bd.millPerms[i][p] != p) ident = false;
      if (ident) { std::swap(bd.millPerms[0], bd.millPerms[i]); break; }
    }
  }
  // byte LUTs
  bd.permLut.resize(bd.perms.size());
  for (size_t s = 0; s < bd.perms.size(); ++s)
    for (int by = 0; by < 4; ++by)
      for (int v = 0; v < 256; ++v) {
        u32 out = 0;
        for (int bit = 0; bit < 8; ++bit)
          if (v >> bit & 1) {
            int p = by * 8 + bit;
            if (p < bd.m) out |= 1u << bd.perms[s][p];
          }
        bd.permLut[s][by][v] = out;
      }
  for (int by = 0; by < 4; ++by)
    for (int v = 0; v < 256; ++v) {
      u64 cw = 0, cb = 0;
      for (int bit = 0; bit < 8; ++bit)
        if (v >> bit & 1) {
          int p = by * 8 + bit;
          if (p < bd.m) {
            cw |= 1ull << (2 * (bd.m - 1 - p) + 1);
            cb |= 1ull << (2 * (bd.m - 1 - p));
          }
        }
      bd.codeLutW[by][v] = cw;
      bd.codeLutB[by][v] = cb;
    }
  bd.millPermLut.resize(bd.millPerms.size());
  for (size_t s = 0; s < bd.millPerms.size(); ++s)
    for (int by = 0; by < 4; ++by)
      for (int v = 0; v < 256; ++v) {
        u32 out = 0;
        for (int bit = 0; bit < 8; ++bit)
          if (v >> bit & 1) {
            int p = by * 8 + bit;
            if (p < bd.m) out |= 1u << bd.millPerms[s][p];
          }
        bd.millPermLut[s][by][v] = out;
      }
  return bd;
}

int Board::minMillEvents(u32 mask) const {
  // complete mills of this mask
  std::vector<u32> full;
  for (u32 mk : millMask)
    if ((mask & mk) == mk) full.push_back(mk);
  if (full.empty()) return 0;
  // exact minimum hitting set (branch on first uncovered mill's 3 points)
  int best = (int)full.size();
  std::function<void(u32, int)> rec = [&](u32 chosen, int used) {
    if (used >= best) return;
    u32 uncovered = 0;
    bool any = false;
    for (u32 mk : full)
      if (!(mk & chosen)) { uncovered = mk; any = true; break; }
    if (!any) { best = used; return; }
    u32 t = uncovered;
    while (t) {
      u32 bit = t & (~t + 1);
      t ^= bit;
      rec(chosen | bit, used + 1);
    }
  };
  rec(0, 0);
  return best;
}

u64 Board::boardHash() const {
  u64 h = 0x9E3779B97F4A7C15ull ^ (u64)m;
  auto mix = [&](u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); };
  for (int p = 0; p < m; ++p) mix(adj[p]);
  for (u32 mk : millMask) mix(mk);
  for (auto& pm : perms)
    for (int p = 0; p < m; ++p) mix(pm[p]);
  mix((u64)spec.pieces << 8 | (spec.flying ? 1 : 0) | (spec.fullBoardDraw ? 2 : 0));
  return h;
}

void Board::writeConfigJson(const std::string& path) const {
  FILE* f = fopen(path.c_str(), "w");
  if (!f) throw std::runtime_error("cannot write " + path);
  fprintf(f, "{\n  \"game\": %d, \"board\": \"%c\", \"points\": %d, \"pieces\": %d,\n",
          spec.id, spec.boardType, m, spec.pieces);
  fprintf(f, "  \"flying\": %s, \"full_board_draw\": %s,\n",
          spec.flying ? "true" : "false", spec.fullBoardDraw ? "true" : "false");
  fprintf(f, "  \"names\": [");
  for (int p = 0; p < m; ++p) fprintf(f, "%s\"%s\"", p ? "," : "", names[p].c_str());
  fprintf(f, "],\n  \"edges\": [");
  bool first = true;
  for (int p = 0; p < m; ++p)
    for (int q = p + 1; q < m; ++q)
      if (adj[p] >> q & 1) {
        fprintf(f, "%s[%d,%d]", first ? "" : ",", p, q);
        first = false;
      }
  fprintf(f, "],\n  \"mills\": [");
  for (size_t i = 0; i < millPts.size(); ++i)
    fprintf(f, "%s[%d,%d,%d]", i ? "," : "", millPts[i][0], millPts[i][1], millPts[i][2]);
  fprintf(f, "],\n  \"symmetries\": [\n");
  for (size_t s = 0; s < perms.size(); ++s) {
    fprintf(f, "    [");
    for (int p = 0; p < m; ++p) fprintf(f, "%s%d", p ? "," : "", perms[s][p]);
    fprintf(f, "]%s\n", s + 1 < perms.size() ? "," : "");
  }
  fprintf(f, "  ],\n  \"board_hash\": %llu\n}\n", (unsigned long long)boardHash());
  fclose(f);
}

int countGraphAutomorphisms(const Board& bd) {
  // brute force with degree pruning (diagnostic only; boards are small)
  int m = bd.m;
  std::vector<int> deg(m);
  for (int p = 0; p < m; ++p) deg[p] = __builtin_popcount(bd.adj[p]);
  std::vector<int> map(m, -1), used(m, 0);
  int count = 0;
  std::function<void(int)> rec = [&](int p) {
    if (p == m) { ++count; return; }
    for (int q = 0; q < m; ++q) {
      if (used[q] || deg[q] != deg[p]) continue;
      bool ok = true;
      for (int r = 0; r < p && ok; ++r) {
        bool e1 = bd.adj[p] >> r & 1, e2 = bd.adj[q] >> map[r] & 1;
        if (e1 != e2) ok = false;
      }
      if (!ok) continue;
      map[p] = q; used[q] = 1;
      rec(p + 1);
      used[q] = 0; map[p] = -1;
    }
  };
  rec(0);
  return count;
}
