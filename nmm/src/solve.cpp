#include "solve.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <sys/stat.h>
#include <thread>

static double nowS() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// evaluate one state given a child-value oracle; returns V_*
template <typename F>
static u32 evalState(const Board& bd, u32 w, u32 b, int wh, int bh, int stm, F childVal,
                     std::vector<Succ>& buf) {
  buf.clear();
  genSuccessors(bd, w, b, wh, bh, stm, buf);
  if (buf.empty()) return V_LOSS;   // no legal move loses (phase 2/3; cannot occur while
                                    // the mover still has pieces in hand)
  bool anyUnk = false, anyDraw = false;
  for (const Succ& s : buf) {
    u32 v = childVal(s);
    if (v == V_LOSS) return V_WIN;
    if (v == V_UNK) anyUnk = true;
    else if (v == V_DRAW) anyDraw = true;
  }
  if (anyUnk) return V_UNK;
  return anyDraw ? V_DRAW : V_LOSS;
}

void Solver::solvePhase23() {
  int hiP = f23->nMax;   // may be < N for truncated (endgame-band) solves
  ph23.assign((hiP - 2) * (hiP - 2), WdlTable{});
  // partitions ascending by total pieces
  std::vector<std::pair<int, int>> order;
  for (int T = 6; T <= 2 * hiP; ++T)
    for (int W = 3; W <= hiP; ++W) {
      int B = T - W;
      if (B >= 3 && B <= hiP) order.push_back({W, B});
    }
  mkdir(dir.c_str(), 0755);
  for (auto [W, B] : order) {
    Sel selWB = f23->sel(W, B);
    u64 n = selWB.count, states = n * 2;
    {  // resume: load if already solved
      char path[512];
      snprintf(path, sizeof path, "%s/ph23_w%02d_b%02d.wdl", dir.c_str(), W, B);
      FILE* f = fopen(path, "rb");
      if (f) {
        u64 hdr[4];
        if (fread(hdr, 8, 4, f) == 4 && hdr[0] == 0x4D4F5252574C4402ull &&
            hdr[1] == bd->boardHash() && hdr[2] == states) {
          WdlTable tab;
          tab.n = states;
          tab.bits.resize((states + 3) / 4);
          if (fread(tab.bits.data(), 1, tab.bits.size(), f) == tab.bits.size()) {
            ph23[sidx23(W, B)] = std::move(tab);
            fclose(f);
            if (!quiet) { printf("[ph23 %2d-%-2d] loaded from disk\n", W, B); fflush(stdout); }
            continue;
          }
        }
        fclose(f);
      }
    }
    WdlTable cur, nxt;
    cur.init(states);
    nxt.init(states);
    double t0 = nowS();
    int iters = 0;
    for (;; ++iters) {
      std::atomic<u64> changes{0}, chunk{0};
      const u64 CH = 4096;
      std::vector<std::thread> th;
      for (int t = 0; t < threads; ++t)
        th.emplace_back([&] {
          std::vector<Succ> buf;
          u64 ch;
          u64 local = 0;
          while ((ch = chunk.fetch_add(1)) * CH < states) {
            u64 lo = ch * CH, hi = std::min(states, lo + CH);
            for (u64 i = lo; i < hi; ++i) {
              u32 v = cur.get(i);
              if (v == V_WIN || v == V_LOSS || v == V_DRAW) { nxt.set(i, v); continue; }
              u64 idx = i >> 1;
              int stm = (int)(i & 1);
              u64 r1 = selWB.selUnrank(idx);
              u32 w, b;
              z23->unrank(r1, w, b);
              u32 nv = evalState(*bd, w, b, 0, 0, stm, [&](const Succ& s) -> u32 {
                int W2 = __builtin_popcount(s.w), B2 = __builtin_popcount(s.b);
                int stm2 = stm ^ 1;
                if ((stm2 ? B2 : W2) < 3) return V_LOSS;   // mover reduced opponent to 2
                if ((stm2 ? W2 : B2) < 3) return V_WIN;
                u32 cw = s.w, cb = s.b;
                bd->canonicalizeAuto(cw, cb);
                if (W2 == W && B2 == B) {                  // same partition: prev buffer
                  u64 k = selWB.selRank(z23->rank(cw, cb));
                  return cur.get(k * 2 + stm2);
                }
                return lookup23(cw, cb, stm2);
              }, buf);
              nxt.set(i, nv);
              if (nv != v) ++local;
            }
          }
          changes += local;
        });
      for (auto& x : th) x.join();
      std::swap(cur.bits, nxt.bits);
      if (changes == 0) break;
    }
    // unresolved -> DRAW
    u64 wCnt = 0, dCnt = 0, lCnt = 0;
    for (u64 i = 0; i < states; ++i) {
      u32 v = cur.get(i);
      if (v == V_UNK) { cur.set(i, V_DRAW); v = V_DRAW; }
      if (v == V_WIN) ++wCnt;
      else if (v == V_DRAW) ++dCnt;
      else ++lCnt;
    }
    ph23[sidx23(W, B)] = std::move(cur);
    {  // persist immediately (atomic rename)
      char path[512], tmp[520];
      snprintf(path, sizeof path, "%s/ph23_w%02d_b%02d.wdl", dir.c_str(), W, B);
      snprintf(tmp, sizeof tmp, "%s.tmp", path);
      FILE* f = fopen(tmp, "wb");
      const WdlTable& tab = ph23[sidx23(W, B)];
      u64 hdr[4] = {0x4D4F5252574C4402ull, bd->boardHash(), tab.n, (u64)W << 8 | (u64)B};
      fwrite(hdr, 8, 4, f);
      fwrite(tab.bits.data(), 1, tab.bits.size(), f);
      fclose(f);
      rename(tmp, path);
    }
    if (!quiet)
      printf("[ph23 %2d-%-2d] states=%llu iters=%d W/D/L=%llu/%llu/%llu (%.1fs)\n", W, B,
             (unsigned long long)states, iters, (unsigned long long)wCnt,
             (unsigned long long)dCnt, (unsigned long long)lCnt, nowS() - t0);
    fflush(stdout);
  }
}

