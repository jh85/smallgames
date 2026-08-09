// minigo.cpp — Solver for Go on small boards (up to 7x7) under Tromp-Taylor rules.
//
// Rules (Tromp-Taylor): area scoring (stones + empty regions reaching only one
// color); game ends after two consecutive passes; positional superko (a play may
// not recreate any earlier board position; passes exempt); suicide allowed by
// default (multi-stone suicide removes the placed chain), toggle with --suicide 0.
//
// Solving approach: Go is cyclic and superko makes move legality history-
// dependent, so a Breakthrough-style acyclic ZDD/MPH backward sweep is unsound
// here. Instead: forward proof search from the root (MIGOS-style). For a
// threshold T we solve the boolean predicate P = "Black's final area score
// (black minus white) >= T" with three-valued (WIN/LOSS/UNKNOWN) iterative-
// deepening AND/OR search:
//   - exact 2N+2-bit position keys (board pair, side, pass flag), no hash
//     collisions; TT keys are folded over the 8 board symmetries.
//   - positional superko enforced exactly against the actual search path (raw
//     boards, not symmetry-folded). Values that depend on a repetition of an
//     ancestor outside a node's own subtree are path-dependent and are NOT
//     stored in the TT (GHI guard: track min repetition ply).
//   - static cutoffs by Benson unconditional life extended to unconditional
//     territory: alive stones plus enclosed regions in which the opponent can
//     never make an eye (every empty adjacent to an alive chain) count as
//     guaranteed area, including trapped opponent stones.
//   - enhanced transposition cutoffs (children probed in the TT before search),
//     staged TT-move, killers, history heuristic, capture-first ordering.
//   - lazy-SMP parallelism: N threads run independent iterative deepening with
//     jittered move ordering over one shared lockless TT (XOR-guarded entries).
// Exact game value = binary search over T (each threshold solved separately).
//
// The WDL dump writes every proven TT entry (canonical key -> WIN/LOSS at the
// solved threshold) to a binary file; see README.md for the format.
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cassert>
#include <cinttypes>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

using u8 = uint8_t;  using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using u128 = unsigned __int128;

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

// ---------------- board geometry ----------------
static int W = 5, H = 5, N = 25;
static bool SUICIDE_OK = true;
static u64 FULL, COL_L, COL_R;
static u64 NEI[64];
static int NSYM = 8;
static int PERM[8][64];
static u64 PERMB[8][8][256];

static inline u64 bit(int c) { return (u64)1 << c; }
static inline u64 expand(u64 m) {
  return ((m >> W) | ((m << W) & FULL) | ((m & ~COL_L) >> 1) | ((m & ~COL_R) << 1)) & FULL;
}
static void initBoard(int w, int h) {
  W = w; H = h; N = W * H;
  if (N > 49) die("board too large (max 7x7)");
  FULL = ((u64)1 << N) - 1;
  COL_L = COL_R = 0;
  for (int r = 0; r < H; ++r) { COL_L |= bit(r * W); COL_R |= bit(r * W + W - 1); }
  for (int c = 0; c < N; ++c) {
    int r = c / W, k = c % W; u64 m = 0;
    if (r > 0) m |= bit(c - W);
    if (r < H - 1) m |= bit(c + W);
    if (k > 0) m |= bit(c - 1);
    if (k < W - 1) m |= bit(c + 1);
    NEI[c] = m;
  }
  NSYM = (W == H) ? 8 : 4;
  for (int t = 0; t < NSYM; ++t)
    for (int c = 0; c < N; ++c) {
      int r = c / W, k = c % W, r2 = 0, k2 = 0;
      switch (t) {
        case 0: r2 = r; k2 = k; break;
        case 1: r2 = r; k2 = W - 1 - k; break;
        case 2: r2 = H - 1 - r; k2 = k; break;
        case 3: r2 = H - 1 - r; k2 = W - 1 - k; break;
        case 4: r2 = k; k2 = r; break;                  // W==H only below
        case 5: r2 = k; k2 = H - 1 - r; break;
        case 6: r2 = W - 1 - k; k2 = r; break;
        case 7: r2 = W - 1 - k; k2 = H - 1 - r; break;
      }
      PERM[t][c] = r2 * W + k2;
    }
  memset(PERMB, 0, sizeof(PERMB));
  for (int t = 0; t < NSYM; ++t)
    for (int by = 0; by * 8 < N; ++by)
      for (int v = 0; v < 256; ++v) {
        u64 m = 0;
        for (int b = 0; b < 8; ++b)
          if ((v >> b) & 1) { int c = by * 8 + b; if (c < N) m |= bit(PERM[t][c]); }
        PERMB[t][by][v] = m;
      }
}
static inline u64 transformMask(u64 m, int t) {
  u64 r = 0;
  for (int by = 0; m; ++by, m >>= 8) r |= PERMB[t][by][m & 0xff];
  return r;
}

// ---------------- position ----------------
struct Pos {
  u64 bb[2];       // [0]=black, [1]=white stone masks
  int side;        // 0=black to move, 1=white
  bool passed;     // previous move was a pass
};
static inline u128 rawBoard(const Pos& p) { return (u128)p.bb[0] | ((u128)p.bb[1] << N); }
static inline u128 canonKey(const Pos& p) {
  u128 best = ~(u128)0;
  for (int t = 0; t < NSYM; ++t) {
    u128 k = (u128)transformMask(p.bb[0], t) | ((u128)transformMask(p.bb[1], t) << N);
    if (k < best) best = k;
  }
  return best | ((u128)p.side << (2 * N)) | ((u128)(p.passed ? 1 : 0) << (2 * N + 1));
}

static inline u64 chainAt(u64 color, int cell) {
  u64 ch = bit(cell), prev;
  do { prev = ch; ch |= expand(ch) & color; } while (ch != prev);
  return ch;
}

static const int PASS = 63;

// Apply move; returns false if illegal (occupied / forbidden suicide).
// Superko is checked by the caller against the search path.
static bool play(const Pos& p, int mv, Pos& out) {
  if (mv == PASS) { out = p; out.side = 1 - p.side; out.passed = true; return true; }
  int s = p.side;
  u64 me = p.bb[s], op = p.bb[1 - s], m = bit(mv);
  if ((me | op) & m) return false;
  me |= m;
  u64 nb = NEI[mv] & op;
  while (nb) {
    int c = __builtin_ctzll(nb);
    u64 ch = chainAt(op, c);
    nb &= ~ch;
    if (!(expand(ch) & ~(me | op))) op &= ~ch;   // capture
  }
  u64 own = chainAt(me, mv);
  if (!(expand(own) & ~(me | op))) {             // no liberties: suicide
    if (!SUICIDE_OK) return false;
    me &= ~own;
  }
  out.bb[s] = me; out.bb[1 - s] = op; out.side = 1 - s; out.passed = false;
  return true;
}

// Tromp-Taylor area score, black minus white.
static int score(const Pos& p) {
  u64 b = p.bb[0], w = p.bb[1];
  int sc = __builtin_popcountll(b) - __builtin_popcountll(w);
  u64 empty = FULL & ~(b | w);
  while (empty) {
    int c = __builtin_ctzll(empty);
    u64 reg = bit(c), prev;
    do { prev = reg; reg |= expand(reg) & empty; } while (reg != prev);
    empty &= ~reg;
    u64 adj = expand(reg);
    bool tb = (adj & b) != 0, tw = (adj & w) != 0;
    int sz = __builtin_popcountll(reg);
    if (tb && !tw) sc += sz;
    else if (tw && !tb) sc -= sz;
  }
  return sc;
}

