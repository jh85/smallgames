// quoridor_wdl.cpp
//
// Strongly solves small Quoridor boards (up to 8x8 squares, 64 cells) and produces a
// full WDL (win/draw/loss) table over ALL states:
//   (side to move, pawn1 square, pawn2 square, walls left per player, wall configuration)
//
// Rules follow the conventions of the reference Rust solver in quoridor-solving/:
//   - Player1 starts at (row 0, col W/2) and races south to row H-1; Player2 mirrored;
//     Player1 moves first.
//   - Wall anchors are the (H-1)x(W-1) interior corners; each anchor holds one horizontal
//     or one vertical wall of length 2. Walls conflict when they cross at the same anchor
//     or overlap along their orientation.
//   - A wall placement is legal iff after placement both pawns' CURRENT squares can still
//     reach their goal rows.
//   - Standard jump rules: straight jump over an adjacent opponent if the edge behind is
//     clear, otherwise diagonal sidesteps around the opponent where clear.
//   - Loopy states (forced repetition) and stalemates are draws, matching the reference
//     solver's retrograde treatment.
//
// Solving method: retrograde analysis layered by k = number of walls on the board.
// Wall placements strictly increase k, so layer k only depends on layer k+1 (wall moves)
// and on itself (pawn moves). Within a layer, for a fixed wall configuration and fixed
// walls-in-hand split, the subgame is a tiny 2*S*S-state graph solved by BFS retrograde
// propagation with counters; unresolved states are draws.
//
// ZDD use: the family of non-overlapping wall configurations is built as a
// zero-suppressed decision diagram via frontier-based construction. The ZDD gives exact
// per-cardinality configuration counts (state-space size estimation, cf. the product
// upper bound in the MCTS paper) and cross-validates the explicit enumeration that the
// solver uses for dense table indexing.
//
// Build:  g++ -O3 -march=native -fopenmp -std=c++17 -o qwdl quoridor_wdl.cpp
//
// Usage:
//   ./qwdl count W H MAXWALLSTOTAL      ZDD counts + state-size table
//   ./qwdl solve W H WALLSPERPLAYER [--threads N] [--save DIR] [--quiet-layers]
//   ./qwdl selftest W H WALLSPERPLAYER  full cross-check vs naive reference engine
//   ./qwdl probe DIR                    interactive probe of a saved table (stdin lines)

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#include <parallel/algorithm>
#endif

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using u128 = unsigned __int128;

static std::string u128_str(u128 v) {
    if (v == 0) return "0";
    std::string s;
    while (v > 0) { s.push_back(char('0' + int(v % 10))); v /= 10; }
    std::reverse(s.begin(), s.end());
    return s;
}

// ---------------------------------------------------------------------------
// Board geometry
// ---------------------------------------------------------------------------

enum Dir { NORTH = 0, SOUTH = 1, WEST = 2, EAST = 3 };
static inline Dir opposite(Dir d) {
    switch (d) {
        case NORTH: return SOUTH;
        case SOUTH: return NORTH;
        case WEST: return EAST;
        default: return WEST;
    }
}

struct Slot {
    int row, col, dir;  // dir: 0 = horizontal, 1 = vertical (matches Rust ordering)
};

struct Board {
    int W, H, S;        // width, height, squares
    int E;              // wall slots = 2*(W-1)*(H-1)
    std::vector<Slot> slots;
    std::vector<u128> conflicts;          // per slot: mask of conflicting slots (excl. self)
    std::vector<std::array<int, 4>> nbr;  // per square: [N,S,W,E], -1 off board
    int start[2];                         // start squares of P1, P2
    int goalRow[2];                       // P1 goal row H-1, P2 goal row 0
    u64 goalMask[2];                      // squares on each player's goal row

    Board(int w, int h) : W(w), H(h), S(w * h) {
        assert(W >= 2 && H >= 2 && S <= 64);
        E = 2 * (W - 1) * (H - 1);
        assert(E <= 128);
        for (int r = 0; r < H - 1; r++)
            for (int c = 0; c < W - 1; c++)
                for (int d = 0; d < 2; d++) slots.push_back({r, c, d});
        conflicts.assign(E, 0);
        for (int a = 0; a < E; a++)
            for (int b = 0; b < E; b++) {
                if (a == b) continue;
                const Slot &x = slots[a], &y = slots[b];
                bool conf;
                if (x.dir != y.dir) {
                    conf = x.row == y.row && x.col == y.col;
                } else if (x.dir == 0) {
                    conf = x.row == y.row && std::abs(x.col - y.col) <= 1;
                } else {
                    conf = x.col == y.col && std::abs(x.row - y.row) <= 1;
                }
                if (conf) conflicts[a] |= (u128(1) << b);
            }
        nbr.resize(S);
        for (int sq = 0; sq < S; sq++) {
            int r = sq / W, c = sq % W;
            nbr[sq][NORTH] = r > 0 ? sq - W : -1;
            nbr[sq][SOUTH] = r + 1 < H ? sq + W : -1;
            nbr[sq][WEST] = c > 0 ? sq - 1 : -1;
            nbr[sq][EAST] = c + 1 < W ? sq + 1 : -1;
        }
        start[0] = W / 2;                  // row 0
        start[1] = (H - 1) * W + W / 2;    // row H-1
        goalRow[0] = H - 1;
        goalRow[1] = 0;
        goalMask[0] = goalMask[1] = 0;
        for (int c = 0; c < W; c++) {
            goalMask[0] |= u64(1) << ((H - 1) * W + c);
            goalMask[1] |= u64(1) << c;
        }
    }

    // Clear-direction bits per square for a wall mask (bit d set = step in dir d open).
    void clearDirs(u128 mask, u8 *clear) const {
        for (int sq = 0; sq < S; sq++) {
            u8 bits = 0;
            for (int d = 0; d < 4; d++)
                if (nbr[sq][d] >= 0) bits |= u8(1) << d;
            clear[sq] = bits;
        }
        while (mask) {
            int e = u64(mask) ? __builtin_ctzll(u64(mask)) : 64 + __builtin_ctzll(u64(mask >> 64));
            const Slot &s = slots[e];
            int sq = s.row * W + s.col;
            if (s.dir == 0) {  // horizontal: blocks S movement from sq and sq+1
                for (int q : {sq, sq + 1}) {
                    clear[q] &= u8(~(1 << SOUTH));
                    clear[q + W] &= u8(~(1 << NORTH));
                }
            } else {  // vertical: blocks E movement from sq and sq+W
                for (int q : {sq, sq + W}) {
                    clear[q] &= u8(~(1 << EAST));
                    clear[q + 1] &= u8(~(1 << WEST));
                }
            }
            mask &= mask - 1;
        }
    }

