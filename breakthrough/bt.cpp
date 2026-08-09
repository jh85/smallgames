// bt.cpp — Strong solver for small Breakthrough boards via a ZDD minimal perfect hash
// over (pieces0, pieces1, advancement) slabs and a single-pass backward solve.
//
// Rules (verified against /data1/dfa-games BreakthroughGame.cpp): side 0 moves first,
// advancing +1 rank per move toward rank H-1; side 1 advances toward rank 0. Straight
// moves to empty squares only; diagonal moves to empty or capturing an enemy. Reaching
// the far rank wins; a player with no legal move loses (normal play). No draws.
//
// State space (canonical, no symmetry reduction): side-0 pieces anywhere on ranks
// 0..H-2, side-1 pieces on ranks 1..H-1, disjoint, 1..2W pieces each, explicit side to
// move. Terminal positions (goal reached / zero pieces) are excluded and recognized
// inline. Acyclic layering: adv = sum of side-0 ranks + sum of (H-1 - side-1 ranks);
// every move increases adv by 1 (non-capture, same piece counts) or moves to a smaller
// piece-count partition (capture). Solve order: (A,B) ascending by A+B, adv descending
// within; one sweep per slab, no fixpoint. Values: 1 bit (0=LOSS, 1=WIN, side to move).
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cinttypes>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <algorithm>
#include <mutex>
#include <memory>
#include <functional>
#include <unordered_map>
#include <sys/stat.h>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;

static std::string commas(u64 v) {
  std::string s = std::to_string(v), r; int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) { r += s[i]; if (++c % 3 == 0 && i) r += ','; }
  std::reverse(r.begin(), r.end()); return r;
}
static double nowS() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void die(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); exit(1);
}

// ---------------- board ----------------
static int W = 5, H = 6, CELLS = 30, NP = 10;   // NP = starting pieces per side (2W)
static int cellOf(int r, int c) { return r * W + c; }
static int rankOf(int cell) { return cell / W; }
// advancement contribution of a piece of side s at cell
static int advOf(int s, int cell) { return s == 0 ? rankOf(cell) : (H - 1 - rankOf(cell)); }

// ---------------- shared ZDD forest over (A,B,adv) roots ----------------
// items: d = cell*2 + (0: side0 piece here, 1: side1 piece here), cell order 0..CELLS-1.
// frontier: (remA, remB, remAdv) after deciding items 0..d-1 (+ overlap flag mid-cell).
struct Forest {
  std::vector<u8> var_;
  std::vector<u32> lo_, hi_;
  std::vector<u64> cntlo_;
  std::map<std::array<int, 3>, u32> roots;   // (A,B,adv) -> root
  std::map<std::array<int, 3>, u64> counts;
  int maxAdvAll = 0;

  std::unordered_map<u64, u32> memo, uniq;
  u32 rec(int d, int remA, int remB, int remAdv, int f) {
    if (d == CELLS * 2) return (remA == 0 && remB == 0 && remAdv == 0) ? 1u : 0u;
    int cell = d / 2, side = d % 2;
    // feasibility pruning: remaining cells can still supply counts and adv
    {
      int cellsLeft = CELLS - cell;
      if (remA + remB > cellsLeft) return 0;
      // adv bounds: compute max/min possible additional adv (rough but sound: use
      // per-piece max contribution H-1)
      if (remAdv < 0 || remAdv > (remA + remB) * (H - 1)) return 0;
    }
    u64 key = (u64)d << 24 | (u64)remA << 19 | (u64)remB << 14 | (u64)remAdv << 1 | f;
    auto it = memo.find(key);
    if (it != memo.end()) return it->second;
    // 0-branch
    u32 l = rec(d + 1, remA, remB, remAdv, side == 0 ? f : 0);
    // 1-branch: place a piece of `side` at cell (if allowed)
    u32 h = 0;
    bool allowed = !(side == 1 && f);                       // overlap
    if (side == 0 && rankOf(cell) == H - 1) allowed = false;  // side0 never on far rank
    if (side == 1 && rankOf(cell) == 0) allowed = false;
    if (allowed) {
      int a2 = remA - (side == 0), b2 = remB - (side == 1);
      int adv2 = remAdv - advOf(side, cell);
      if (a2 >= 0 && b2 >= 0 && adv2 >= 0)
        h = rec(d + 1, a2, b2, adv2, side == 0 ? 1 : 0);
    }
    u32 res;
    if (h == 0) res = l;
    else {
      u64 hk = (u64)d << 56 | (u64)l << 28 | h;
      auto it2 = uniq.find(hk);
      if (it2 != uniq.end()) res = it2->second;
      else {
        res = (u32)var_.size();
        var_.push_back((u8)d); lo_.push_back(l); hi_.push_back(h);
        uniq.emplace(hk, res);
      }
    }
    memo.emplace(key, res);
    return res;
  }