// ---------------- Benson unconditional life + unconditional territory ----------------
static u64 benson(const Pos& p, int color) {
  u64 mine = p.bb[color], other = p.bb[1 - color];
  if (!mine) return 0;
  u64 chains[32]; int nch = 0;
  u64 rest = mine;
  while (rest) {
    int c = __builtin_ctzll(rest);
    u64 ch = chainAt(mine, c);
    rest &= ~ch;
    chains[nch++] = ch;
  }
  struct Reg { u64 emptyPts; u32 adjChains; };
  Reg regs[64]; int nreg = 0;
  u64 comp = FULL & ~mine;
  u64 empty = FULL & ~(mine | other);
  while (comp) {
    int c = __builtin_ctzll(comp);
    u64 reg = bit(c), prev;
    do { prev = reg; reg |= expand(reg) & comp; } while (reg != prev);
    comp &= ~reg;
    u32 adj = 0;
    u64 ring = expand(reg);
    for (int i = 0; i < nch; ++i) if (ring & chains[i]) adj |= 1u << i;
    regs[nreg++] = {reg & empty, adj};
  }
  bool aliveCh[32]; for (int i = 0; i < nch; ++i) aliveCh[i] = true;
  bool changed = true;
  while (changed) {
    changed = false;
    for (int i = 0; i < nch; ++i) {
      if (!aliveCh[i]) continue;
      int vital = 0;
      for (int r = 0; r < nreg; ++r) {
        bool ok = true;
        for (int j = 0; j < nch; ++j)
          if ((regs[r].adjChains >> j) & 1 && !aliveCh[j]) { ok = false; break; }
        if (!ok) continue;
        if (!((regs[r].adjChains >> i) & 1)) continue;
        if ((regs[r].emptyPts & ~expand(chains[i])) == 0) ++vital;   // vital to chain i
      }
      if (vital < 2) { aliveCh[i] = false; changed = true; }
    }
  }
  u64 alive = 0;
  for (int i = 0; i < nch; ++i) if (aliveCh[i]) alive |= chains[i];
  return alive;
}

// Guaranteed final area for `color` under any opponent play: unconditionally
// alive stones, plus every enclosed region R (component of the complement of
// color's stones) such that (a) every color chain adjacent to R is alive and
// (b) R has at most ONE interior point (an empty with no alive-chain
// neighbor). An opponent eye can only ever exist at an interior point: a
// border empty always neighbors an immortal enemy stone, and interior-ness is
// fixed because alive chains never move or die. With at most one potential
// eye point the invader can never reach two eyes, one eye is never life, so
// trapped opponent stones are dead and the whole region is color's area.
static int guaranteedArea(const Pos& p, int color) {
  u64 alive = benson(p, color);
  if (!alive) return 0;
  int g = __builtin_popcountll(alive);
  u64 aliveAdj = expand(alive);
  u64 deadOwn = p.bb[color] & ~alive;
  u64 comp = FULL & ~p.bb[color];
  while (comp) {
    int c = __builtin_ctzll(comp);
    u64 reg = bit(c), prev;
    do { prev = reg; reg |= expand(reg) & comp; } while (reg != prev);
    comp &= ~reg;
    if (expand(reg) & deadOwn) continue;          // bordered by a mortal own chain
    // potential opponent eye points: any region point (currently empty OR
    // opponent-occupied — occupancy can change, alive-adjacency cannot) that
    // has no unconditionally-alive neighbor
    u64 interior = reg & ~aliveAdj;
    if (__builtin_popcountll(interior) > 1) continue;
    g += __builtin_popcountll(reg);
  }
  return g;
}

// ---------------- lockless transposition table ----------------
// Entry: 3 words {k0^D, k1^D, D}; D packs res|depth|best|used. Torn concurrent
// writes fail validation on read and count as a miss (chess-engine style).
enum : u8 { R_UNK = 0, R_WIN = 1, R_LOSS = 2 };   // predicate "black >= T"
struct TTEnt { u64 a, b, d; };
static TTEnt* TT = nullptr;
static u64 TTMASK = 0;

static void ttAlloc(double gb) {
  u64 want = (u64)(gb * (1ull << 30)) / sizeof(TTEnt);
  u64 n = 1; while (n * 2 <= want) n *= 2;
  TT = (TTEnt*)calloc(n, sizeof(TTEnt));
  if (!TT) die("TT alloc failed (%.1f GB)", gb);
  TTMASK = n - 1;
  fprintf(stderr, "[tt] %s entries (%.1f GB)\n", commas(n).c_str(),
          n * sizeof(TTEnt) / (double)(1ull << 30));
}
static void ttClear() { memset(TT, 0, (TTMASK + 1) * sizeof(TTEnt)); }
static inline u64 mix(u64 x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x;
}
static inline u64 packD(u8 res, u8 depth, u8 best) {
  return 1ull | ((u64)res << 1) | ((u64)depth << 8) | ((u64)best << 16);
}
struct TTView { bool found; u8 res, depth, best; };
static inline TTView ttProbe(u128 key) {
  u64 k0 = (u64)key, k1 = (u64)(key >> 64);
  u64 h = mix(k0 ^ mix(k1));
  for (int i = 0; i < 4; ++i) {
    TTEnt* e = &TT[(h + i) & TTMASK];
    u64 a = e->a, b = e->b, d = e->d;
    if (d && (a ^ d) == k0 && (b ^ d) == k1)
      return {true, (u8)((d >> 1) & 3), (u8)(d >> 8), (u8)(d >> 16)};
  }
  return {false, R_UNK, 0, 0xff};
}
static inline void ttStore(u128 key, u8 res, u8 depth, u8 best) {
  u64 k0 = (u64)key, k1 = (u64)(key >> 64);
  u64 h = mix(k0 ^ mix(k1));
  TTEnt* victim = nullptr;
  int victimDepth = 256;
  for (int i = 0; i < 4; ++i) {
    TTEnt* e = &TT[(h + i) & TTMASK];
    u64 a = e->a, b = e->b, d = e->d;
    if (!d) { victim = e; victimDepth = -1; break; }              // empty slot
    if ((a ^ d) == k0 && (b ^ d) == k1) {                         // same key
      if (((d >> 1) & 3) != R_UNK && res == R_UNK) return;        // keep proven fact
      victim = e; break;
    }
    u8 eres = (d >> 1) & 3, edep = (u8)(d >> 8);
    if (eres == R_UNK && (int)edep < victimDepth) { victim = e; victimDepth = edep; }
  }
  if (!victim) return;   // all four slots proven, different keys: keep them
  u64 d = packD(res, depth, best);
  victim->a = k0 ^ d; victim->b = k1 ^ d; victim->d = d;
}

// ---------------- search ----------------
static int THRESH = 25;
static std::atomic<u64> gNodes{0}, gBensonCuts{0}, gEtcCuts{0};
static std::atomic<int> gStop{0};        // 0 = run; 1 = root proven / abort
static std::atomic<int> gRootRes{-1};
static const int INF_PLY = 1 << 20;

struct PathSet {
  std::vector<std::pair<u128, int>> v;   // (board, ply) sorted by board
  int find(u128 b) const {
    auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(b, -1));
    if (it != v.end() && it->first == b) return it->second;
    return -1;
  }
  void insert(u128 b, int ply) {
    v.insert(std::lower_bound(v.begin(), v.end(), std::make_pair(b, -1)), {b, ply});
  }
  void erase(u128 b) {
    v.erase(std::lower_bound(v.begin(), v.end(), std::make_pair(b, -1)));
  }
};

struct SRes { u8 res; int repPly; };

struct Ctx {
  PathSet path;
  int hist[2][64];
  int killers[1024][2];
  u64 nodes = 0, nodesFlushed = 0;
  u64 jitterSeed = 0;                    // 0 = no jitter (thread 0)
  int tid = 0;