    // Squares that can reach `row` under clear-direction table (undirected flood fill).
    u64 reachRow(const u8 *clear, int row) const {
        u64 seen = 0;
        int stack[64], top = 0;
        for (int c = 0; c < W; c++) {
            int sq = row * W + c;
            seen |= u64(1) << sq;
            stack[top++] = sq;
        }
        while (top) {
            int sq = stack[--top];
            for (int d = 0; d < 4; d++) {
                if (!(clear[sq] & (1 << d))) continue;
                int nq = nbr[sq][d];
                if (nq < 0 || (seen >> nq) & 1) continue;
                seen |= u64(1) << nq;
                stack[top++] = nq;
            }
        }
        return seen;
    }

    // Pawn destination mask for mover at `from` with opponent at `opp` (jump rules).
    u64 pawnMoves(const u8 *clear, int from, int opp) const {
        static const Dir sides[4][2] = {{WEST, EAST}, {WEST, EAST}, {NORTH, SOUTH}, {NORTH, SOUTH}};
        u64 moves = 0;
        for (int d = 0; d < 4; d++) {
            if (!(clear[from] & (1 << d))) continue;
            int next = nbr[from][d];
            if (next != opp) {
                moves |= u64(1) << next;
                continue;
            }
            if (clear[opp] & (1 << d)) {
                moves |= u64(1) << nbr[opp][d];
                continue;
            }
            for (Dir sd : sides[d])
                if (clear[opp] & (1 << sd)) moves |= u64(1) << nbr[opp][sd];
        }
        return moves;
    }
};

// ---------------------------------------------------------------------------
// ZDD over wall slots: family of all non-conflicting wall sets with <= K walls.
// Frontier-based construction; used for exact per-cardinality counts and to
// cross-validate the explicit enumeration.
// ---------------------------------------------------------------------------

struct ZddResult {
    std::vector<u128> countByCard;  // exact number of configurations with k walls
    size_t unreducedNodes = 0;
    size_t reducedNodes = 0;
};

// Frontier state while sweeping anchors row-major, two decisions (H then V) per anchor:
//   vb   : per column, V wall present at the anchor "above" this column's frontier line
//   hp   : H wall present at the previous anchor in the current row
//   hc   : H wall present at the current anchor (between its H and V decisions)
static ZddResult zddWallFamily(const Board &b, int K) {
    const int WB = b.W - 1;  // anchors per row
    const int NKEY = 1 << (WB + 2);
    auto key = [&](u32 vb, int hp, int hc) { return vb | (u32(hp) << WB) | (u32(hc) << (WB + 1)); };

    // Forward DP counts per (frontier key, cards); also count decision nodes.
    std::vector<std::vector<u128>> cur(NKEY), nxt(NKEY);
    cur[key(0, 0, 0)].assign(K + 1, 0);
    cur[key(0, 0, 0)][0] = 1;

    ZddResult res;
    size_t liveNodes = 0;

    // For reduced-ZDD size, build explicit node layers keyed by (frontier,cards) and
    // reduce bottom-up afterwards. Node id 0 = FALSE, 1 = TRUE.
    struct RawNode { int var; long lo, hi; };
    std::vector<RawNode> raw;
    raw.push_back({-1, 0, 0});  // FALSE
    raw.push_back({-1, 0, 0});  // TRUE
    std::vector<std::map<std::pair<u32, int>, long>> layerIds(b.E + 1);

    // Assign ids level by level going forward; edges filled as we transition.
    layerIds[0][{key(0, 0, 0), 0}] = raw.size();
    raw.push_back({0, -1, -1});

    for (int e = 0; e < b.E; e++) {
        const Slot &s = b.slots[e];
        for (auto &v : nxt) v.clear();
        auto &ids = layerIds[e];
        auto &nids = layerIds[e + 1];
        liveNodes += ids.size();

        auto getNext = [&](u32 k2, int cards) -> long {
            auto it = nids.find({k2, cards});
            if (it != nids.end()) return it->second;
            long id = raw.size();
            raw.push_back({e + 1, -1, -1});
            nids[{k2, cards}] = id;
            return id;
        };

        for (auto &entry : ids) {
            u32 fk = entry.first.first;
            int cards = entry.first.second;
            long id = entry.second;
            u32 vb = fk & ((1u << WB) - 1);
            int hp = (fk >> WB) & 1, hc = (fk >> (WB + 1)) & 1;

            u32 loKey, hiKey;
            bool canTake;
            if (s.dir == 0) {  // H decision at anchor (row,col)
                loKey = key(vb, hp, 0);
                hiKey = key(vb, hp, 1);
                canTake = hp == 0 && cards < K;
            } else {  // V decision; afterwards advance to next anchor
                bool lastInRow = s.col == WB - 1;
                u32 vbLo = vb & ~(1u << s.col);
                u32 vbHi = vb | (1u << s.col);
                int hpNext = lastInRow ? 0 : hc;
                loKey = key(vbLo, hpNext, 0);
                hiKey = key(vbHi, hpNext, 0);
                canTake = hc == 0 && !((vb >> s.col) & 1) && cards < K;
            }

            long lo, hi;
            if (e + 1 == b.E) {
                lo = 1;
                hi = canTake ? 1 : 0;
            } else {
                lo = getNext(loKey, cards);
                hi = canTake ? getNext(hiKey, cards + 1) : 0;
            }
            raw[id].lo = lo;
            raw[id].hi = hi;
            (void)loKey; (void)hiKey;
        }

        // DP count propagation (separate from node graph, cheaper representation).
        for (int k2 = 0; k2 < NKEY; k2++) {
            if (cur[k2].empty()) continue;
            u32 vb = u32(k2) & ((1u << WB) - 1);
            int hp = (k2 >> WB) & 1, hc = (k2 >> (WB + 1)) & 1;
            u32 loKey, hiKey;
            bool canTakeGeom;
            if (s.dir == 0) {
                loKey = key(vb, hp, 0);
                hiKey = key(vb, hp, 1);
                canTakeGeom = hp == 0;
            } else {
                bool lastInRow = s.col == WB - 1;
                int hpNext = lastInRow ? 0 : hc;
                loKey = key(vb & ~(1u << s.col), hpNext, 0);
                hiKey = key(vb | (1u << s.col), hpNext, 0);
                canTakeGeom = hc == 0 && !((vb >> s.col) & 1);
            }
            if (nxt[loKey].empty()) nxt[loKey].assign(K + 1, 0);
            for (int c = 0; c <= K; c++) nxt[loKey][c] += cur[k2][c];
            if (canTakeGeom) {
                if (nxt[hiKey].empty()) nxt[hiKey].assign(K + 1, 0);
                for (int c = 0; c < K; c++) nxt[hiKey][c + 1] += cur[k2][c];
            }
        }
        std::swap(cur, nxt);
    }

    res.countByCard.assign(K + 1, 0);
    for (int k2 = 0; k2 < NKEY; k2++)
        if (!cur[k2].empty())
            for (int c = 0; c <= K; c++) res.countByCard[c] += cur[k2][c];

    res.unreducedNodes = liveNodes;

    // Reduce: bottom-up merge of identical (var,lo,hi); zero-suppression: hi==FALSE -> lo.
    {
        std::vector<long> remap(raw.size(), -1);
        remap[0] = 0;
        remap[1] = 1;
        std::map<std::tuple<int, long, long>, long> uniq;
        long reduced = 2;
        for (int e = b.E - 1; e >= 0; e--) {
            for (auto &entry : layerIds[e]) {
                long id = entry.second;
                long lo = remap[raw[id].lo], hi = remap[raw[id].hi];
                if (hi == 0) {
                    remap[id] = lo;  // zero-suppress
                    continue;
                }
                auto sig = std::make_tuple(raw[id].var, lo, hi);
                auto it = uniq.find(sig);
                if (it != uniq.end()) {
                    remap[id] = it->second;
                } else {
                    remap[id] = reduced++;
                    uniq[sig] = remap[id];
                }
            }
        }
        res.reducedNodes = size_t(reduced - 2);
    }
    return res;
}