void Solver::solvePlacement() {
  for (int H = 1; H <= 2 * N; ++H) {
    int wh = H / 2, bh = (H + 1) / 2;   // H even: (H/2,H/2) white to move; odd: black
    int stm = (wh == bh) ? 0 : 1;
    plPrev.swap(plCur);
    plCur.clear();
    double t0 = nowS();
    u64 layerStates = 0;
    for (int wb = 0; wb <= N - wh; ++wb)
      for (int bb = 0; bb <= N - bh; ++bb) {
        if (wb + bb > bd->m) continue;
        Sel s = fp->sel(wb, bb);
        if (!s.count) continue;
        WdlTable tab;
        tab.init(s.count);
        layerStates += s.count;
        std::atomic<u64> chunk{0};
        const u64 CH = 2048;
        std::vector<std::thread> th;
        for (int t = 0; t < threads; ++t)
          th.emplace_back([&] {
            std::vector<Succ> buf;
            u64 ch;
            while ((ch = chunk.fetch_add(1)) * CH < s.count) {
              u64 lo = ch * CH, hi = std::min(s.count, lo + CH);
              for (u64 i = lo; i < hi; ++i) {
                u64 r1 = s.selUnrank(i);
                u32 w, b;
                zp->unrank(r1, w, b);
                u32 nv = evalState(*bd, w, b, wh, bh, stm, [&](const Succ& sc) -> u32 {
                  int W2 = __builtin_popcount(sc.w), B2 = __builtin_popcount(sc.b);
                  if (sc.wh + sc.bh == 0) {
                    // entering movement phase; white to move by construction
                    if (bd->spec.fullBoardDraw && W2 + B2 == bd->m) return V_DRAW;
                    if (W2 < 3) return V_LOSS;
                    if (B2 < 3) return V_WIN;
                    return lookup23(sc.w, sc.b, 0);
                  }
                  // still placement: layer H-1 tables
                  u32 cw = sc.w, cb = sc.b;
                  bd->canonicalize(cw, cb);   // board group during placement
                  u64 rr = zp->rank(cw, cb);
                  Sel s2 = fp->sel(W2, B2);
                  u64 k = rr == UINT64_MAX ? UINT64_MAX : s2.selRank(rr);
                  if (k == UINT64_MAX) { ++outOfIndexChildren; return V_DRAW; }
                  auto it = plPrev.find(W2 << 8 | B2);
                  assert(it != plPrev.end());
                  return it->second.get(k);
                }, buf);
                if (nv == V_UNK) nv = V_DRAW;   // cannot happen: layers are acyclic
                tab.set(i, nv);
              }
            }
          });
        for (auto& x : th) x.join();
        if (keepAllPlacement) plAll[{H, wb, bb}] = tab;
        plCur[wb << 8 | bb] = std::move(tab);
      }
    if (!quiet)
      printf("[place H=%2d] hands=(%d,%d) %s-to-move states=%llu (%.1fs)\n", H, wh, bh,
             stm ? "black" : "white", (unsigned long long)layerStates, nowS() - t0);
    fflush(stdout);
    // persist the layer
    mkdir(dir.c_str(), 0755);
    for (auto& [key, tab] : plCur) {
      char path[512];
      snprintf(path, sizeof path, "%s/place_H%02d_w%02d_b%02d.wdl", dir.c_str(), H,
               key >> 8, key & 255);
      FILE* f = fopen(path, "wb");
      u64 hdr[4] = {0x4D4F5252574C4401ull, bd->boardHash(), tab.n,
                    (u64)H << 32 | (u64)key};
      fwrite(hdr, 8, 4, f);
      fwrite(tab.bits.data(), 1, tab.bits.size(), f);
      fclose(f);
    }
  }
  // initial state: empty board, layer 2N
  initialValue = plCur[0].get(0);
}