  void resetHeuristics() {
    memset(hist, 0, sizeof(hist));
    memset(killers, 0xff, sizeof(killers));
  }
  inline void bumpNodes() {
    if ((++nodes & 0x3ffff) == 0) {
      gNodes.fetch_add(nodes - nodesFlushed, std::memory_order_relaxed);
      nodesFlushed = nodes;
    }
  }
  SRes search(const Pos& p, int depth, int ply);
};

SRes Ctx::search(const Pos& p, int depth, int ply) {
  bumpNodes();
  if (gStop.load(std::memory_order_relaxed)) return {R_UNK, INF_PLY};

  // terminal-by-pass fast path
  if (p.passed) {
    int s = score(p);
    if (p.side == 0 && s >= THRESH) return {R_WIN, INF_PLY};
    if (p.side == 1 && s < THRESH) return {R_LOSS, INF_PLY};
  }

  u128 key = canonKey(p);
  TTView tv = ttProbe(key);
  if (tv.found) {
    if (tv.res != R_UNK) return {tv.res, INF_PLY};
    if (tv.depth >= depth) return {R_UNK, INF_PLY};
  }
  u8 ttBest = tv.found ? tv.best : 0xff;

  // static unconditional-territory cuts (need >= 6 stones for two eyes)
  if (__builtin_popcountll(p.bb[1]) >= 6) {
    int gw = guaranteedArea(p, 1);
    if (gw > 0 && N - gw < THRESH) {
      gBensonCuts.fetch_add(1, std::memory_order_relaxed);
      ttStore(key, R_LOSS, 0, 0xff);
      return {R_LOSS, INF_PLY};
    }
  }
  if (__builtin_popcountll(p.bb[0]) >= 6) {
    int gb = guaranteedArea(p, 0);
    if (gb >= THRESH) {
      gBensonCuts.fetch_add(1, std::memory_order_relaxed);
      ttStore(key, R_WIN, 0, 0xff);
      return {R_WIN, INF_PLY};
    }
  }

  if (depth <= 0) return {R_UNK, INF_PLY};

  bool orNode = (p.side == 0);
  u8 good = orNode ? R_WIN : R_LOSS;
  u8 bad = orNode ? R_LOSS : R_WIN;

  auto finishCut = [&](int mv, SRes r) -> SRes {
    SRes ret = (r.repPly < INF_PLY && r.repPly <= ply) ? SRes{good, r.repPly}
                                                       : SRes{good, INF_PLY};
    hist[p.side][mv] += depth * depth;
    if (mv != killers[ply][0]) { killers[ply][1] = killers[ply][0]; killers[ply][0] = mv; }
    if (ret.repPly >= ply) ttStore(key, good, (u8)std::min(depth, 255), (u8)mv);
    return ret;
  };

  // stage 1: TT best-move hint before simulating all children
  if (ttBest != 0xff) {
    Pos q; bool ok = false, changes = false;
    if (ttBest == PASS) { q = p; q.side = 1 - p.side; q.passed = true; ok = true; }
    else if (ttBest < N && play(p, (int)ttBest, q)) {
      if (path.find(rawBoard(q)) < 0) { ok = true; changes = true; }
    }
    if (ok) {
      if (changes) path.insert(rawBoard(q), ply + 1);
      SRes r = search(q, depth - 1, ply + 1);
      if (changes) path.erase(rawBoard(q));
      if (r.res == good) return finishCut(ttBest, r);
    }
  }

  // stage 2: generate legal children with superko filtering
  struct Child { Pos pos; int mv; int ord; bool changes; };
  Child ch[64]; int nc = 0;
  int minRep = INF_PLY;
  u64 cand = FULL & ~(p.bb[0] | p.bb[1]);
  while (cand) {
    int c = __builtin_ctzll(cand); cand &= cand - 1;
    Pos q;
    if (!play(p, c, q)) continue;
    u128 rb = rawBoard(q);
    int rp = path.find(rb);
    if (rp >= 0) { if (rp < minRep) minRep = rp; continue; }   // superko
    int caps = __builtin_popcountll(p.bb[1 - p.side]) - __builtin_popcountll(q.bb[1 - p.side]);
    int ord = hist[p.side][c] + caps * 4096;
    if (c == ttBest) ord += 1 << 28;
    if (c == killers[ply][0] || c == killers[ply][1]) ord += 2048;
    int r = c / W, k = c % W;
    ord += 8 * ((std::min(r, H - 1 - r) + 1) * (std::min(k, W - 1 - k) + 1));
    if (jitterSeed) ord += (int)(mix((u64)c ^ jitterSeed ^ (u64)key) & 127);
    ch[nc++] = {q, c, ord, true};
  }
  { // pass child
    Pos q = p; q.side = 1 - p.side; q.passed = true;
    int ord = (PASS == ttBest) ? (1 << 28) : -(1 << 20);
    ch[nc++] = {q, PASS, ord, false};
  }

  // enhanced transposition cutoffs: probe children before any deep search
  bool resolved[64] = {false};
  if (depth >= 4) {
    for (int i = 0; i < nc; ++i) {
      TTView cv = ttProbe(canonKey(ch[i].pos));
      if (!cv.found) continue;
      if (cv.res == good) {
        gEtcCuts.fetch_add(1, std::memory_order_relaxed);
        return finishCut(ch[i].mv, {good, INF_PLY});
      }
      if (cv.res == bad) resolved[i] = true;                  // known refuted child
      else if (cv.depth >= depth - 1) { resolved[i] = true; ch[i].ord -= 1 << 24; }
      // ^ UNKNOWN at sufficient depth: this iteration would just re-return UNK
    }
  }

  std::sort(ch, ch + nc, [](const Child& a, const Child& b) { return a.ord > b.ord; });

  bool anyUnknown = false;
  int childRep = INF_PLY;
  u8 firstUnkMv = 0xff;

  for (int i = 0; i < nc; ++i) {
    SRes r;
    if (resolved[i]) {
      TTView cv = ttProbe(canonKey(ch[i].pos));   // re-read (may have improved)
      if (cv.found && cv.res != R_UNK) r = {cv.res, INF_PLY};
      else if (cv.found && cv.depth >= depth - 1) r = {R_UNK, INF_PLY};
      else { resolved[i] = false; }
      if (resolved[i]) {
        if (r.res == good) return finishCut(ch[i].mv, r);
        if (r.res == R_UNK) { anyUnknown = true; if (firstUnkMv == 0xff) firstUnkMv = (u8)ch[i].mv; }
        continue;
      }
    }
    if (ch[i].changes) path.insert(rawBoard(ch[i].pos), ply + 1);
    r = search(ch[i].pos, depth - 1, ply + 1);
    if (ch[i].changes) path.erase(rawBoard(ch[i].pos));
    if (r.res == good) return finishCut(ch[i].mv, r);
    if (r.res == R_UNK) { anyUnknown = true; if (firstUnkMv == 0xff) firstUnkMv = (u8)ch[i].mv; }
    if (r.repPly < childRep) childRep = r.repPly;
  }

  int rep = std::min(minRep, childRep);
  if (anyUnknown) {
    // do not cache aborted (stop-flag) unwinds: their UNK is not a depth-d fact
    if (rep >= ply && !gStop.load(std::memory_order_relaxed))
      ttStore(key, R_UNK, (u8)std::min(depth, 255), firstUnkMv);
    return {R_UNK, rep};
  }
  if (rep >= ply) ttStore(key, bad, (u8)std::min(depth, 255), 0xff);
  return {bad, rep};
}