// ---------------------------------------------------------------------------
// Explicit enumeration of wall configurations grouped by exact wall count.
// ---------------------------------------------------------------------------

static void enumRec(const Board &b, int e, u128 mask, int used, int K,
                    std::vector<std::vector<u128>> &out) {
    if (e == b.E) {
        out[used].push_back(mask);
        return;
    }
    // Prune: even taking every remaining slot cannot matter for correctness, only speed.
    enumRec(b, e + 1, mask, used, K, out);
    if (used < K && !((mask >> e) & 1) && (mask & b.conflicts[e]) == 0)
        enumRec(b, e + 1, mask | (u128(1) << e), used + 1, K, out);
}

// Parallel enumeration: split on decision prefixes over the first PRE slots.
static std::vector<std::vector<u128>> enumerateConfigs(const Board &b, int K, int threads) {
    std::vector<std::vector<u128>> result(K + 1);
    int PRE = std::min(b.E, 12);
    // Enumerate prefix masks over slots [0, PRE)
    std::vector<std::pair<u128, int>> prefixes;
    {
        std::vector<std::vector<u128>> tmp(K + 1);
        Board bp = b;  // reuse recursion with truncated E
        bp.E = PRE;
        for (int used = 0; used <= K; used++) tmp[used].clear();
        enumRec(bp, 0, 0, 0, K, tmp);
        for (int used = 0; used <= K; used++)
            for (u128 m : tmp[used]) prefixes.push_back({m, used});
    }
    int T = std::max(1, threads);
    std::vector<std::vector<std::vector<u128>>> perThread(T, std::vector<std::vector<u128>>(K + 1));
#pragma omp parallel for schedule(dynamic, 16) num_threads(T)
    for (size_t i = 0; i < prefixes.size(); i++) {
#ifdef _OPENMP
        int t = omp_get_thread_num();
#else
        int t = 0;
#endif
        enumRec(b, PRE, prefixes[i].first, prefixes[i].second, K, perThread[t]);
    }
    for (int k = 0; k <= K; k++) {
        size_t total = 0;
        for (int t = 0; t < T; t++) total += perThread[t][k].size();
        result[k].reserve(total);
        for (int t = 0; t < T; t++) {
            result[k].insert(result[k].end(), perThread[t][k].begin(), perThread[t][k].end());
            perThread[t][k].clear();
            perThread[t][k].shrink_to_fit();
        }
#ifdef _OPENMP
        __gnu_parallel::sort(result[k].begin(), result[k].end());
#else
        std::sort(result[k].begin(), result[k].end());
#endif
    }
    return result;
}

// ---------------------------------------------------------------------------
// Layered WDL solve
// ---------------------------------------------------------------------------

// Value encoding: 0 = draw/unknown, 1 = Player1 wins, 2 = Player2 wins.
struct Layer {
    int k = 0;
    int splitLo = 0, splitCount = 0;  // used1 in [splitLo, splitLo+splitCount)
    std::vector<u128> masks;          // sorted
    std::vector<u64> reach[2];        // per config: squares reaching P1 goal / P2 goal
    std::vector<u8> values;           // packed 2-bit, blocks of blockBytes per (cfg,split)
};

struct Solver {
    Board b;
    int w;      // walls per player
    int kmax;   // 2*w
    size_t blockStates, blockBytes;
    std::vector<Layer> layers;
    int threads;

    Solver(int W, int H, int wallsPerPlayer, int nthreads)
        : b(W, H), w(wallsPerPlayer), kmax(2 * wallsPerPlayer), threads(nthreads) {
        blockStates = size_t(b.S) * b.S * 2;
        blockBytes = ((blockStates * 2 + 63) / 64) * 8;
    }

    static inline u8 getv(const u8 *base, size_t idx) {
        return (base[idx >> 2] >> ((idx & 3) * 2)) & 3;
    }

    struct Scratch {
        std::vector<u8> clear;      // S
        std::vector<u64> M, R;      // S*S dest / reverse masks
        std::vector<u8> pcnt;       // S*S popcounts of M
        std::vector<u8> value, pending;
        std::vector<u16> total;
        std::vector<u16> queue;
        std::vector<u8> packed;
        std::vector<std::pair<u32, u32>> trans;  // (slot, next-layer config index)
        u64 cnt[4] = {0, 0, 0, 0};               // value stats (valid states)
        void init(const Solver &s) {
            int S = s.b.S;
            clear.assign(S, 0);
            M.assign(size_t(S) * S, 0);
            R.assign(size_t(S) * S, 0);
            pcnt.assign(size_t(S) * S, 0);
            value.assign(s.blockStates, 0);
            pending.assign(s.blockStates, 0);
            total.assign(s.blockStates, 0);
            queue.assign(s.blockStates, 0);
            packed.assign(s.blockBytes, 0);
        }
    };

    int splitLoOf(int k) const { return std::max(0, k - w); }
    int splitCountOf(int k) const { return std::min(k, w) - splitLoOf(k) + 1; }