  void build() {
    var_.assign(2, (u8)(CELLS * 2)); lo_.assign(2, 0); hi_.assign(2, 0);
    maxAdvAll = 0;
    for (int A = 1; A <= NP; ++A)
      for (int B = 1; B <= NP; ++B) {
        int maxAdv = A * (H - 2) + B * (H - 2);   // pieces capped one short of goal
        for (int adv = 0; adv <= maxAdv; ++adv) {
          u32 r = rec(0, A, B, adv, 0);
          if (r) {
            roots[{A, B, adv}] = r;
            if (adv > maxAdvAll) maxAdvAll = adv;
          }
        }
      }
    memo.clear(); uniq.clear();
    std::vector<u64> cnt(var_.size());
    cnt[0] = 0; cnt[1] = 1;
    for (u32 i = 2; i < (u32)var_.size(); ++i) cnt[i] = cnt[lo_[i]] + cnt[hi_[i]];
    cntlo_.assign(var_.size(), 0);
    for (u32 i = 2; i < (u32)var_.size(); ++i) cntlo_[i] = cnt[lo_[i]];
    for (auto& [k, r] : roots) counts[k] = cnt[r];
  }

  u64 rank(u32 root, u32 m0, u32 m1) const {
    u32 n = root; u64 k = 0;
    while (n > 1) {
      int cell = var_[n] / 2, side = var_[n] % 2;
      u32 bit = side == 0 ? (m0 >> cell) & 1 : (m1 >> cell) & 1;
      if (bit) { k += cntlo_[n]; n = hi_[n]; }
      else n = lo_[n];
    }
    return n == 1 ? k : UINT64_MAX;
  }
  void unrank(u32 root, u64 k, u32& m0, u32& m1) const {
    m0 = m1 = 0;
    u32 n = root;
    while (n > 1) {
      int cell = var_[n] / 2, side = var_[n] % 2;
      if (cntlo_[n] <= k) {
        k -= cntlo_[n];
        if (side == 0) m0 |= 1u << cell; else m1 |= 1u << cell;
        n = hi_[n];
      } else n = lo_[n];
    }
    assert(n == 1 && k == 0);
  }
};

// ---------------- solver ----------------
struct Slab {
  std::vector<u8> bits;   // 1 bit per state, idx = confIdx*2 + stm
  u64 n = 0;
  void init(u64 states) { n = states; bits.assign((states + 7) / 8, 0); }
  int get(u64 i) const { return (bits[i >> 3] >> (i & 7)) & 1; }
  void set(u64 i) { bits[i >> 3] |= (u8)(1 << (i & 7)); }
};

struct Solver {
  bool keepAll = false;
  Forest F;
  std::map<std::array<int, 3>, Slab> tabs;
  int threads = 64;
  std::string dir;
  u64 totalStates = 0;

  // value of successor position (masks after the move), for the side now to move
  // returns 1 if that side WINS. Terminal cases resolved inline by the caller.
  int lookup(u32 m0, u32 m1, int stm) {
    int A = __builtin_popcount(m0), B = __builtin_popcount(m1);
    int adv = 0;
    u32 t = m0;
    while (t) { int c = __builtin_ctz(t); t &= t - 1; adv += advOf(0, c); }
    t = m1;
    while (t) { int c = __builtin_ctz(t); t &= t - 1; adv += advOf(1, c); }
    auto it = F.roots.find({A, B, adv});
    if (it == F.roots.end()) die("missing root %d %d %d", A, B, adv);
    u64 k = F.rank(it->second, m0, m1);
    if (k == UINT64_MAX) die("rank failed");
    return tabs.at({A, B, adv}).get(k * 2 + stm);
  }