// ---------------- df-pn (depth-first proof-number search) ----------------
// Proof number pn = cost to prove the predicate (black >= T) TRUE, disproof
// number dn = cost to prove it FALSE. OR nodes: black to move (pn = min over
// children, dn = sum); AND nodes: white to move (dual). Superko handled on the
// live path; proven results are stored to the TT only when path-independent
// (same GHI guard as the ID search). pn/dn of unproven nodes are heuristics,
// stored freely. Uses the shared TT with pn/dn packed into the data word.
static const u32 PN_INF = 0x7fffffffu;    // true infinity: proven/disproven only
static const u32 PN_SAT = PN_INF - 1;     // ceiling for unproven estimates/sums

// Arithmetic for the opposite-side number at a node (JHBR3-style switch):
//   pndn: classic proof/disproof numbers — sum over children (Nagai df-pn).
//   bns:  branch numbers — selected child's number + (number of unresolved
//         siblings - 1), per Okabe's route-branch-number search; immune to
//         the DAG double-counting that inflates pn/dn sums on merged graphs.
static bool ARITH_BNS = false;

struct DfpnTT {
  // pack pn,dn (31 bits each) into the d word; bit0 = used marker.
  static inline u64 packPD(u32 pn, u32 dn) {
    return 1ull | ((u64)(pn & PN_INF) << 1) | ((u64)(dn & PN_INF) << 32);
  }
  static inline bool get(u128 key, u32& pn, u32& dn) {
    u64 k0 = (u64)key, k1 = (u64)(key >> 64);
    u64 h = mix(k0 ^ mix(k1));
    for (int i = 0; i < 4; ++i) {
      TTEnt* e = &TT[(h + i) & TTMASK];
      u64 a = e->a, b = e->b, d = e->d;
      if (d && (a ^ d) == k0 && (b ^ d) == k1) {
        pn = (u32)((d >> 1) & PN_INF); dn = (u32)((d >> 32) & PN_INF);
        return true;
      }
    }
    pn = 1; dn = 1;   // unexpanded
    return false;
  }
  static inline void put(u128 key, u32 pn, u32 dn) {
    u64 k0 = (u64)key, k1 = (u64)(key >> 64);
    u64 h = mix(k0 ^ mix(k1));
    TTEnt* victim = nullptr;
    for (int i = 0; i < 4; ++i) {
      TTEnt* e = &TT[(h + i) & TTMASK];
      u64 a = e->a, b = e->b, d = e->d;
      if (!d) { victim = e; break; }
      if ((a ^ d) == k0 && (b ^ d) == k1) {
        u32 opn = (u32)((d >> 1) & PN_INF), odn = (u32)((d >> 32) & PN_INF);
        if ((opn == 0 || odn == 0) && pn != 0 && dn != 0) return;  // keep proven
        victim = e; break;
      }
      if (!victim) victim = e;   // fallback: first slot (df-pn favors recency)
    }
    u64 d = packPD(pn, dn);
    victim->a = k0 ^ d; victim->b = k1 ^ d; victim->d = d;
  }
};

// df-pn+ heuristic initialization for unexpanded nodes, tuned for the
// high-threshold predicate (black must own nearly everything): black's proof
// effort scales with surviving white stones and unclaimed space; white's
// disproof effort shrinks as white approaches a living group.
static inline void dfpnInit(const Pos& q, u32& pn, u32& dn) {
  int wS = __builtin_popcountll(q.bb[1]);
  u64 empty = FULL & ~(q.bb[0] | q.bb[1]);
  int eNB = __builtin_popcountll(empty & ~expand(q.bb[0]));
  pn = 1 + 2 * wS + (eNB + 1) / 2;
  dn = 1 + std::max(0, 7 - wS);
}

// Feature toggles for the JHBR3-derived engineering experiments.
static bool USE_MOVECACHE = true;   // memoize movegen + child keys per position
static bool USE_FRESHEN = true;     // refresh only the just-searched child's view
static bool USE_OVERRIDES = true;   // path-scoped cache of route-dependent verdicts

struct Dfpn {
  PathSet path;
  u64 nodes = 0;
  u64 nodeLimit = ~0ull;
  u64 rng = 0;         // nonzero: jitter child selection (parallel workers)
  int tid = 0;
  inline u32 jit() { if (!rng) return 0; rng = mix(rng); return (u32)(rng & 3); }

  // Move cache (JHBR3 mate/bns.cc idea): movegen output and child keys are
  // pure functions of (board, side); superko legality is filtered per visit
  // against the live path. Direct-mapped, per-thread. n==0 marks empty (real
  // entries always include the pass child, so n >= 1).
  static const int MC_MOVES = 50;             // up to N stone moves + pass
  struct MCSlot { u128 key; u16 n; u8 mv[MC_MOVES]; u128 raw[MC_MOVES]; u128 ck[MC_MOVES]; };
  static const u64 MC_MASK = (1u << 17) - 1;
  std::vector<MCSlot> mcache;
  u64 mcProbes = 0, mcHits = 0;
  Dfpn() { if (USE_MOVECACHE) mcache.resize(MC_MASK + 1); }

  // Path-override list (JHBR3): proven-but-route-dependent verdicts, valid
  // while the anchoring ancestor (the repetition target) stays on the path.
  struct Override { u128 key; u32 pn, dn; int anchor; };
  std::vector<Override> overrides;
  inline void dropOverridesAtReturn(int ply) {
    // entries are not anchor-sorted by push order: a deep node can record a
    // shallow anchor after a deeper one; scan-erase (list is normally tiny)
    if (overrides.empty()) return;
    overrides.erase(std::remove_if(overrides.begin(), overrides.end(),
        [ply](const Override& o) { return o.anchor >= ply; }), overrides.end());
  }

  struct Kid {
    Pos pos; int mv; bool changes;
    u128 key;    // canonical (TT)
    u128 xkey;   // exact (overrides; symmetry variants have distinct contexts)
    u32 pn, dn; bool localProven; int anchor;
  };
  static inline u128 exactKey(const Pos& q) {
    return rawBoard(q) | ((u128)q.side << (2 * N)) |
           ((u128)(q.passed ? 1 : 0) << (2 * N + 1));
  }