    // Solve one wall configuration (all splits) in layer `lay`, next = solved layer k+1.
    void solveConfig(Layer &lay, const Layer *next, size_t ci, Scratch &sc) {
        const int S = b.S;
        u128 mask = lay.masks[ci];
        b.clearDirs(mask, sc.clear.data());

        // Movement tables shared by both players (goal rows don't affect movement).
        for (int p = 0; p < S; p++)
            for (int o = 0; o < S; o++) {
                if (p == o) { sc.M[p * S + o] = 0; sc.pcnt[p * S + o] = 0; continue; }
                u64 m = b.pawnMoves(sc.clear.data(), p, o);
                sc.M[p * S + o] = m;
                sc.pcnt[p * S + o] = u8(__builtin_popcountll(m));
            }
        // Reverse: R[q*S+o] = set of p with q in M[p][o]
        std::fill(sc.R.begin(), sc.R.end(), 0);
        for (int p = 0; p < S; p++)
            for (int o = 0; o < S; o++) {
                if (p == o) continue;
                u64 m = sc.M[p * S + o];
                while (m) {
                    int q = __builtin_ctzll(m);
                    m &= m - 1;
                    sc.R[size_t(q) * S + o] |= u64(1) << p;
                }
            }

        // Wall-move transitions into layer k+1 (config indices), if any walls can be placed.
        sc.trans.clear();
        if (lay.k < kmax && next) {
            for (int e = 0; e < b.E; e++) {
                if ((mask >> e) & 1) continue;
                if (mask & b.conflicts[e]) continue;
                u128 nm = mask | (u128(1) << e);
                auto it = std::lower_bound(next->masks.begin(), next->masks.end(), nm);
                assert(it != next->masks.end() && *it == nm);
                sc.trans.push_back({u32(e), u32(it - next->masks.begin())});
            }
        }

        const u64 g1 = b.goalMask[0], g2 = b.goalMask[1];
        const u64 all = (S == 64) ? ~u64(0) : ((u64(1) << S) - 1);

        for (int sp = 0; sp < lay.splitCount; sp++) {
            int used1 = lay.splitLo + sp;
            int l1 = w - used1, l2 = w - (lay.k - used1);
            u8 *value = sc.value.data();
            u8 *pending = sc.pending.data();
            u16 *total = sc.total.data();
            std::fill(sc.value.begin(), sc.value.end(), 0);
            std::fill(sc.pending.begin(), sc.pending.end(), 0);
            std::fill(sc.total.begin(), sc.total.end(), 0);

            // Terminal values and pawn-child counts.
            for (int p1 = 0; p1 < S; p1++)
                for (int p2 = 0; p2 < S; p2++) {
                    if (p1 == p2) continue;
                    size_t s0 = (size_t(p1) * S + p2) * 2;
                    if ((g1 >> p1) & 1) {
                        value[s0] = value[s0 + 1] = 1;  // P1 on goal row: game over
                        continue;
                    }
                    if ((g2 >> p2) & 1) {
                        value[s0] = value[s0 + 1] = 2;
                        continue;
                    }
                    u8 c0 = sc.pcnt[p1 * S + p2];  // P1 to move
                    u8 c1 = sc.pcnt[p2 * S + p1];  // P2 to move
                    total[s0] = c0;
                    pending[s0] = c0;
                    total[s0 + 1] = c1;
                    pending[s0 + 1] = c1;
                }

            // Exit edges: wall placements into layer k+1 (values already known).
            if (!sc.trans.empty() && (l1 > 0 || l2 > 0)) {
                int nlo = next->splitLo;
                for (auto &tr : sc.trans) {
                    u32 nc = tr.second;
                    u64 r1 = next->reach[0][nc] & ~g1;  // non-terminal p1 squares kept legal
                    u64 r2 = next->reach[1][nc] & ~g2;
                    r1 &= all;
                    r2 &= all;
                    const u8 *blk[2] = {nullptr, nullptr};
                    if (l1 > 0)
                        blk[0] = next->values.data() +
                                 (size_t(nc) * next->splitCount + (used1 + 1 - nlo)) * blockBytes;
                    if (l2 > 0)
                        blk[1] = next->values.data() +
                                 (size_t(nc) * next->splitCount + (used1 - nlo)) * blockBytes;
                    u64 m1 = r1;
                    while (m1) {
                        int p1 = __builtin_ctzll(m1);
                        m1 &= m1 - 1;
                        u64 m2 = r2 & ~(u64(1) << p1);
                        while (m2) {
                            int p2 = __builtin_ctzll(m2);
                            m2 &= m2 - 1;
                            size_t s0 = (size_t(p1) * S + p2) * 2;
                            size_t child = s0;  // child block same pawn squares
                            for (int t = 0; t < 2; t++) {
                                if (!blk[t]) continue;
                                u8 vc = getv(blk[t], child + (1 - t));
                                size_t s = s0 + t;
                                total[s]++;
                                if (vc == u8(t + 1)) {
                                    value[s] = u8(t + 1);  // winning wall move
                                } else if (vc != u8(2 - t)) {
                                    pending[s]++;  // draw exit: never resolves to loss
                                }
                                // losing exit: counted in total only
                            }
                        }
                    }
                }
            }

            // Seed queue: terminals + states already decided by exits.
            u16 *qs = sc.queue.data();
            size_t qh = 0, qt = 0;
            for (int p1 = 0; p1 < S; p1++)
                for (int p2 = 0; p2 < S; p2++) {
                    if (p1 == p2) continue;
                    size_t s0 = (size_t(p1) * S + p2) * 2;
                    bool term = ((g1 >> p1) & 1) || ((g2 >> p2) & 1);
                    for (int t = 0; t < 2; t++) {
                        size_t s = s0 + t;
                        if (term) {
                            if (t == 0) qs[qt++] = u16(s0), qs[qt++] = u16(s0 + 1);
                            break;
                        }
                        if (value[s] != 0) {
                            qs[qt++] = u16(s);
                        } else if (total[s] > 0 && pending[s] == 0) {
                            value[s] = u8(2 - t);  // every move loses
                            qs[qt++] = u16(s);
                        }
                        // total==0: stalemate -> draw (leave unresolved), matches reference
                    }
                }

            // BFS retrograde propagation over in-block pawn edges.
            while (qh < qt) {
                size_t s = qs[qh++];
                u8 v = value[s];
                int tc = int(s & 1);
                int q1 = int((s >> 1) / S), q2 = int((s >> 1) % S);
                int tp = 1 - tc;
                // parents: mover pawn is player tp; opponent pawn fixed
                u64 pm = (tp == 0) ? sc.R[size_t(q1) * S + q2] : sc.R[size_t(q2) * S + q1];
                while (pm) {
                    int p = __builtin_ctzll(pm);
                    pm &= pm - 1;
                    size_t ps = (tp == 0) ? ((size_t(p) * S + q2) * 2)
                                          : ((size_t(q1) * S + p) * 2 + 1);
                    if (value[ps] != 0) continue;
                    // parent is non-terminal by construction (terminals have value set)
                    if (v == u8(tp + 1)) {
                        value[ps] = u8(tp + 1);
                        qs[qt++] = u16(ps);
                    } else {
                        if (--pending[ps] == 0) {
                            value[ps] = u8(2 - tp);
                            qs[qt++] = u16(ps);
                        }
                    }
                }
            }

            // Pack and store; accumulate stats over valid states.
            std::fill(sc.packed.begin(), sc.packed.end(), 0);
            for (size_t s = 0; s < blockStates; s++) {
                int p1 = int((s >> 1) / S), p2 = int((s >> 1) % S);
                if (p1 == p2) continue;
                sc.cnt[value[s]]++;
                sc.packed[s >> 2] |= u8(value[s] << ((s & 3) * 2));
            }
            std::memcpy(lay.values.data() + (ci * lay.splitCount + sp) * blockBytes,
                        sc.packed.data(), blockBytes);
        }
    }