void Solver::saveTables() const {
  mkdir(dir.c_str(), 0755);
  for (int W = 3; W <= N; ++W)
    for (int B = 3; B <= N; ++B) {
      const WdlTable& tab = ph23[sidx23(W, B)];
      if (!tab.n) continue;
      char path[512];
      snprintf(path, sizeof path, "%s/ph23_w%02d_b%02d.wdl", dir.c_str(), W, B);
      FILE* f = fopen(path, "wb");
      u64 hdr[4] = {0x4D4F5252574C4402ull, bd->boardHash(), tab.n, (u64)W << 8 | (u64)B};
      fwrite(hdr, 8, 4, f);
      fwrite(tab.bits.data(), 1, tab.bits.size(), f);
      fclose(f);
    }
}

bool Solver::loadPhase23() {
  ph23.assign((N - 2) * (N - 2), WdlTable{});
  for (int W = 3; W <= N; ++W)
    for (int B = 3; B <= N; ++B) {
      char path[512];
      snprintf(path, sizeof path, "%s/ph23_w%02d_b%02d.wdl", dir.c_str(), W, B);
      FILE* f = fopen(path, "rb");
      if (!f) return false;
      u64 hdr[4];
      if (fread(hdr, 8, 4, f) != 4 || hdr[0] != 0x4D4F5252574C4402ull ||
          hdr[1] != bd->boardHash()) {
        fclose(f);
        return false;
      }
      WdlTable& tab = ph23[sidx23(W, B)];
      tab.n = hdr[2];
      tab.bits.resize((tab.n + 3) / 4);
      if (fread(tab.bits.data(), 1, tab.bits.size(), f) != tab.bits.size()) {
        fclose(f);
        return false;
      }
      fclose(f);
    }
  return true;
}