  // Returns the taint anchor: the max ply (< this node's ply) of any path
  // repetition the result depends on, or -1 if route-independent.
  // Dependencies at the node's own ply dissolve there ("the loop head owns
  // its loops": every route to this position recreates them).
  int search(const Pos& p, u32 thPn, u32 thDn, u32& pn, u32& dn, int ply) {
    ++nodes;
    if ((nodes & 0xfff) == 0 && gStop.load(std::memory_order_relaxed)) {
      pn = 1; dn = 1; return -1;   // abort unwind; estimates not stored
    }
    if ((nodes & 0x3ffffff) == 0 && tid == 0)
      fprintf(stderr, "[dfpn] nodes=%s ply=%d thPn=%u thDn=%u stones=%d+%d %.0fs\n",
              commas(nodes).c_str(), ply, thPn, thDn,
              __builtin_popcountll(p.bb[0]), __builtin_popcountll(p.bb[1]), nowS());

    // terminal / static evaluation
    if (p.passed) {
      int s = score(p);
      if (p.side == 0 && s >= THRESH) {
        pn = 0; dn = PN_INF; DfpnTT::put(canonKey(p), pn, dn); return -1;
      }
      if (p.side == 1 && s < THRESH) {
        pn = PN_INF; dn = 0; DfpnTT::put(canonKey(p), pn, dn); return -1;
      }
    }
    u128 key = canonKey(p);
    if (__builtin_popcountll(p.bb[1]) >= 6) {
      int gw = guaranteedArea(p, 1);
      if (gw > 0 && N - gw < THRESH) {
        pn = PN_INF; dn = 0; DfpnTT::put(key, pn, dn); return -1;
      }
    }
    if (__builtin_popcountll(p.bb[0]) >= 6) {
      int gb = guaranteedArea(p, 0);
      if (gb >= THRESH) { pn = 0; dn = PN_INF; DfpnTT::put(key, pn, dn); return -1; }
    }

    bool orNode = (p.side == 0);

    // ---- children: from the move cache when possible ----
    // Rule-legality, child boards and canonical keys are position-intrinsic
    // and cacheable; superko legality is path-dependent and filtered per
    // visit against the live path.
    Kid kid[64]; int nk = 0;
    int skAnchor = -1;   // max shallow (< ply) target of superko-pruned moves
    MCSlot* slot = nullptr;
    u128 gkey = rawBoard(p) | ((u128)p.side << (2 * N));
    if (USE_MOVECACHE) {
      slot = &mcache[mix((u64)gkey ^ mix((u64)(gkey >> 64))) & MC_MASK];
      ++mcProbes;
    }
    if (slot && slot->n && slot->key == gkey) {
      ++mcHits;
      for (int i = 0; i < slot->n; ++i) {
        int mv = slot->mv[i];
        u128 raw = slot->raw[i];
        if (mv != PASS) {
          int rp = path.find(raw);
          if (rp >= 0) { if (rp < ply) skAnchor = std::max(skAnchor, rp); continue; }
        }
        Pos q;
        q.bb[0] = (u64)(raw & ((((u128)1) << N) - 1));
        q.bb[1] = (u64)((raw >> N) & ((((u128)1) << N) - 1));
        q.side = 1 - p.side;
        q.passed = (mv == PASS);
        kid[nk++] = {q, mv, mv != PASS, slot->ck[i], raw | ((u128)q.side << (2 * N)) |
                     ((u128)(q.passed ? 1 : 0) << (2 * N + 1)), 1, 1, false, -1};
      }
    } else {
      int sn = 0;
      u64 cand = FULL & ~(p.bb[0] | p.bb[1]);
      while (cand) {
        int c = __builtin_ctzll(cand); cand &= cand - 1;
        Pos q;
        if (!play(p, c, q)) continue;
        u128 raw = rawBoard(q);
        u128 ck = canonKey(q);
        if (slot && sn < MC_MOVES) { slot->mv[sn] = (u8)c; slot->raw[sn] = raw; slot->ck[sn] = ck; ++sn; }
        int rp = path.find(raw);
        if (rp >= 0) { if (rp < ply) skAnchor = std::max(skAnchor, rp); continue; }
        kid[nk++] = {q, c, true, ck, exactKey(q), 1, 1, false, -1};
      }
      { Pos q = p; q.side = 1 - p.side; q.passed = true;
        u128 ck = canonKey(q);
        if (slot && sn < MC_MOVES) { slot->mv[sn] = (u8)PASS; slot->raw[sn] = rawBoard(q); slot->ck[sn] = ck; ++sn; }
        kid[nk++] = {q, PASS, false, ck, exactKey(q), 1, 1, false, -1}; }
      if (slot) { slot->n = (u16)sn; slot->key = gkey; }
    }

    u128 xkeySelf = exactKey(p);
    int passes = 0;
    int lastBi = -1;
    for (;;) {
      // safety valve: never spin forever inside one node (parallel TT races);
      // bailing with current estimates is always sound in df-pn
      bool bail = (++passes > 512);
      // refresh child pn/dn; the just-searched child already carries its
      // returned values, so with freshening only periodic full passes re-probe
      // all siblings (needed: other threads and overrides can change them)
      bool full = !USE_FRESHEN || lastBi < 0 || (passes & 15) == 0 ||
                  (USE_OVERRIDES && !overrides.empty());
      // non-full passes refresh nothing: the just-searched child keeps its
      // returned values (fresher than the TT, which may even have evicted it)
      for (int i = 0; full && i < nk; ++i) {
        if (kid[i].localProven) continue;
        if (USE_OVERRIDES && !overrides.empty()) {
          bool hit = false;
          for (auto it = overrides.rbegin(); it != overrides.rend(); ++it)
            if (it->key == kid[i].xkey) {
              kid[i].pn = it->pn; kid[i].dn = it->dn; kid[i].anchor = it->anchor;
              hit = true; break;
            }
          if (hit) continue;
        }
        kid[i].anchor = -1;
        if (!DfpnTT::get(kid[i].key, kid[i].pn, kid[i].dn))
          dfpnInit(kid[i].pos, kid[i].pn, kid[i].dn);   // df-pn+ heuristic init
      }
      // compute node pn/dn
      if (orNode) {
        pn = PN_INF; u64 dsum = 0; u32 k = 0; int bestI = -1;
        for (int i = 0; i < nk; ++i) {
          if (kid[i].pn < pn) { pn = kid[i].pn; bestI = i; }
          dsum += kid[i].dn;
          if (kid[i].pn < PN_INF) ++k;
        }
        // clamp to PN_SAT so a saturated estimate is never mistaken for a
        // true (proven) infinity
        if (pn == 0) dn = PN_INF;                       // proven by a child
        else if (k == 0) dn = 0;                        // all children disproven
        else if (ARITH_BNS)
          dn = (u32)std::min<u64>((u64)kid[bestI].dn + (k - 1), PN_SAT);
        else
          dn = (u32)std::min<u64>(dsum, PN_SAT);
      } else {
        dn = PN_INF; u64 psum = 0; u32 k = 0; int bestI = -1;
        for (int i = 0; i < nk; ++i) {
          if (kid[i].dn < dn) { dn = kid[i].dn; bestI = i; }
          psum += kid[i].pn;
          if (kid[i].dn < PN_INF) ++k;
        }
        if (dn == 0) pn = PN_INF;
        else if (k == 0) pn = 0;
        else if (ARITH_BNS)
          pn = (u32)std::min<u64>((u64)kid[bestI].pn + (k - 1), PN_SAT);
        else
          pn = (u32)std::min<u64>(psum, PN_SAT);
      }
      // superko-pruned moves could only help the mover: a mover-success proof
      // that ignores them is valid and depends only on its winning child; a
      // mover-failure proof (all moves fail) also depends on the pruned moves.
      if (pn >= thPn || dn >= thDn || bail || nodes > nodeLimit) {
        bool proven = (pn == 0 || dn == 0);
        bool moverWins = orNode ? (pn == 0) : (dn == 0);
        int anchor = -1;
        auto absorb = [&](int i) {
          if (kid[i].anchor >= 0 && kid[i].anchor < ply)
            anchor = std::max(anchor, kid[i].anchor);
        };
        if (proven && moverWins) {
          // decided by one child; prefer an untainted deciding child
          int deciding = -1;
          for (int i = 0; i < nk; ++i)
            if ((orNode ? kid[i].pn : kid[i].dn) == 0) {
              deciding = i;
              if (kid[i].anchor < 0) break;
            }
          if (deciding >= 0) absorb(deciding);
        } else {
          for (int i = 0; i < nk; ++i) absorb(i);
          if (skAnchor >= 0) anchor = std::max(anchor, skAnchor);
        }
        if (proven && anchor >= 0) {
          // route-dependent verdict: path-scoped cache instead of the TT
          if (USE_OVERRIDES) overrides.push_back({xkeySelf, pn, dn, anchor});
        } else {
          DfpnTT::put(key, pn, dn);
        }
        return anchor;
      }
      // select most-proving child
      int bi = -1;
      if (orNode) {
        u32 best = PN_INF + 1, second = PN_INF + 1;
        for (int i = 0; i < nk; ++i) {
          u32 eff = (kid[i].pn == 0 || kid[i].pn >= PN_INF) ? kid[i].pn : kid[i].pn + jit();
          if (eff < best) { second = best; best = eff; bi = i; }
          else if (eff < second) second = eff;
        }
        u32 eps = std::max<u32>(1, second >> 3);   // 1+eps threshold widening
        u32 childThPn = std::min(thPn, (second >= PN_INF) ? PN_INF : second + eps);
        childThPn = std::max(childThPn, kid[bi].pn + 1);   // guarantee child progress
        u32 childThDn = (thDn >= PN_INF) ? PN_INF
                       : (u32)std::min<u64>((u64)thDn - dn + kid[bi].dn, PN_INF);
        if (kid[bi].changes) path.insert(rawBoard(kid[bi].pos), ply + 1);
        u32 cpn, cdn;
        int ca = search(kid[bi].pos, childThPn, childThDn, cpn, cdn, ply + 1);
        if (kid[bi].changes) path.erase(rawBoard(kid[bi].pos));
        if (USE_OVERRIDES) dropOverridesAtReturn(ply + 1);
        kid[bi].pn = cpn; kid[bi].dn = cdn; kid[bi].anchor = ca;
        if (cpn == 0 || cdn == 0) kid[bi].localProven = true;
        lastBi = bi;
      } else {
        u32 best = PN_INF + 1, second = PN_INF + 1;
        for (int i = 0; i < nk; ++i) {
          u32 eff = (kid[i].dn == 0 || kid[i].dn >= PN_INF) ? kid[i].dn : kid[i].dn + jit();
          if (eff < best) { second = best; best = eff; bi = i; }
          else if (eff < second) second = eff;
        }
        u32 eps = std::max<u32>(1, second >> 3);
        u32 childThDn = std::min(thDn, (second >= PN_INF) ? PN_INF : second + eps);
        childThDn = std::max(childThDn, kid[bi].dn + 1);
        u32 childThPn = (thPn >= PN_INF) ? PN_INF
                       : (u32)std::min<u64>((u64)thPn - pn + kid[bi].pn, PN_INF);
        if (kid[bi].changes) path.insert(rawBoard(kid[bi].pos), ply + 1);
        u32 cpn, cdn;
        int ca = search(kid[bi].pos, childThPn, childThDn, cpn, cdn, ply + 1);
        if (kid[bi].changes) path.erase(rawBoard(kid[bi].pos));
        if (USE_OVERRIDES) dropOverridesAtReturn(ply + 1);
        kid[bi].pn = cpn; kid[bi].dn = cdn; kid[bi].anchor = ca;
        if (cpn == 0 || cdn == 0) kid[bi].localProven = true;
        lastBi = bi;
      }
    }
  }
};