    // Prepare masks + reach arrays for every layer (values allocated lazily per layer).
    void prepareLayers(std::vector<std::vector<u128>> &&configs) {
        layers.resize(kmax + 1);
        for (int k = 0; k <= kmax; k++) {
            Layer &L = layers[k];
            L.k = k;
            L.splitLo = splitLoOf(k);
            L.splitCount = splitCountOf(k);
            L.masks = std::move(configs[k]);
            size_t N = L.masks.size();
            L.reach[0].assign(N, 0);
            L.reach[1].assign(N, 0);
#pragma omp parallel num_threads(threads)
            {
                std::vector<u8> clear(b.S);
#pragma omp for schedule(dynamic, 1024)
                for (size_t i = 0; i < N; i++) {
                    b.clearDirs(L.masks[i], clear.data());
                    L.reach[0][i] = b.reachRow(clear.data(), b.goalRow[0]);
                    L.reach[1][i] = b.reachRow(clear.data(), b.goalRow[1]);
                }
            }
        }
    }

    // Solve all layers top-down; returns per-layer stats. keepAll retains every layer's
    // values in memory (for selftest/probe); otherwise layer k+1 is freed after k.
    struct LayerStats { u64 cnt[4]; size_t states; };
    std::vector<LayerStats> solveAll(bool keepAll, const std::string &saveDir, bool quiet) {
        std::vector<LayerStats> stats(kmax + 1);
        std::vector<Scratch> scratch(threads);
        for (auto &s : scratch) s.init(*this);

        for (int k = kmax; k >= 0; k--) {
            Layer &L = layers[k];
            const Layer *next = (k < kmax) ? &layers[k + 1] : nullptr;
            size_t N = L.masks.size();
            L.values.assign(N * L.splitCount * blockBytes, 0);
            for (auto &s : scratch) s.cnt[0] = s.cnt[1] = s.cnt[2] = s.cnt[3] = 0;

            double t0 = now();
#pragma omp parallel for schedule(dynamic, 64) num_threads(threads)
            for (size_t ci = 0; ci < N; ci++) {
#ifdef _OPENMP
                int t = omp_get_thread_num();
#else
                int t = 0;
#endif
                solveConfig(L, next, ci, scratch[t]);
            }
            LayerStats &st = stats[k];
            st.cnt[0] = st.cnt[1] = st.cnt[2] = st.cnt[3] = 0;
            for (auto &s : scratch)
                for (int v = 0; v < 4; v++) st.cnt[v] += s.cnt[v];
            st.states = st.cnt[0] + st.cnt[1] + st.cnt[2];
            if (!quiet)
                fprintf(stderr,
                        "layer k=%d configs=%zu splits=%d states=%zu W1=%llu W2=%llu D=%llu "
                        "(%.1fs)\n",
                        k, N, L.splitCount, st.states, (unsigned long long)st.cnt[1],
                        (unsigned long long)st.cnt[2], (unsigned long long)st.cnt[0],
                        now() - t0);

            if (!saveDir.empty()) saveLayer(saveDir, k);
            if (!keepAll && next) {
                layers[k + 1].values.clear();
                layers[k + 1].values.shrink_to_fit();
            }
        }
        return stats;
    }

    static double now() {
#ifdef _OPENMP
        return omp_get_wtime();
#else
        return double(clock()) / CLOCKS_PER_SEC;
#endif
    }

    void saveLayer(const std::string &dir, int k) const {
        const Layer &L = layers[k];
        {
            std::ofstream f(dir + "/layer_" + std::to_string(k) + ".masks", std::ios::binary);
            f.write(reinterpret_cast<const char *>(L.masks.data()), L.masks.size() * sizeof(u128));
        }
        {
            std::ofstream f(dir + "/layer_" + std::to_string(k) + ".wdl", std::ios::binary);
            f.write(reinterpret_cast<const char *>(L.values.data()), L.values.size());
        }
    }

    // Look up a state's value from in-memory layers (requires keepAll solve).
    // Returns 0/1/2, or -1 if the state is outside the table.
    int lookup(int turn, int p1, int p2, int l1, int l2, u128 mask) const {
        int k = 0;
        for (u128 m = mask; m; m &= m - 1) k++;
        if (k > kmax) return -1;
        int used1 = w - l1, used2 = w - l2;
        if (used1 + used2 != k) return -1;
        const Layer &L = layers[k];
        auto it = std::lower_bound(L.masks.begin(), L.masks.end(), mask);
        if (it == L.masks.end() || *it != mask) return -1;
        size_t ci = it - L.masks.begin();
        int sp = used1 - L.splitLo;
        if (sp < 0 || sp >= L.splitCount) return -1;
        size_t idx = (size_t(p1) * b.S + p2) * 2 + turn;
        return getv(L.values.data() + (ci * L.splitCount + sp) * blockBytes, idx);
    }
};

// ---------------------------------------------------------------------------
// Naive reference engine (independent implementation) + reachable-graph solver.
// Used by selftest to validate the fast solver's full table on tiny boards.
// ---------------------------------------------------------------------------