  // evaluate state: returns 1 if stm wins
  int evalState(u32 m0, u32 m1, int stm) {
    u32 own = stm == 0 ? m0 : m1, opp = stm == 0 ? m1 : m0;
    u32 occ = m0 | m1;
    int dir = stm == 0 ? 1 : -1, goal = stm == 0 ? H - 1 : 0;
    bool any = false;
    u32 t = own;
    while (t) {
      int c = __builtin_ctz(t);
      t &= t - 1;
      int r = rankOf(c), col = c % W, r2 = r + dir;
      if (r2 < 0 || r2 >= H) continue;
      for (int dc = -1; dc <= 1; ++dc) {
        int c2 = col + dc;
        if (c2 < 0 || c2 >= W) continue;
        int cell2 = cellOf(r2, c2);
        u32 b2 = 1u << cell2;
        bool isCap = false;
        if (dc == 0) {
          if (occ & b2) continue;              // straight: empty only
        } else {
          if (own & b2) continue;              // diagonal: empty or capture
          isCap = (opp & b2) != 0;
        }
        any = true;
        if (r2 == goal) return 1;              // reaches far rank: win
        if (isCap && __builtin_popcount(opp) == 1) return 1;   // captures last piece
        u32 n0 = m0, n1 = m1;
        if (stm == 0) { n0 = (m0 ^ (1u << c)) | b2; n1 = m1 & ~b2; }
        else { n1 = (m1 ^ (1u << c)) | b2; n0 = m0 & ~b2; }
        if (!lookup(n0, n1, stm ^ 1)) return 1;   // opponent loses there
      }
    }
    (void)any;
    return 0;   // no winning move (includes stalemate: no moves at all => loss)
  }

  std::string slabPath(int A, int B, int adv) const {
    char p[256];
    snprintf(p, sizeof p, "%s/bt%dx%d_s%02d_%02d_%03d.bin", dir.c_str(), W, H, A, B, adv);
    return p;
  }
  void writeSlab(int A, int B, int adv, const Slab& tab) {
    std::string p = slabPath(A, B, adv);
    FILE* f = fopen((p + ".tmp").c_str(), "wb");
    u64 hdr[4] = {0x4254534C41420001ull, (u64)W << 8 | H,
                  (u64)A << 32 | (u64)B << 16 | (u64)adv, tab.n};
    fwrite(hdr, 8, 4, f);
    fwrite(tab.bits.data(), 1, tab.bits.size(), f);
    fclose(f);
    rename((p + ".tmp").c_str(), p.c_str());
  }
  bool loadSlab(int A, int B, int adv, Slab& tab, u64 states) {
    FILE* f = fopen(slabPath(A, B, adv).c_str(), "rb");
    if (!f) return false;
    u64 hdr[4];
    if (fread(hdr, 8, 4, f) != 4 || hdr[0] != 0x4254534C41420001ull || hdr[3] != states) {
      fclose(f);
      return false;
    }
    tab.init(states);
    bool ok = fread(tab.bits.data(), 1, tab.bits.size(), f) == tab.bits.size();
    fclose(f);
    return ok;
  }
  void solve() {
    mkdir(dir.c_str(), 0755);
    double t0 = nowS();
    for (int T = 2; T <= 2 * NP; ++T) {
      double tb = nowS();
      u64 bandStates = 0;
      for (int A = 1; A <= NP; ++A) {
        int B = T - A;
        if (B < 1 || B > NP) continue;
        for (int adv = F.maxAdvAll; adv >= 0; --adv) {
          auto it = F.roots.find({A, B, adv});
          if (it == F.roots.end()) continue;
          u64 n = F.counts[{A, B, adv}], states = n * 2;
          Slab& tab = tabs[{A, B, adv}];
          totalStates += states;
          bandStates += states;
          if (loadSlab(A, B, adv, tab, states)) continue;   // resume
          tab.init(states);
          std::atomic<u64> chunk{0};
          const u64 CH = 1024;
          std::vector<std::thread> th;
          int nth = states < 4096 ? 1 : threads;
          for (int t = 0; t < nth; ++t)
            th.emplace_back([&] {
              u64 ch;
              while ((ch = chunk.fetch_add(1)) * CH < n) {
                u64 lo = ch * CH, hi = std::min(n, lo + CH);
                for (u64 i = lo; i < hi; ++i) {
                  u32 m0, m1;
                  F.unrank(it->second, i, m0, m1);
                  for (int stm = 0; stm < 2; ++stm)
                    if (evalState(m0, m1, stm)) {
                      // set with byte-level race avoided: chunk-local, but two stm bits
                      // share a byte only within this thread's i range
                      tab.set(i * 2 + stm);
                    }
                }
              }
            });
          for (auto& x : th) x.join();
          writeSlab(A, B, adv, tab);
        }
      }
      // evict band T-1 (only bands T and T+1 reference it; T is done)
      if (T >= 3 && !keepAll)
        for (int A = 1; A <= NP; ++A) {
          int B = T - 1 - A;
          if (B < 1 || B > NP) continue;
          for (auto it2 = tabs.lower_bound({A, B, 0});
               it2 != tabs.end() && (*it2).first[0] == A && (*it2).first[1] == B;)
            it2 = tabs.erase(it2);
        }
      printf("[band %2d] cumulative states=%s band=%s (%.1fs)\n", T,
             commas(totalStates).c_str(), commas(bandStates).c_str(), nowS() - tb);
      fflush(stdout);
    }
    printf("[solve] %s states solved in %.1fs\n", commas(totalStates).c_str(), nowS() - t0);
  }