// ---------------- parallel iterative-deepening driver ----------------
static int gThreads = 1;
static u64 gNodeCap = 0;   // 0 = unlimited; else total df-pn visit budget

static void idWorker(Pos root, int maxDepth, int tid, bool verbose) {
  Ctx* ctx = new Ctx();
  ctx->tid = tid;
  ctx->jitterSeed = tid ? mix(0x9e3779b97f4a7c15ull * (tid + 1)) : 0;
  ctx->resetHeuristics();
  ctx->path.v.clear();
  ctx->path.insert(rawBoard(root), 0);
  double t0 = nowS();
  for (int d = 2; d <= maxDepth; ++d) {
    SRes r = ctx->search(root, d, 0);
    if (gStop.load()) break;
    if (verbose && tid == 0)
      fprintf(stderr, "[depth %3d] res=%s nodes=%s benson=%s etc=%s %.1fs\n",
              d, r.res == R_WIN ? "WIN " : r.res == R_LOSS ? "LOSS" : "UNK ",
              commas(gNodes.load() + ctx->nodes - ctx->nodesFlushed).c_str(),
              commas(gBensonCuts.load()).c_str(), commas(gEtcCuts.load()).c_str(),
              nowS() - t0);
    for (int s = 0; s < 2; ++s) for (int c = 0; c < 64; ++c) ctx->hist[s][c] >>= 1;
    if (r.res != R_UNK) {
      int want = -1;
      if (gRootRes.compare_exchange_strong(want, (int)r.res)) gStop.store(1);
      break;
    }
  }
  gNodes.fetch_add(ctx->nodes - ctx->nodesFlushed);
  delete ctx;
}

static u8 solveRoot(const Pos& root, int maxDepth, bool verbose) {
  gStop.store(0); gRootRes.store(-1);
  gNodes.store(0); gBensonCuts.store(0); gEtcCuts.store(0);
  std::vector<std::thread> th;
  for (int t = 0; t < gThreads; ++t)
    th.emplace_back(idWorker, root, maxDepth, t, verbose);
  for (auto& x : th) x.join();
  int r = gRootRes.load();
  if (r < 0) die("not solved within depth %d", maxDepth);
  return (u8)r;
}

// ---------------- WDL dump ----------------
// Format (little-endian):
//   char magic[8] = "MGWDL1\0\0"
//   u32 W, H; u8 suicideAllowed; u8 superko(1=positional); i16 threshold;
//   u64 count; then count records of 16 bytes:
//     u64 canonicalKey; u8 res(1=WIN black>=T, 2=LOSS); u8 best(0..N-1,63=pass,255=n/a);
//     u8 depth; u8 pad[5]
static void dumpWdl(const char* path) {
  if (2 * N + 2 > 64) die("dump format supports keys up to 64 bits (boards <= 5x6)");
  FILE* f = fopen(path, "wb");
  if (!f) die("cannot open %s", path);
  char magic[8] = {'M','G','W','D','L','1',0,0};
  fwrite(magic, 1, 8, f);
  u32 w32 = W, h32 = H; u8 su = SUICIDE_OK, sk = 1; short th = (short)THRESH;
  fwrite(&w32, 4, 1, f); fwrite(&h32, 4, 1, f);
  fwrite(&su, 1, 1, f); fwrite(&sk, 1, 1, f); fwrite(&th, 2, 1, f);
  std::vector<std::pair<u64, u64>> recs;
  for (u64 i = 0; i <= TTMASK; ++i) {
    TTEnt& e = TT[i];
    u64 a = e.a, b = e.b, d = e.d;
    if (!d) continue;
    u8 res = (d >> 1) & 3;
    if (res == R_UNK) continue;
    u64 k0 = a ^ d, k1 = b ^ d;
    if (k1 != 0) continue;   // torn write remnant; real keys fit 64 bits here
    u64 meta = (u64)res | (((d >> 16) & 0xff) << 8) | (((d >> 8) & 0xff) << 16);
    recs.push_back({k0, meta});
  }
  std::sort(recs.begin(), recs.end());
  recs.erase(std::unique(recs.begin(), recs.end(),
                         [](auto& x, auto& y) { return x.first == y.first; }),
             recs.end());
  u64 cnt = recs.size();
  fwrite(&cnt, 8, 1, f);
  for (auto& r : recs) { fwrite(&r.first, 8, 1, f); fwrite(&r.second, 8, 1, f); }
  fclose(f);
  fprintf(stderr, "[dump] %s proven positions -> %s\n", commas(cnt).c_str(), path);
}