namespace naive {

struct State {
    int turn, p[2], walls[2];
    std::vector<int> placed;  // sorted slot indices
    bool operator<(const State &o) const {
        if (turn != o.turn) return turn < o.turn;
        if (p[0] != o.p[0]) return p[0] < o.p[0];
        if (p[1] != o.p[1]) return p[1] < o.p[1];
        if (walls[0] != o.walls[0]) return walls[0] < o.walls[0];
        if (walls[1] != o.walls[1]) return walls[1] < o.walls[1];
        return placed < o.placed;
    }
};

// Independent edge-blocking test: can a pawn step from a to b given placed walls?
static bool stepOpen(const Board &b, const std::vector<int> &placed, int from, int to) {
    int W = b.W;
    int fr = from / W, fc = from % W, tr = to / W, tc = to % W;
    for (int e : placed) {
        const Slot &s = b.slots[e];
        if (s.dir == 0) {  // horizontal wall between rows s.row and s.row+1, cols s.col..s.col+1
            if (fc == tc && std::min(fr, tr) == s.row && std::max(fr, tr) == s.row + 1 &&
                (fc == s.col || fc == s.col + 1))
                return false;
        } else {  // vertical wall between cols s.col and s.col+1, rows s.row..s.row+1
            if (fr == tr && std::min(fc, tc) == s.col && std::max(fc, tc) == s.col + 1 &&
                (fr == s.row || fr == s.row + 1))
                return false;
        }
    }
    return true;
}

static bool reachesRow(const Board &b, const std::vector<int> &placed, int from, int row) {
    std::vector<char> seen(b.S, 0);
    std::queue<int> q;
    q.push(from);
    seen[from] = 1;
    while (!q.empty()) {
        int sq = q.front();
        q.pop();
        if (sq / b.W == row) return true;
        for (int d = 0; d < 4; d++) {
            int nq = b.nbr[sq][d];
            if (nq < 0 || seen[nq]) continue;
            if (!stepOpen(b, placed, sq, nq)) continue;
            seen[nq] = 1;
            q.push(nq);
        }
    }
    return false;
}

static int winner(const Board &b, const State &s) {
    if (s.p[0] / b.W == b.H - 1) return 1;
    if (s.p[1] / b.W == 0) return 2;
    return 0;
}

static std::vector<State> successors(const Board &b, const State &s) {
    std::vector<State> out;
    int me = s.turn, op = 1 - me;
    int from = s.p[me], opp = s.p[op];
    // Pawn moves (independent jump-rule implementation).
    static const Dir sides[4][2] = {{WEST, EAST}, {WEST, EAST}, {NORTH, SOUTH}, {NORTH, SOUTH}};
    std::vector<int> dests;
    for (int d = 0; d < 4; d++) {
        int next = b.nbr[from][d];
        if (next < 0 || !stepOpen(b, s.placed, from, next)) continue;
        if (next != opp) {
            dests.push_back(next);
            continue;
        }
        int behind = b.nbr[opp][d];
        if (behind >= 0 && stepOpen(b, s.placed, opp, behind)) {
            dests.push_back(behind);
            continue;
        }
        for (Dir sd : sides[d]) {
            int side = b.nbr[opp][sd];
            if (side >= 0 && stepOpen(b, s.placed, opp, side)) dests.push_back(side);
        }
    }
    std::sort(dests.begin(), dests.end());
    dests.erase(std::unique(dests.begin(), dests.end()), dests.end());
    for (int dst : dests) {
        State n = s;
        n.turn = op;
        n.p[me] = dst;
        out.push_back(n);
    }
    // Wall moves.
    if (s.walls[me] > 0) {
        for (int e = 0; e < b.E; e++) {
            bool conflict = false;
            for (int pe : s.placed)
                if (pe == e || ((b.conflicts[e] >> pe) & 1)) { conflict = true; break; }
            if (conflict) continue;
            std::vector<int> np = s.placed;
            np.insert(std::lower_bound(np.begin(), np.end(), e), e);
            if (!reachesRow(b, np, s.p[0], b.H - 1)) continue;
            if (!reachesRow(b, np, s.p[1], 0)) continue;
            State n = s;
            n.turn = op;
            n.placed = np;
            n.walls[me]--;
            out.push_back(n);
        }
    }
    return out;
}

// Reachable-graph BFS + retrograde attractor (mirrors the Rust fallback solver).
static std::map<State, int> solveReachable(const Board &b, int wallsPerPlayer) {
    State start;
    start.turn = 0;
    start.p[0] = b.start[0];
    start.p[1] = b.start[1];
    start.walls[0] = start.walls[1] = wallsPerPlayer;
    std::vector<State> states{start};
    std::map<State, int> index{{start, 0}};
    std::vector<std::vector<int>> children;
    for (size_t i = 0; i < states.size(); i++) {
        State s = states[i];
        children.push_back({});
        if (winner(b, s)) continue;
        for (State &c : successors(b, s)) {
            auto it = index.find(c);
            int ci;
            if (it == index.end()) {
                ci = int(states.size());
                index[c] = ci;
                states.push_back(c);
            } else {
                ci = it->second;
            }
            children[i].push_back(ci);
        }
    }
    size_t n = states.size();
    std::vector<std::vector<int>> parents(n);
    for (size_t i = 0; i < n; i++)
        for (int c : children[i]) parents[c].push_back(int(i));
    std::vector<int> outcome(n, 0), remaining(n);
    std::queue<int> q;
    for (size_t i = 0; i < n; i++) {
        remaining[i] = int(children[i].size());
        int win = winner(b, states[i]);
        if (win) {
            outcome[i] = win;
            q.push(int(i));
        }
    }
    while (!q.empty()) {
        int c = q.front();
        q.pop();
        int cw = outcome[c];
        for (int p : parents[c]) {
            if (outcome[p]) continue;
            int mover = states[p].turn + 1;
            if (mover == cw) {
                outcome[p] = mover;
                q.push(p);
            } else if (--remaining[p] == 0) {
                outcome[p] = 3 - mover;
                q.push(p);
            }
        }
    }
    std::map<State, int> result;
    for (size_t i = 0; i < n; i++) result[states[i]] = outcome[i];
    return result;
}

}  // namespace naive

// ---------------------------------------------------------------------------
// CLI modes
// ---------------------------------------------------------------------------