  void saveAll() {
    std::string path = dir + "/bt_" + std::to_string(W) + "x" + std::to_string(H) + ".wdl";
    FILE* f = fopen((path + ".tmp").c_str(), "wb");
    u64 hdr[4] = {0x4254574C00000001ull, (u64)W << 8 | (u64)H, tabs.size(), totalStates};
    fwrite(hdr, 8, 4, f);
    for (auto& [k, tab] : tabs) {
      u64 rh[4] = {(u64)k[0], (u64)k[1], (u64)k[2], tab.n};
      fwrite(rh, 8, 4, f);
      fwrite(tab.bits.data(), 1, tab.bits.size(), f);
    }
    fclose(f);
    rename((path + ".tmp").c_str(), path.c_str());
    printf("[save] %s\n", path.c_str());
  }
};

// ---------------- independent negamax (validation) ----------------
namespace bf {
static std::unordered_map<u64, int> memo;
static int negamax(u32 m0, u32 m1, int stm) {
  u64 key = ((u64)m0 << 32 | m1) << 1 | stm;
  auto it = memo.find(key);
  if (it != memo.end()) return it->second;
  u32 own = stm == 0 ? m0 : m1, occ = m0 | m1;
  int dir = stm == 0 ? 1 : -1, goal = stm == 0 ? H - 1 : 0;
  int res = 0;
  u32 t = own;
  while (t && !res) {
    int c = __builtin_ctz(t);
    t &= t - 1;
    int r = rankOf(c), col = c % W, r2 = r + dir;
    if (r2 < 0 || r2 >= H) continue;
    for (int dc = -1; dc <= 1 && !res; ++dc) {
      int c2 = col + dc;
      if (c2 < 0 || c2 >= W) continue;
      int cell2 = cellOf(r2, c2);
      u32 b2 = 1u << cell2;
      if (dc == 0) { if (occ & b2) continue; }
      else if (own & b2) continue;
      if (r2 == goal) { res = 1; break; }
      u32 n0 = m0, n1 = m1;
      if (stm == 0) { n0 = (m0 ^ (1u << c)) | b2; n1 = m1 & ~b2; }
      else { n1 = (m1 ^ (1u << c)) | b2; n0 = m0 & ~b2; }
      if ((stm == 0 ? n1 : n0) == 0) { res = 1; break; }
      if (!negamax(n0, n1, stm ^ 1)) res = 1;
    }
  }
  memo[key] = res;
  return res;
}
}  // namespace bf