// Same file format, but reading df-pn pn/dn entries: proven = pn==0 or dn==0.
static void dumpWdlDfpn(const char* path) {
  if (2 * N + 2 > 64) die("dump format supports keys up to 64 bits (boards <= 5x6)");
  FILE* f = fopen(path, "wb");
  if (!f) die("cannot open %s", path);
  char magic[8] = {'M','G','W','D','L','1',0,0};
  fwrite(magic, 1, 8, f);
  u32 w32 = W, h32 = H; u8 su = SUICIDE_OK, sk = 1; short th = (short)THRESH;
  fwrite(&w32, 4, 1, f); fwrite(&h32, 4, 1, f);
  fwrite(&su, 1, 1, f); fwrite(&sk, 1, 1, f); fwrite(&th, 2, 1, f);
  std::vector<std::pair<u64, u64>> recs;
  for (u64 i = 0; i <= TTMASK; ++i) {
    TTEnt& e = TT[i];
    u64 a = e.a, b = e.b, d = e.d;
    if (!d) continue;
    u32 pn = (u32)((d >> 1) & PN_INF), dn = (u32)((d >> 31) & PN_INF);
    if (pn != 0 && dn != 0) continue;
    u64 k0 = a ^ d, k1 = b ^ d;
    if (k1 != 0) continue;
    u8 res = (pn == 0) ? R_WIN : R_LOSS;
    u64 meta = (u64)res | (0xffull << 8);
    recs.push_back({k0, meta});
  }
  std::sort(recs.begin(), recs.end());
  recs.erase(std::unique(recs.begin(), recs.end(),
                         [](auto& x, auto& y) { return x.first == y.first; }),
             recs.end());
  u64 cnt = recs.size();
  fwrite(&cnt, 8, 1, f);
  for (auto& r : recs) { fwrite(&r.first, 8, 1, f); fwrite(&r.second, 8, 1, f); }
  fclose(f);
  fprintf(stderr, "[dump] %s proven positions -> %s\n", commas(cnt).c_str(), path);
}

// ---------------- position I/O ----------------
static void printPos(const Pos& p) {
  for (int r = 0; r < H; ++r) {
    for (int c = 0; c < W; ++c) {
      int i = r * W + c;
      fputc((p.bb[0] >> i) & 1 ? 'X' : (p.bb[1] >> i) & 1 ? 'O' : '.', stderr);
    }
    fputc('\n', stderr);
  }
}
static Pos parsePos(const char* s, int side, bool passed) {
  Pos p{{0, 0}, side, passed};
  int i = 0;
  for (const char* q = s; *q && i < N; ++q) {
    if (*q == '/' || *q == ' ' || *q == '\n') continue;
    if (*q == 'X' || *q == 'x' || *q == 'B' || *q == 'b') p.bb[0] |= bit(i);
    else if (*q == 'O' || *q == 'o' || *q == 'W' || *q == 'w') p.bb[1] |= bit(i);
    else if (*q != '.') die("bad position char '%c'", *q);
    ++i;
  }
  if (i != N) die("position string has %d cells, want %d", i, N);
  return p;
}

// ---------------- self tests ----------------
static void expectEq(long long a, long long b, const char* what) {
  if (a != b) die("SELFTEST FAIL: %s: got %lld want %lld", what, a, b);
  fprintf(stderr, "  ok: %s = %lld\n", what, a);
}
static void selftest() {
  initBoard(3, 3);
  Pos p = parsePos(".X./X.X/.X.", 0, false);
  Pos q;
  expectEq(score(p), 9, "3x3 diamond scores +9");
  p = parsePos("XXX/XXX/XXX", 0, false);
  expectEq(score(p), 9, "3x3 full black scores +9");
  p = parsePos("XXX/OOO/...", 0, false);
  expectEq(score(p), -3, "3x3 black wall vs white wall + territory scores -3");
  p = parsePos("OX./.../...", 0, false);
  if (!play(p, 3, q)) die("capture move rejected");
  expectEq((long long)q.bb[1], 0, "white corner stone captured");
  expectEq((long long)(q.bb[0] >> 3 & 1), 1, "black stone placed");
  p = parsePos(".O./O.O/.O.", 0, false);
  SUICIDE_OK = false;
  expectEq(play(p, 4, q) ? 1 : 0, 0, "single-stone suicide illegal (suicide off)");
  SUICIDE_OK = true;
  expectEq(play(p, 4, q) ? 1 : 0, 1, "single-stone suicide legal when allowed");
  expectEq((long long)q.bb[0], 0, "suicide stone removed");
  initBoard(5, 5);
  p = parsePos("OOOOO/O.O.O/OOOOO/...../.....", 0, false);
  expectEq((long long)__builtin_popcountll(benson(p, 1)), 13, "benson: two-eyed white group alive");
  p = parsePos("OOOOO/O...O/OOOOO/...../.....", 0, false);
  expectEq((long long)__builtin_popcountll(benson(p, 1)), 0, "benson: big-eye group not unconditional");
  p = parsePos("OOOOO/O.O.O/OOOOO/...../.....", 0, false);
  expectEq(guaranteedArea(p, 1), 15, "guaranteed white area 15");
  // unconditional territory: trapped black stone (1 liberty) inside white's eyespace
  p = parsePos("OOOOO/O.OX./OOOOO/...../.....", 0, false);
  expectEq(guaranteedArea(p, 1), 15, "trapped black stone counts as white area");
  // whole-board unconditional black area with a dead white invader
  p = parsePos("XX.XX/XXXXX/XXXXX/XXXXX/XX.OX", 0, false);
  expectEq(guaranteedArea(p, 0), 25, "black owns whole board incl. dead invader");
  // one-interior-point region: opponent can never reach two eyes there
  p = parsePos("XXXXX/X.X.X/XXXXX/XX.XX/X...X", 0, false);
  expectEq(guaranteedArea(p, 0), 25, "region with one interior point is secure");
  fprintf(stderr, "selftest passed\n");
}