static int defaultThreads() {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

static void printCountTable(int W, int H, int K) {
    Board b(W, H);
    ZddResult z = zddWallFamily(b, K);
    printf("board %dx%d: squares=%d wall_slots=%d\n", W, H, b.S, b.E);
    printf("ZDD nodes: unreduced=%zu reduced=%zu (family of all <=%d-wall configs)\n",
           z.unreducedNodes, z.reducedNodes, K);
    printf("\n%-4s %20s %22s %22s %26s\n", "k", "exact N_k (ZDD)", "C(slots,k)",
           "paper-style est.", "layer states (all splits)");
    // paper-style estimate: prod_{j<k}(F-4j)/k!  (each wall kills ~4 slots)
    long double paperTotal = 0, exactTotal = 0;
    u128 pawnPairs = u128(b.S) * (b.S - 1) * 2;  // ordered pawn pairs * side-to-move
    for (int k = 0; k <= K; k++) {
        long double binom = 1, paper = 1;
        for (int j = 0; j < k; j++) {
            binom = binom * (b.E - j) / (j + 1);
            paper = paper * std::max(0, b.E - 4 * j) / (j + 1);
        }
        // splits when solving with w = K/2 walls per player
        int w = K / 2;
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        u128 layerStates = z.countByCard[k] * u128(splits) * pawnPairs;
        printf("%-4d %20s %22.0Lf %22.0Lf %26s\n", k, u128_str(z.countByCard[k]).c_str(), binom,
               paper, u128_str(layerStates).c_str());
        paperTotal += paper;
        exactTotal += (long double)(u64)(z.countByCard[k] > u128(~u64(0)) ? 0 : u64(z.countByCard[k]));
    }
    u128 cfgTotal = 0, stTotal = 0;
    int w = K / 2;
    for (int k = 0; k <= K; k++) {
        cfgTotal += z.countByCard[k];
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        stTotal += z.countByCard[k] * u128(splits) * pawnPairs;
    }
    printf("\ntotal wall configurations (<=%d walls): %s\n", K, u128_str(cfgTotal).c_str());
    printf("total WDL table states (w=%d per player): %s\n", w, u128_str(stTotal).c_str());
    size_t S = size_t(b.S);
    u128 bytes = stTotal / 4;
    printf("approx table size at 2 bits/state: %s bytes (%.2f GiB)\n", u128_str(bytes).c_str(),
           double((u64)bytes) / (1024.0 * 1024 * 1024));
    (void)S;
    (void)paperTotal;
    (void)exactTotal;
}

static int runSolve(int W, int H, int w, int threads, const std::string &saveDir, bool keepAll,
                    bool quiet, Solver **out = nullptr) {
    Solver *sv = new Solver(W, H, w, threads);
    double t0 = Solver::now();

    // ZDD counts (cheap) cross-validate enumeration.
    ZddResult z = zddWallFamily(sv->b, sv->kmax);
    auto configs = enumerateConfigs(sv->b, sv->kmax, threads);
    for (int k = 0; k <= sv->kmax; k++) {
        if (u128(configs[k].size()) != z.countByCard[k]) {
            fprintf(stderr, "FATAL: enumeration/ZDD mismatch at k=%d: enum=%zu zdd=%s\n", k,
                    configs[k].size(), u128_str(z.countByCard[k]).c_str());
            return 2;
        }
    }
    if (!quiet)
        fprintf(stderr, "enumeration validated against ZDD; %d layers, %.1fs\n", sv->kmax + 1,
                Solver::now() - t0);

    if (!saveDir.empty()) {
        std::string cmd = "mkdir -p '" + saveDir + "'";
        if (system(cmd.c_str()) != 0) {
            fprintf(stderr, "cannot create save dir\n");
            return 2;
        }
    }

    sv->prepareLayers(std::move(configs));
    auto stats = sv->solveAll(keepAll, saveDir, quiet);

    // Start-position value.
    int v = sv->lookup(0, sv->b.start[0], sv->b.start[1], w, w, 0);
    const char *names[] = {"Draw", "Player1", "Player2"};
    u64 tw1 = 0, tw2 = 0, td = 0, ts = 0;
    for (auto &st : stats) {
        tw1 += st.cnt[1];
        tw2 += st.cnt[2];
        td += st.cnt[0];
        ts += st.states;
    }
    printf("width=%d height=%d walls=%d winner=%s method=retrograde_full states=%llu "
           "W1=%llu W2=%llu D=%llu time=%.1fs\n",
           W, H, w, names[v], (unsigned long long)ts, (unsigned long long)tw1,
           (unsigned long long)tw2, (unsigned long long)td, Solver::now() - t0);

    if (!saveDir.empty()) {
        std::ofstream f(saveDir + "/meta.txt");
        f << "W " << W << "\nH " << H << "\nw " << w << "\nkmax " << sv->kmax << "\nblockBytes "
          << sv->blockBytes << "\n";
        for (int k = 0; k <= sv->kmax; k++)
            f << "layer " << k << " configs " << sv->layers[k].masks.size() << " splitLo "
              << sv->layers[k].splitLo << " splitCount " << sv->layers[k].splitCount << "\n";
        f << "winner " << names[v] << "\n";
    }

    if (out)
        *out = sv;
    else
        delete sv;
    return 0;
}

static int runSelftest(int W, int H, int w, int threads) {
    printf("selftest %dx%d walls=%d\n", W, H, w);
    Solver *sv = nullptr;
    int rc = runSolve(W, H, w, threads, "", /*keepAll=*/true, /*quiet=*/true, &sv);
    if (rc != 0) return rc;

    Board b(W, H);
    auto ref = naive::solveReachable(b, w);
    size_t checked = 0, mismatches = 0;
    for (auto &kv : ref) {
        const naive::State &s = kv.first;
        u128 mask = 0;
        for (int e : s.placed) mask |= u128(1) << e;
        int fast = sv->lookup(s.turn, s.p[0], s.p[1], s.walls[0], s.walls[1], mask);
        int refv = kv.second;
        // Reference marks terminal states with the winner; table stores same convention.
        if (fast != refv) {
            if (mismatches < 10)
                printf("MISMATCH turn=%d p1=%d p2=%d l1=%d l2=%d k=%zu ref=%d fast=%d\n", s.turn,
                       s.p[0], s.p[1], s.walls[0], s.walls[1], s.placed.size(), refv, fast);
            mismatches++;
        }
        checked++;
    }
    printf("selftest %dx%d walls=%d: reachable states checked=%zu mismatches=%zu -> %s\n", W, H, w,
           checked, mismatches, mismatches ? "FAIL" : "OK");
    delete sv;
    return mismatches ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Probe mode: query a saved table from disk without loading it into memory.
// Query line: "t p1r p1c p2r p2c l1 l2 [Hr,c|Vr,c ...]"   (t: 1 or 2 to move)
// Prints the state's WDL value and the value of every legal move.
// ---------------------------------------------------------------------------

struct ProbeCtx {
    std::string dir;
    int W = 0, H = 0, w = 0, kmax = 0;
    size_t blockBytes = 0;
    std::vector<size_t> nCfg;
    std::vector<int> splitLo, splitCount;
    std::vector<std::vector<u128>> masks;  // lazy loaded
    std::vector<FILE *> wdl;

    bool load(const std::string &d) {
        dir = d;
        std::ifstream f(dir + "/meta.txt");
        if (!f) return false;
        std::string key;
        while (f >> key) {
            if (key == "W") f >> W;
            else if (key == "H") f >> H;
            else if (key == "w") f >> w;
            else if (key == "kmax") f >> kmax;
            else if (key == "blockBytes") f >> blockBytes;
            else if (key == "layer") {
                int k; std::string s; size_t n; int lo, cnt;
                f >> k >> s >> n >> s >> lo >> s >> cnt;
                nCfg.resize(kmax + 1); splitLo.resize(kmax + 1); splitCount.resize(kmax + 1);
                nCfg[k] = n; splitLo[k] = lo; splitCount[k] = cnt;
            } else {
                std::string rest; std::getline(f, rest);
            }
        }
        masks.resize(kmax + 1);
        wdl.assign(kmax + 1, nullptr);
        return W > 0;
    }

    const std::vector<u128> &layerMasks(int k) {
        if (masks[k].empty() && nCfg[k] > 0) {
            std::ifstream f(dir + "/layer_" + std::to_string(k) + ".masks", std::ios::binary);
            masks[k].resize(nCfg[k]);
            f.read(reinterpret_cast<char *>(masks[k].data()), nCfg[k] * sizeof(u128));
        }
        return masks[k];
    }

    int value(const Board &b, int turn, int p1, int p2, int l1, int l2, u128 mask) {
        int k = 0;
        for (u128 m = mask; m; m &= m - 1) k++;
        if (k > kmax) return -1;
        int used1 = w - l1, used2 = w - l2;
        if (used1 < 0 || used2 < 0 || used1 + used2 != k) return -1;
        const auto &ms = layerMasks(k);
        auto it = std::lower_bound(ms.begin(), ms.end(), mask);
        if (it == ms.end() || *it != mask) return -1;
        size_t ci = it - ms.begin();
        int sp = used1 - splitLo[k];
        if (sp < 0 || sp >= splitCount[k]) return -1;
        if (!wdl[k]) {
            wdl[k] = fopen((dir + "/layer_" + std::to_string(k) + ".wdl").c_str(), "rb");
            if (!wdl[k]) return -1;
        }
        size_t idx = (size_t(p1) * b.S + p2) * 2 + turn;
        size_t off = (ci * splitCount[k] + sp) * blockBytes + (idx >> 2);
        u8 byte;
        if (fseeko(wdl[k], off, SEEK_SET) != 0 || fread(&byte, 1, 1, wdl[k]) != 1) return -1;
        return (byte >> ((idx & 3) * 2)) & 3;
    }
};

static int runProbe(const std::string &dir) {
    ProbeCtx ctx;
    if (!ctx.load(dir)) {
        fprintf(stderr, "cannot read %s/meta.txt\n", dir.c_str());
        return 2;
    }
    Board b(ctx.W, ctx.H);
    const char *names[] = {"Draw", "Player1Win", "Player2Win"};
    printf("table %dx%d walls=%d  (query: t p1r p1c p2r p2c l1 l2 [Hr,c Vr,c ...])\n", ctx.W,
           ctx.H, ctx.w);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        int t, p1r, p1c, p2r, p2c, l1, l2;
        if (!(in >> t >> p1r >> p1c >> p2r >> p2c >> l1 >> l2)) continue;
        t -= 1;
        u128 mask = 0;
        std::string tok;
        bool ok = true;
        while (in >> tok) {
            int r, c;
            if (tok.size() >= 4 && (tok[0] == 'H' || tok[0] == 'V') &&
                sscanf(tok.c_str() + 1, "%d,%d", &r, &c) == 2) {
                int e = (r * (b.W - 1) + c) * 2 + (tok[0] == 'V');
                mask |= u128(1) << e;
            } else ok = false;
        }
        if (!ok || t < 0 || t > 1) { printf("bad query\n"); continue; }
        int p1 = p1r * b.W + p1c, p2 = p2r * b.W + p2c;
        int v = ctx.value(b, t, p1, p2, l1, l2, mask);
        if (v < 0) { printf("state outside table\n"); continue; }
        printf("value=%s\n", names[v]);
        // Child moves with values.
        std::vector<u8> clear(b.S);
        b.clearDirs(mask, clear.data());
        int from = t == 0 ? p1 : p2, opp = t == 0 ? p2 : p1;
        u64 mv = b.pawnMoves(clear.data(), from, opp);
        while (mv) {
            int q = __builtin_ctzll(mv);
            mv &= mv - 1;
            int c1 = t == 0 ? q : p1, c2 = t == 0 ? p2 : q;
            int cv = ctx.value(b, 1 - t, c1, c2, l1, l2, mask);
            printf("  move %d,%d -> %s\n", q / b.W, q % b.W, cv < 0 ? "?" : names[cv]);
        }
        int lm = t == 0 ? l1 : l2;
        if (lm > 0) {
            for (int e = 0; e < b.E; e++) {
                if ((mask >> e) & 1 || (mask & b.conflicts[e])) continue;
                u128 nm = mask | (u128(1) << e);
                std::vector<u8> nc(b.S);
                b.clearDirs(nm, nc.data());
                if (!((b.reachRow(nc.data(), b.goalRow[0]) >> p1) & 1)) continue;
                if (!((b.reachRow(nc.data(), b.goalRow[1]) >> p2) & 1)) continue;
                int nl1 = l1 - (t == 0), nl2 = l2 - (t == 1);
                int cv = ctx.value(b, 1 - t, p1, p2, nl1, nl2, nm);
                const Slot &s = b.slots[e];
                printf("  wall %c%d,%d -> %s\n", s.dir ? 'V' : 'H', s.row, s.col,
                       cv < 0 ? "?" : names[cv]);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n  %s count W H MAXWALLSTOTAL\n  %s solve W H WALLS [--threads N] [--save "
                "DIR] [--keep] [--quiet-layers]\n  %s selftest W H WALLS\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "probe" && argc >= 3) return runProbe(argv[2]);
    if (mode == "count" && argc >= 5) {
        printCountTable(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
    if ((mode == "solve" || mode == "selftest") && argc >= 5) {
        int W = atoi(argv[2]), H = atoi(argv[3]), w = atoi(argv[4]);
        int threads = defaultThreads();
        std::string saveDir;
        bool keep = false, quiet = false;
        for (int i = 5; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
            else if (a == "--save" && i + 1 < argc) saveDir = argv[++i];
            else if (a == "--keep") keep = true;
            else if (a == "--quiet-layers") quiet = true;
        }
        if (mode == "selftest") return runSelftest(W, H, w, threads);
        return runSolve(W, H, w, threads, saveDir, keep, quiet);
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