int main(int argc, char** argv) {
  std::string cmd = argc > 1 ? argv[1] : "";
  int w = 5, h = 6, threads = 64;
  // Relative to the working directory, so the tree can be moved without editing source.
  std::string dir = "data";
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--board" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
    else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
    else if (a == "--dir" && i + 1 < argc) dir = argv[++i];
  }
  W = w; H = h; CELLS = W * H; NP = 2 * W;
  printf("=== Breakthrough %dx%d (width x ranks): %d cells, %d pieces/side, side 0 "
         "moves first toward rank %d ===\n", W, H, CELLS, NP, H - 1);
  if (cmd == "count" || cmd == "solve" || cmd == "selftest") {
    Solver sv;
    sv.keepAll = (cmd == "selftest");
    sv.threads = threads;
    sv.dir = dir;
    double t0 = nowS();
    sv.F.build();
    u64 tot = 0, maxSlab = 0;
    for (auto& [k, c] : sv.F.counts) { tot += c; maxSlab = std::max(maxSlab, c); }
    printf("[forest] nodes=%s slabs=%s configs=%s (x2 stm) largest-slab=%s (%.1fs)\n",
           commas(sv.F.var_.size()).c_str(), commas(sv.F.roots.size()).c_str(),
           commas(tot).c_str(), commas(maxSlab).c_str(), nowS() - t0);
    printf("[forest] WDL at 1 bit/state: %s bytes\n", commas(tot * 2 / 8).c_str());
    if (cmd == "count") {
      std::map<int, u64> band;
      for (auto& [k, c] : sv.F.counts) band[k[0] + k[1]] += c;
      u64 peak = 0;
      for (auto& [T, c] : band) {
        u64 pair = c + (band.count(T - 1) ? band[T - 1] : 0);
        peak = std::max(peak, pair);
        printf("[band %2d] configs=%s bytes=%s\n", T, commas(c).c_str(),
               commas(c * 2 / 8).c_str());
      }
      printf("[peak] two adjacent bands: %s bytes\n", commas(peak * 2 / 8).c_str());
      return 0;
    }
    sv.solve();
    // initial position value
    u32 m0 = 0, m1 = 0;
    for (int c = 0; c < 2 * W; ++c) m0 |= 1u << c;
    for (int c = CELLS - 2 * W; c < CELLS; ++c) m1 |= 1u << c;
    int v = sv.lookup(m0, m1, 0);
    printf("=== INITIAL POSITION (%dx%d): %s ===\n", W, H,
           v ? "FIRST PLAYER WINS" : "SECOND PLAYER WINS");
    if (cmd == "selftest") {
      // exhaustive comparison with independent negamax over sampled slabs
      std::mt19937_64 rng(1);
      u64 checked = 0, mism = 0;
      for (auto& [k, tab] : sv.tabs) {
        u64 n = tab.n / 2;
        for (int s = 0; s < 40 && n; ++s) {
          u64 i = rng() % n;
          u32 a0, a1;
          sv.F.unrank(sv.F.roots[k], i, a0, a1);
          for (int stm = 0; stm < 2; ++stm) {
            int mine = tab.get(i * 2 + stm);
            int ref = bf::negamax(a0, a1, stm);
            ++checked;
            if (mine != ref && ++mism < 5)
              printf("MISMATCH %d,%d,%d idx=%llu stm=%d mine=%d bf=%d\n", k[0], k[1],
                     k[2], (unsigned long long)i, stm, mine, ref);
            if (bf::memo.size() > 60000000) bf::memo.clear();
          }
        }
      }
      printf("[selftest] %s sampled states vs independent negamax, %s mismatches\n",
             commas(checked).c_str(), commas(mism).c_str());
      if (mism) return 1;
    }
    printf("[done] slabs on disk in %s\n", sv.dir.c_str());
  } else if (cmd == "spotcheck") {
    // validate on-disk slabs for small piece counts against independent negamax
    Solver sv;
    sv.dir = dir;
    sv.F.build();
    std::mt19937_64 rng(2);
    u64 checked = 0, mism = 0, slabs = 0;
    for (auto& [k, root] : sv.F.roots) {
      if (k[0] + k[1] > 7) continue;
      u64 n = sv.F.counts[k];
      Slab tab;
      if (!sv.loadSlab(k[0], k[1], k[2], tab, n * 2))
        die("missing/corrupt slab %d %d %d", k[0], k[1], k[2]);
      ++slabs;
      for (int s = 0; s < 20; ++s) {
        u64 i = rng() % n;
        u32 a0, a1;
        sv.F.unrank(root, i, a0, a1);
        for (int stm = 0; stm < 2; ++stm) {
          int mine = tab.get(i * 2 + stm);
          int ref = bf::negamax(a0, a1, stm);
          ++checked;
          if (mine != ref && ++mism < 5)
            printf("MISMATCH %d,%d,%d idx=%" PRIu64 " stm=%d mine=%d bf=%d\n",
                   k[0], k[1], k[2], i, stm, mine, ref);
          if (bf::memo.size() > 60000000) bf::memo.clear();
        }
      }
    }
    printf("[spotcheck] %s slabs (<=7 pieces), %s states vs negamax, %s mismatches\n",
           commas(slabs).c_str(), commas(checked).c_str(), commas(mism).c_str());
    return mism ? 1 : 0;
  } else
    die("usage: bt count|solve|selftest|spotcheck [--board WxH] [--threads N] [--dir D]");
  return 0;
}