static int solveExact(int w, int h, int maxDepth, bool verbose) {
  initBoard(w, h);
  Pos root{{0, 0}, 0, false};
  int lo = -N, hi = N;
  while (lo < hi) {
    int T = lo + (hi - lo + 1) / 2;
    THRESH = T;
    ttClear();
    fprintf(stderr, "[solve %dx%d] threshold T=%d (predicate: black >= %d)\n", w, h, T, T);
    u8 r = solveRoot(root, maxDepth, verbose);
    if (r == R_WIN) lo = T; else hi = T - 1;
    fprintf(stderr, "[solve %dx%d] T=%d -> %s   value in [%d,%d]\n",
            w, h, T, r == R_WIN ? "WIN" : "LOSS", lo, hi);
  }
  printf("=== %dx%d Tromp-Taylor value (black area margin): %+d ===\n", w, h, lo);
  return lo;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage:\n"
      "  minigo selftest\n"
      "  minigo solve  --w 5 --h 5 --T 25 [--suicide 0|1] [--tt-gb G] [--threads K]\n"
      "                [--max-depth D] [--dump FILE]\n"
      "  minigo sweep  --w 4 --h 4 [--suicide 0|1] [--tt-gb G] [--threads K] [--max-depth D]\n"
      "  minigo probe  --file F --pos 'XXO../...' --side b|w [--passed 0|1]\n");
    return 1;
  }
  std::string mode = argv[1];
  int w = 5, h = 5, T = 25, maxDepth = 900;
  double ttGb = 4.0;
  const char* dumpPath = nullptr; const char* posStr = nullptr; const char* filePath = nullptr;
  int side = 0; int passed = 0;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i]; const char* v = argv[i + 1];
    if (k == "--w") w = atoi(v);
    else if (k == "--h") h = atoi(v);
    else if (k == "--T") T = atoi(v);
    else if (k == "--suicide") SUICIDE_OK = atoi(v) != 0;
    else if (k == "--tt-gb") ttGb = atof(v);
    else if (k == "--threads") gThreads = atoi(v);
    else if (k == "--max-depth") maxDepth = atoi(v);
    else if (k == "--dump") dumpPath = v;
    else if (k == "--pos") posStr = v;
    else if (k == "--file") filePath = v;
    else if (k == "--side") side = (v[0] == 'w' || v[0] == 'W' || v[0] == 'o') ? 1 : 0;
    else if (k == "--passed") passed = atoi(v);
    else if (k == "--nodes") gNodeCap = strtoull(v, nullptr, 10);
    else if (k == "--arith") ARITH_BNS = (v[0] == 'b' || v[0] == 'B');
    else if (k == "--mc") USE_MOVECACHE = atoi(v) != 0;
    else if (k == "--freshen") USE_FRESHEN = atoi(v) != 0;
    else if (k == "--ovr") USE_OVERRIDES = atoi(v) != 0;
    else die("unknown flag %s", k.c_str());
  }
  if (maxDepth > 1000) die("--max-depth capped at 1000 (killer table)");

  if (mode == "selftest") { selftest(); return 0; }

  if (mode == "solve") {
    initBoard(w, h);
    ttAlloc(ttGb);
    THRESH = T;
    Pos root{{0, 0}, 0, false};
    fprintf(stderr, "[solve] %dx%d Tromp-Taylor, suicide=%d, T=%d, threads=%d\n",
            w, h, (int)SUICIDE_OK, T, gThreads);
    double t0 = nowS();
    u8 r = solveRoot(root, maxDepth, true);
    printf("=== %dx%d T=%d: %s (black %s achieve area score >= %d) "
           "nodes=%s in %.1fs ===\n",
           w, h, T, r == R_WIN ? "WIN" : "LOSS", r == R_WIN ? "CAN" : "CANNOT", T,
           commas(gNodes.load()).c_str(), nowS() - t0);
    if (dumpPath) dumpWdl(dumpPath);
    return 0;
  }

  if (mode == "eval") {
    initBoard(w, h);
    if (!posStr) die("eval needs --pos");
    Pos p = parsePos(posStr, side, passed != 0);
    printPos(p);
    printf("side=%s passed=%d\n", p.side ? "white" : "black", (int)p.passed);
    printf("stones: black=%d white=%d  static score (both pass now) = %+d\n",
           __builtin_popcountll(p.bb[0]), __builtin_popcountll(p.bb[1]), score(p));
    for (int c = 0; c < 2; ++c) {
      u64 alive = benson(p, c);
      printf("%s: benson-alive stones=%d guaranteedArea=%d\n",
             c ? "white" : "black", __builtin_popcountll(alive), guaranteedArea(p, c));
    }
    return 0;
  }

  if (mode == "dfpn") {
    initBoard(w, h);
    ttAlloc(ttGb);
    THRESH = T;
    Pos root = posStr ? parsePos(posStr, side, passed != 0) : Pos{{0, 0}, 0, false};
    fprintf(stderr, "[dfpn] %dx%d Tromp-Taylor, suicide=%d, T=%d, threads=%d, arith=%s"
            " mc=%d freshen=%d ovr=%d\n",
            w, h, (int)SUICIDE_OK, T, gThreads, ARITH_BNS ? "bns" : "pndn",
            (int)USE_MOVECACHE, (int)USE_FRESHEN, (int)USE_OVERRIDES);
    double t0 = nowS();
    gStop.store(0); gRootRes.store(-1);
    std::atomic<u64> totNodes{0};
    std::vector<std::thread> th;
    for (int t = 0; t < gThreads; ++t)
      th.emplace_back([&, t]() {
        Dfpn* d = new Dfpn();
        d->tid = t;
        d->rng = t ? mix(0x12345678u * (t + 1)) : 0;
        if (gNodeCap) d->nodeLimit = std::max<u64>(1, gNodeCap / gThreads);
        d->path.insert(rawBoard(root), 0);
        u32 pn = 1, dn = 1;
        int rounds = 0;
        while (pn != 0 && dn != 0 && !gStop.load() && d->nodes <= d->nodeLimit) {
          d->search(root, PN_INF, PN_INF, pn, dn, 0);
          if (t == 0 && !gNodeCap)
            fprintf(stderr, "[root] pn=%u dn=%u nodes(t0)=%s %.0fs\n",
                    pn, dn, commas(d->nodes).c_str(), nowS() - t0);
          if (++rounds > (1 << 20)) break;
        }
        if (pn == 0 || dn == 0) {
          int want = -1;
          if (gRootRes.compare_exchange_strong(want, pn == 0 ? 1 : 2)) gStop.store(1);
        }
        if (t == 0 && USE_MOVECACHE && d->mcProbes)
          fprintf(stderr, "[mc] t0 probes=%s hits=%s (%.1f%%)\n",
                  commas(d->mcProbes).c_str(), commas(d->mcHits).c_str(),
                  100.0 * d->mcHits / d->mcProbes);
        totNodes.fetch_add(d->nodes);
        delete d;
      });
    for (auto& x : th) x.join();
    int rr = gRootRes.load();
    if (rr < 0) {
      if (!gNodeCap) die("dfpn: no proof (threshold overflow?)");
      printf("=== %dx%d T=%d dfpn: UNSOLVED nodes=%s in %.1fs ===\n",
             w, h, T, commas(totNodes.load()).c_str(), nowS() - t0);
      return 3;
    }
    printf("=== %dx%d T=%d dfpn: %s (black %s achieve area score >= %d) "
           "nodes=%s in %.1fs ===\n",
           w, h, T, rr == 1 ? "WIN" : "LOSS", rr == 1 ? "CAN" : "CANNOT", T,
           commas(totNodes.load()).c_str(), nowS() - t0);
    if (dumpPath) dumpWdlDfpn(dumpPath);
    return 0;
  }

  if (mode == "sweep") {
    initBoard(w, h);
    ttAlloc(ttGb);
    solveExact(w, h, maxDepth, true);
    return 0;
  }

  if (mode == "probe") {
    if (!posStr || !filePath) die("probe needs --file and --pos");
    FILE* f = fopen(filePath, "rb");
    if (!f) die("cannot open %s", filePath);
    char magic[8]; u32 fw, fh; u8 su, sk; short th; u64 cnt;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MGWDL1", 6)) die("bad magic");
    if (fread(&fw, 4, 1, f) != 1 || fread(&fh, 4, 1, f) != 1 ||
        fread(&su, 1, 1, f) != 1 || fread(&sk, 1, 1, f) != 1 ||
        fread(&th, 2, 1, f) != 1 || fread(&cnt, 8, 1, f) != 1) die("bad header");
    initBoard(fw, fh); SUICIDE_OK = su;
    Pos p = parsePos(posStr, side, passed != 0);
    u64 k64 = (u64)canonKey(p);
    long base = ftell(f);
    u64 lo = 0, hi = cnt;
    long long foundMeta = -1;
    while (lo < hi) {
      u64 mid = (lo + hi) / 2;
      u64 kk, mm;
      fseek(f, base + (long)(mid * 16), SEEK_SET);
      if (fread(&kk, 8, 1, f) != 1 || fread(&mm, 8, 1, f) != 1) die("read error");
      if (kk == k64) { foundMeta = (long long)mm; break; }
      if (kk < k64) lo = mid + 1; else hi = mid;
    }
    fclose(f);
    printPos(p);
    fprintf(stderr, "side=%s passed=%d threshold=%d\n", side ? "white" : "black", passed, (int)th);
    if (foundMeta < 0) { printf("NOT IN TABLE\n"); return 2; }
    u8 res = foundMeta & 0xff, best = (foundMeta >> 8) & 0xff;
    printf("%s (black %s reach >= %d)", res == R_WIN ? "WIN" : "LOSS",
           res == R_WIN ? "can" : "cannot", (int)th);
    if (best != 0xff) {
      if (best == PASS) printf("  best=pass");
      else printf("  best=%c%d", 'a' + best % fw, best / fw + 1);
    }
    printf("\n");
    return 0;
  }

  die("unknown mode %s", mode.c_str());
}
