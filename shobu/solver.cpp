// Mini-Shobu strong solver (retrograde analysis -> WDL table).
//
// Mini-Shobu: 2-board reduced variant of Shobu, faithful to the rules of the
// Java engine in ../Shobu (validated by replaying all 104,396 dataset games):
//   - 2 boards, each W x H. Board 0 = White's home, board 1 = Black's home.
//   - Each board starts with K white stones on row 0 and K black stones on
//     the last row. Black moves first.
//   - A turn = passive move (own stone, own home board, pushes nothing)
//     + aggressive move (own stone, the other board, SAME heading; may push
//     at most 1 enemy stone, which moves by the same heading vector; a pushed
//     stone leaving the board is captured).
//   - Headings: queen directions, distance 1 or 2 (16 headings).
//   - Win: a board containing zero stones of a color ends the game; the other
//     color wins. (Equivalently: a state where the player to move has zero
//     stones on some board is a lost terminal.)
//   - A player with no legal turn loses (stalemate = loss; counted, see log).
//
// Method: exhaustive retrograde analysis over the full product state space,
// or its exact player/geometric-symmetry quotient in the *-sym modes. States
// are indexed densely by combinatorial ranking; no product-sized hash table.
//
// Usage:
//   solver solve[-sym]  W H K outprefix -> writes .wdl and .summary.txt
//   solver solve-layered-sym W H K prefix -> exact WDL-only material layers
//   solver verify[-sym] W H K file.wdl  -> re-checks table consistency
//   solver query[-sym]  W H K file.wdl "boardstring" turn
//        boardstring: 2*W*H chars, 'o'=white 'x'=black '.'=empty,
//        board 0 then board 1, row-major, row 0 = white side.
//        turn: w or b. Prints value, depth, and every legal move's value.
//
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <unordered_map>
using namespace std;
using State = unsigned __int128;
static constexpr State STATE_INVALID = ~State(0);

static int W, H, K, SQ;                 // board dim, stones per color, N*N
static int64_t Cb[20][20];           // binomials
static int MAXB;                     // configs per board
static int64_t NSTATES;
static bool USE_SYM = false;

// In symmetry modes, first normalize the player to move (swapping boards and
// colors when Black is on turn), then quotient by the simultaneous geometric
// symmetries of both boards: D2 for rectangles, D4 for squares.  A SymRow is a
// canonical first-board rank.  Its second board is quotiented by the
// stabilizer of the first board, which gives a dense perfect rank of pair
// orbits without a hash table.
struct SymSubgroup {
    int mask = 0;
    vector<int> reps;
    vector<int> rank;                // empty for the identity subgroup
    vector<vector<int>> materialReps;
    vector<int> materialRank;
};
struct SymRow {
    int board0 = 0;
    int subgroup = 0;
    int64_t offset = 0;
};
static int NSYM;
static vector<vector<int>> boardSym;
static vector<SymSubgroup> symSubgroups;
static vector<SymRow> symRows;
static vector<int> symRowIndex;
static vector<uint8_t> boardMaterial;

static void initSymmetry();
static int64_t rankStateSym(State s);
static State unrankStateSym(int64_t id);

// value encoding in table
enum : uint8_t { V_UNREACHED = 0, V_DRAW = 1, V_WIN = 2, V_LOSS = 3 };

struct Head { int dx, dy, ux, uy, dist; };
static Head HEADS[16];

// State layout: four SQ-bit masks followed by the turn bit.  This needs 65
// bits on 4x4, hence State is wider than the dense table's 64-bit index.
// masks: index b (0/1), color c (0=white,1=black): bits [ (b*2+c)*SQ , +SQ )
static inline uint64_t getMask(State s, int board, int color) {
    return (uint64_t)(s >> ((board * 2 + color) * SQ)) & ((1ull << SQ) - 1);
}
static inline State setMask(State s, int board, int color, uint64_t m) {
    int sh = (board * 2 + color) * SQ;
    State field = State((1ull << SQ) - 1) << sh;
    return (s & ~field) | (State(m) << sh);
}
static inline int getTurn(State s) { return (int)(s >> (4 * SQ)) & 1; }
static inline State withTurn(State s, int t) {
    return (s & ~(State(1) << (4 * SQ))) | (State(t) << (4 * SQ));
}

// ---- combinatorial ranking of one board config ----
// offset[w][b]: number of configs with (w',b') lexicographically smaller.
static vector<int64_t> off; // (K+2)x(K+2)  (allow w,b up to K+1 for reverse gen rejects)

static void initTables() {
    for (int a = 0; a < 20; a++) {
        Cb[a][0] = 1;
        for (int b = 1; b <= a; b++) Cb[a][b] = Cb[a-1][b-1] + Cb[a-1][b];
    }
    int hd = 0;
    for (int dx = -2; dx <= 2; dx++)
        for (int dy = -2; dy <= 2; dy++) {
            if (dx == 0 && dy == 0) continue;
            if (abs(dx) == 2 && abs(dy) == 1) continue;
            if (abs(dx) == 1 && abs(dy) == 2) continue;
            Head h;
            h.dx = dx; h.dy = dy;
            h.ux = (dx > 0) - (dx < 0); h.uy = (dy > 0) - (dy < 0);
            h.dist = max(abs(dx), abs(dy));
            HEADS[hd++] = h;
        }
    // offsets for w,b <= K
    off.assign((K + 1) * (K + 1), 0);
    int64_t acc = 0;
    for (int w = 0; w <= K; w++)
        for (int b = 0; b <= K; b++) {
            if (w + b > SQ) { off[w * (K + 1) + b] = -1; continue; }
            off[w * (K + 1) + b] = acc;
            acc += Cb[SQ][w] * Cb[SQ - w][b];
        }
    MAXB = (int)acc;
    if (USE_SYM) initSymmetry();
    else NSTATES = (int64_t)MAXB * MAXB * 2;
}

// rank a board config; returns -1 if counts exceed caps
static inline int rankBoard(uint64_t wm, uint64_t bm) {
    int w = __builtin_popcountll(wm), b = __builtin_popcountll(bm);
    if (w > K || b > K || w + b > SQ) return -1;
    // colex rank of wm
    int rw = 0, i = 1;
    uint64_t t = wm;
    while (t) {
        int p = __builtin_ctzll(t); t &= t - 1;
        rw += (int)Cb[p][i]; i++;
    }
    // rank of bm within complement of wm
    int rb = 0, j = 1, empty = 0;
    for (int p = 0; p < SQ; p++) {
        if ((wm >> p) & 1) continue;
        if ((bm >> p) & 1) { rb += (int)Cb[empty][j]; j++; }
        empty++;
    }
    return (int)(off[w * (K + 1) + b] + (int64_t)rw * Cb[SQ - w][b] + rb);
}

static inline int64_t rankState(State s) {
    if (USE_SYM) return rankStateSym(s);
    int r0 = rankBoard(getMask(s, 0, 0), getMask(s, 0, 1));
    int r1 = rankBoard(getMask(s, 1, 0), getMask(s, 1, 1));
    if (r0 < 0 || r1 < 0) return -1;
    return ((int64_t)r0 * MAXB + r1) * 2 + getTurn(s);
}

// unrank subset: choose k positions from n, colex rank r
static uint64_t unrankSubset(int n, int k, int r) {
    uint64_t m = 0;
    for (int p = n - 1; p >= 0 && k > 0; p--) {
        if (r >= Cb[p][k]) { r -= (int)Cb[p][k]; m |= 1ull << p; k--; }
    }
    return m;
}

static void unrankBoard(int r, uint64_t &wm, uint64_t &bm) {
    // find (w,b) bucket containing r
    int ww = 0, bb = 0;
    for (ww = 0; ww <= K; ww++) {
        bool found = false;
        for (bb = 0; bb <= K; bb++) {
            if (ww + bb > SQ) continue;
            int64_t o = off[ww * (K + 1) + bb];
            int64_t sz = Cb[SQ][ww] * Cb[SQ - ww][bb];
            if (r >= o && r < o + sz) { found = true; break; }
        }
        if (found) break;
    }
    r -= (int)off[ww * (K + 1) + bb];
    int cw = (int)Cb[SQ - ww][bb];
    int rw = r / cw, rb = r % cw;
    wm = unrankSubset(SQ, ww, rw);
    // place b stones on complement of wm, colex index rb
    // complement positions e_0<...<e_{SQ-ww-1}; find subset of indices
    uint64_t idxm = unrankSubset(SQ - ww, bb, rb);
    bm = 0;
    int ei = 0;
    for (int p = 0; p < SQ; p++) {
        if ((wm >> p) & 1) continue;
        if ((idxm >> ei) & 1) bm |= 1ull << p;
        ei++;
    }
}

static inline State unrankState(int64_t id) {
    if (USE_SYM) return unrankStateSym(id);
    int turn = (int)(id & 1);
    int64_t v = id >> 1;
    int r1 = (int)(v % MAXB), r0 = (int)(v / MAXB);
    uint64_t wm0, bm0, wm1, bm1;
    unrankBoard(r0, wm0, bm0);
    unrankBoard(r1, wm1, bm1);
    State s = State(wm0) | (State(bm0) << SQ) | (State(wm1) << (2 * SQ)) |
              (State(bm1) << (3 * SQ));
    return withTurn(s, turn);
}

// Swap the players' identities.  Their home boards swap as well, so this is
// an automorphism of the game graph and changes whose turn it is.
static inline State swapPlayers(State s) {
    uint64_t wm0 = getMask(s, 1, 1), bm0 = getMask(s, 1, 0);
    uint64_t wm1 = getMask(s, 0, 1), bm1 = getMask(s, 0, 0);
    State r = State(wm0) | (State(bm0) << SQ) | (State(wm1) << (2 * SQ)) |
              (State(bm1) << (3 * SQ));
    return withTurn(r, 1 - getTurn(s));
}

static inline State normalizePlayer(State s) {
    return getTurn(s) == 0 ? s : swapPlayers(s);
}

static void initSymmetry() {
    NSYM = (W == H) ? 8 : 4;
    int perm[8][16] = {};
    for (int p = 0; p < SQ; p++) {
        int x = p % W, y = p / W;
        auto put = [&](int g, int nx, int ny) { perm[g][p] = nx + ny * W; };
        put(0, x, y);
        put(1, W - 1 - x, y);
        put(2, x, H - 1 - y);
        put(3, W - 1 - x, H - 1 - y);
        if (W == H) {
            put(4, H - 1 - y, x);
            put(5, y, W - 1 - x);
            put(6, y, x);
            put(7, W - 1 - y, H - 1 - x);
        }
    }
    boardSym.assign(NSYM, vector<int>(MAXB));
    boardMaterial.assign(MAXB, 0);
#pragma omp parallel for schedule(static)
    for (int r = 0; r < MAXB; r++) {
        uint64_t wm, bm;
        unrankBoard(r, wm, bm);
        boardMaterial[r] = (uint8_t)(__builtin_popcountll(wm) +
                                     __builtin_popcountll(bm));
        for (int g = 0; g < NSYM; g++) {
            uint64_t tw = 0, tb = 0;
            uint64_t x = wm;
            while (x) {
                int p = __builtin_ctzll(x); x &= x - 1;
                tw |= 1ull << perm[g][p];
            }
            x = bm;
            while (x) {
                int p = __builtin_ctzll(x); x &= x - 1;
                tb |= 1ull << perm[g][p];
            }
            boardSym[g][r] = rankBoard(tw, tb);
        }
    }

    vector<int> canonicalBoards, stabilizers;
    canonicalBoards.reserve(MAXB / NSYM + 1024);
    vector<int> masks;
    for (int a = 0; a < MAXB; a++) {
        int ca = a;
        for (int g = 1; g < NSYM; g++) ca = min(ca, boardSym[g][a]);
        if (ca != a) continue;
        int mask = 0;
        for (int g = 0; g < NSYM; g++)
            if (boardSym[g][a] == a) mask |= 1 << g;
        canonicalBoards.push_back(a);
        stabilizers.push_back(mask);
        masks.push_back(mask);
    }
    sort(masks.begin(), masks.end());
    masks.erase(unique(masks.begin(), masks.end()), masks.end());
    symSubgroups.clear();
    unordered_map<int, int> subgroupForMask;
    for (int mask : masks) {
        int si = (int)symSubgroups.size();
        subgroupForMask[mask] = si;
        SymSubgroup sub;
        sub.mask = mask;
        if (mask != 1) sub.rank.assign(MAXB, -1);
        sub.materialReps.assign(2 * K + 1, {});
        sub.materialRank.assign(MAXB, -1);
        for (int b = 0; b < MAXB; b++) {
            int cb = b;
            for (int g = 0; g < NSYM; g++)
                if ((mask >> g) & 1) cb = min(cb, boardSym[g][b]);
            if (cb == b) {
                if (mask != 1) {
                    sub.rank[b] = (int)sub.reps.size();
                    sub.reps.push_back(b);
                }
                int m = boardMaterial[b];
                sub.materialRank[b] = (int)sub.materialReps[m].size();
                sub.materialReps[m].push_back(b);
            }
        }
        symSubgroups.push_back(move(sub));
    }

    symRows.clear();
    symRows.reserve(canonicalBoards.size());
    symRowIndex.assign(MAXB, -1);
    int64_t offset = 0;
    for (size_t i = 0; i < canonicalBoards.size(); i++) {
        int si = subgroupForMask[stabilizers[i]];
        int a = canonicalBoards[i];
        symRowIndex[a] = (int)symRows.size();
        symRows.push_back({a, si, offset});
        offset += symSubgroups[si].mask == 1 ? MAXB
                                             : (int64_t)symSubgroups[si].reps.size();
    }
    NSTATES = offset;
    fprintf(stderr, "symmetry: player normalization x2, geometry group order %d; "
                    "%zu first-board orbits, %zu stabilizers, %lld state orbits "
                    "(%.3fx reduction)\n",
            NSYM, symRows.size(), symSubgroups.size(), (long long)NSTATES,
            (double)(2.0L * MAXB * MAXB / NSTATES));
}

static int64_t rankStateSym(State s) {
    s = normalizePlayer(s);
    int a = rankBoard(getMask(s, 0, 0), getMask(s, 0, 1));
    int b = rankBoard(getMask(s, 1, 0), getMask(s, 1, 1));
    if (a < 0 || b < 0) return -1;
    int ca = a, cb = b;
    for (int g = 1; g < NSYM; g++) {
        int ga = boardSym[g][a], gb = boardSym[g][b];
        if (ga < ca || (ga == ca && gb < cb)) { ca = ga; cb = gb; }
    }
    int ri = symRowIndex[ca];
    if (ri < 0) return -1;
    const SymRow &row = symRows[ri];
    const SymSubgroup &sub = symSubgroups[row.subgroup];
    int rb = sub.mask == 1 ? cb : sub.rank[cb];
    return rb < 0 ? -1 : row.offset + rb;
}

static State unrankStateSym(int64_t id) {
    auto it = upper_bound(symRows.begin(), symRows.end(), id,
                          [](int64_t x, const SymRow &r) { return x < r.offset; });
    if (it == symRows.begin()) abort();
    --it;
    int rb = (int)(id - it->offset);
    const SymSubgroup &sub = symSubgroups[it->subgroup];
    int b = sub.mask == 1 ? rb : sub.reps[rb];
    uint64_t wm0, bm0, wm1, bm1;
    unrankBoard(it->board0, wm0, bm0);
    unrankBoard(b, wm1, bm1);
    return State(wm0) | (State(bm0) << SQ) | (State(wm1) << (2 * SQ)) |
           (State(bm1) << (3 * SQ));
}

static int stateOrbitSize(State s) {
    if (!USE_SYM) return 1;
    s = normalizePlayer(s);
    int a = rankBoard(getMask(s, 0, 0), getMask(s, 0, 1));
    int b = rankBoard(getMask(s, 1, 0), getMask(s, 1, 1));
    int stabilizer = 0;
    for (int g = 0; g < NSYM; g++)
        stabilizer += boardSym[g][a] == a && boardSym[g][b] == b;
    return 2 * NSYM / stabilizer;
}

// Dense indexing for one total-stone layer of the symmetry quotient.  The
// global quotient is row-major in canonical board 0.  Here each row retains
// only board-1 representatives whose material completes the requested total.
struct MaterialRow {
    int symRow = 0;
    int board1Material = 0;
    int64_t offset = 0;
};

struct MaterialLayerIndex {
    int total = 0;
    int64_t nstates = 0;
    vector<MaterialRow> rows;
    vector<int64_t> offsetBySymRow;

    void build(int wanted) {
        total = wanted;
        nstates = 0;
        rows.clear();
        offsetBySymRow.assign(symRows.size(), -1);
        for (size_t ri = 0; ri < symRows.size(); ri++) {
            const SymRow &sr = symRows[ri];
            int mb = wanted - boardMaterial[sr.board0];
            if (mb < 0 || mb > 2 * K) continue;
            const SymSubgroup &sub = symSubgroups[sr.subgroup];
            int64_t width = (int64_t)sub.materialReps[mb].size();
            if (width == 0) continue;
            offsetBySymRow[ri] = nstates;
            rows.push_back({(int)ri, mb, nstates});
            nstates += width;
        }
    }

    int64_t rank(State s) const {
        s = normalizePlayer(s);
        int a = rankBoard(getMask(s, 0, 0), getMask(s, 0, 1));
        int b = rankBoard(getMask(s, 1, 0), getMask(s, 1, 1));
        if (a < 0 || b < 0 || boardMaterial[a] + boardMaterial[b] != total)
            return -1;
        int ca = a, cb = b;
        for (int g = 1; g < NSYM; g++) {
            int ga = boardSym[g][a], gb = boardSym[g][b];
            if (ga < ca || (ga == ca && gb < cb)) { ca = ga; cb = gb; }
        }
        int ri = symRowIndex[ca];
        if (ri < 0 || offsetBySymRow[ri] < 0) return -1;
        const SymSubgroup &sub = symSubgroups[symRows[ri].subgroup];
        int rb = sub.materialRank[cb];
        return rb < 0 ? -1 : offsetBySymRow[ri] + rb;
    }

    State unrank(int64_t id) const {
        auto it = upper_bound(rows.begin(), rows.end(), id,
                              [](int64_t x, const MaterialRow &r) {
                                  return x < r.offset;
                              });
        if (it == rows.begin()) abort();
        --it;
        const SymRow &sr = symRows[it->symRow];
        const SymSubgroup &sub = symSubgroups[sr.subgroup];
        int rb = (int)(id - it->offset);
        int b = sub.materialReps[it->board1Material][rb];
        uint64_t wm0, bm0, wm1, bm1;
        unrankBoard(sr.board0, wm0, bm0);
        unrankBoard(b, wm1, bm1);
        return State(wm0) | (State(bm0) << SQ) |
               (State(wm1) << (2 * SQ)) | (State(bm1) << (3 * SQ));
    }
};

static inline int totalStones(State s) {
    int n = 0;
    for (int b = 0; b < 2; b++)
        for (int c = 0; c < 2; c++) n += __builtin_popcountll(getMask(s, b, c));
    return n;
}

struct PackedLayer {
    int total = -1;
    int64_t nstates = 0;
    vector<uint8_t> bytes;

    uint8_t get(int64_t id) const {
        return (bytes[id >> 2] >> (2 * (id & 3))) & 3;
    }
    void set(int64_t id, uint8_t v) {
        uint8_t &x = bytes[id >> 2];
        int sh = 2 * (id & 3);
        x = (uint8_t)((x & ~(3u << sh)) | (v << sh));
    }
};

// Most useful layers fit in 32-bit local ids even when the monolithic table
// does not.  Keeping the frontier local to a layer therefore halves its usual
// memory cost, with an automatic 64-bit fallback for larger layers.
struct LayerQueue {
    bool wide;
    size_t head = 0;
    vector<uint32_t> q32;
    vector<uint64_t> q64;

    explicit LayerQueue(int64_t nstates) : wide((uint64_t)nstates > UINT32_MAX) {}
    void reserve(size_t n) { if (wide) q64.reserve(n); else q32.reserve(n); }
    void push(uint64_t id) {
        if (wide) q64.push_back(id);
        else q32.push_back((uint32_t)id);
    }
    size_t size() const { return wide ? q64.size() : q32.size(); }
    size_t live() const { return size() - head; }
    bool empty() const { return head == size(); }
    uint64_t pop() { return wide ? q64[head++] : q32[head++]; }
    int bytesPerId() const { return wide ? 8 : 4; }
    void compactConsumed() {
        if (wide) {
            vector<uint64_t> liveIds(q64.begin() + head, q64.end());
            q64.swap(liveIds);
        } else {
            vector<uint32_t> liveIds(q32.begin() + head, q32.end());
            q32.swap(liveIds);
        }
        head = 0;
    }
};

// ---- rules ----
static inline bool onboard(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

// terminal: some board missing a color. Returns true if game over.
static inline bool terminalAny(State s) {
    for (int b = 0; b < 2; b++)
        if (getMask(s, b, 0) == 0 || getMask(s, b, 1) == 0) return true;
    return false;
}

// Apply a specific turn: passive psq on P's home board, aggressive asq on the
// other board, heading hidx. Returns STATE_INVALID if illegal.
static State applyTurn(State s, int psq, int asq, int hidx) {
    int P = getTurn(s);
    int home = (P == 0) ? 0 : 1;   // white home board 0, black home board 1
    int away = 1 - home;
    uint64_t oh = getMask(s, home, P), xh = getMask(s, home, 1 - P);
    uint64_t oa = getMask(s, away, P), xa = getMask(s, away, 1 - P);
    if (!((oh >> psq) & 1) || !((oa >> asq) & 1)) return STATE_INVALID;
    Head h = HEADS[hidx];
    int px = psq % W, py = psq / W;
    int ax = asq % W, ay = asq / W;
    // passive: path empty
    int tx = px + h.dx, ty = py + h.dy;
    if (!onboard(tx, ty)) return STATE_INVALID;
    {
        int cx = px + h.ux, cy = py + h.uy;
        for (int i = 0; i < h.dist; i++, cx += h.ux, cy += h.uy) {
            int csq = cx + cy * W;
            if (((oh | xh) >> csq) & 1) return STATE_INVALID;
        }
    }
    int pd = tx + ty * W;
    // aggressive
    int bx = ax + h.dx, by = ay + h.dy;
    if (!onboard(bx, by)) return STATE_INVALID;
    int esq = -1; // pushed enemy square
    {
        int cx = ax + h.ux, cy = ay + h.uy;
        for (int i = 0; i < h.dist; i++, cx += h.ux, cy += h.uy) {
            int csq = cx + cy * W;
            if ((oa >> csq) & 1) return STATE_INVALID;    // pushes own stone
            if ((xa >> csq) & 1) {
                if (esq >= 0) return STATE_INVALID;       // 2 stones in path
                esq = csq;
            }
        }
    }
    int ad = bx + by * W;
    uint64_t noa = (oa & ~(1ull << asq)) | (1ull << ad);
    uint64_t nxa = xa;
    if (esq >= 0) {
        // pushed stone moves by full heading
        int ex = esq % W + h.dx, ey = esq / W + h.dy;
        nxa = xa & ~(1ull << esq);
        if (onboard(ex, ey)) {
            int ed = ex + ey * W;
            if (((oa | xa) >> ed) & 1) return STATE_INVALID; // would push a 2nd stone
            nxa |= 1ull << ed;
        } // else captured (off board)
    }
    uint64_t noh = (oh & ~(1ull << psq)) | (1ull << pd);
    State r = setMask(s, home, P, noh);
    r = setMask(r, away, P, noa);
    r = setMask(r, away, 1 - P, nxa);
    return withTurn(r, 1 - P);
}

// Generate all legal successors, calling cb(successor). Returns move count.
template <typename F>
static int genMoves(State s, F &&cb) {
    int P = getTurn(s);
    int home = (P == 0) ? 0 : 1;
    int away = 1 - home;
    uint64_t oh = getMask(s, home, P), xh = getMask(s, home, 1 - P);
    uint64_t oa = getMask(s, away, P), xa = getMask(s, away, 1 - P);
    int cnt = 0;
    for (int psq = 0; psq < SQ; psq++) {
        if (!((oh >> psq) & 1)) continue;
        int px = psq % W, py = psq / W;
        for (int hi = 0; hi < 16; hi++) {
            Head h = HEADS[hi];
            int tx = px + h.dx, ty = py + h.dy;
            if (!onboard(tx, ty)) continue;
            bool blocked = false;
            int cx = px + h.ux, cy = py + h.uy;
            for (int i = 0; i < h.dist; i++, cx += h.ux, cy += h.uy) {
                if (((oh | xh) >> (cx + cy * W)) & 1) { blocked = true; break; }
            }
            if (blocked) continue;
            int pd = tx + ty * W;
            uint64_t noh = (oh & ~(1ull << psq)) | (1ull << pd);
            for (int asq = 0; asq < SQ; asq++) {
                if (!((oa >> asq) & 1)) continue;
                int ax = asq % W, ay = asq / W;
                int bx = ax + h.dx, by = ay + h.dy;
                if (!onboard(bx, by)) continue;
                int esq = -1;
                bool bad = false;
                int qx = ax + h.ux, qy = ay + h.uy;
                for (int i = 0; i < h.dist; i++, qx += h.ux, qy += h.uy) {
                    int csq = qx + qy * W;
                    if ((oa >> csq) & 1) { bad = true; break; }
                    if ((xa >> csq) & 1) {
                        if (esq >= 0) { bad = true; break; }
                        esq = csq;
                    }
                }
                if (bad) continue;
                int ad = bx + by * W;
                uint64_t noa = (oa & ~(1ull << asq)) | (1ull << ad);
                uint64_t nxa = xa;
                if (esq >= 0) {
                    int ex = esq % W + h.dx, ey = esq / W + h.dy;
                    nxa = xa & ~(1ull << esq);
                    if (onboard(ex, ey)) {
                        int ed = ex + ey * W;
                        if (((oa | xa) >> ed) & 1) continue;
                        nxa |= 1ull << ed;
                    }
                }
                State r = setMask(s, home, P, noh);
                r = setMask(r, away, P, noa);
                r = setMask(r, away, 1 - P, nxa);
                cb(withTurn(r, 1 - P));
                cnt++;
            }
        }
    }
    return cnt;
}

// In the quotient graph, several symmetric moves can lead to the same state
// orbit.  Retrograde counters count successor vertices, not move labels.
static int solveOutdegree(State s) {
    if (!USE_SYM) return genMoves(s, [](State) {});
    int64_t succ[256];
    int n = 0;
    genMoves(s, [&](State t) {
        if (n >= 256) abort();
        succ[n++] = rankState(t);
    });
    sort(succ, succ + n);
    return (int)(unique(succ, succ + n) - succ);
}

// Generate legal predecessor states.  This is the inverse of a complete
// passive+aggressive turn; applyTurn is used as a final exactness check.
template <typename F>
static void genPredecessors(State s, F &&cb) {
    int Q = getTurn(s);
    int P = 1 - Q;
    int home = (P == 0) ? 0 : 1;
    int away = 1 - home;
    uint64_t ph = getMask(s, home, P);
    uint64_t pa = getMask(s, away, P);
    uint64_t qa = getMask(s, away, Q);
    for (int hi = 0; hi < 16; hi++) {
        Head h = HEADS[hi];
        int passO[16], npass = 0;
        uint64_t t = ph;
        while (t) {
            int d = __builtin_ctzll(t); t &= t - 1;
            int dx = d % W, dy = d / W;
            int ox = dx - h.dx, oy = dy - h.dy;
            if (!onboard(ox, oy)) continue;
            int o = ox + oy * W;
            uint64_t allh = getMask(s, home, 0) | getMask(s, home, 1);
            if ((allh >> o) & 1) continue;
            if (h.dist == 2) {
                int mx = dx - h.ux, my = dy - h.uy;
                if ((allh >> (mx + my * W)) & 1) continue;
            }
            passO[npass++] = o;
        }
        if (npass == 0) continue;

        struct Agg { int o2, e, kind; };
        Agg aggs[64]; int nagg = 0;
        uint64_t alla = getMask(s, away, 0) | getMask(s, away, 1);
        t = pa;
        while (t) {
            int d2 = __builtin_ctzll(t); t &= t - 1;
            int dx = d2 % W, dy = d2 / W;
            int ox = dx - h.dx, oy = dy - h.dy;
            if (!onboard(ox, oy)) continue;
            int o2 = ox + oy * W;
            if ((alla >> o2) & 1) continue;
            if (h.dist == 2) {
                int mx = dx - h.ux, my = dy - h.uy;
                if ((alla >> (mx + my * W)) & 1) continue;
            }
            aggs[nagg++] = {o2, -1, 0};
            int pathsq[2], npath = 0;
            pathsq[npath++] = (dx - h.dx + h.ux) +
                              (dy - h.dy + h.uy) * W;
            if (h.dist == 2) pathsq[npath++] = d2;
            for (int pi = 0; pi < npath; pi++) {
                int e = pathsq[pi];
                if (e != d2 && ((alla >> e) & 1)) continue;
                int ex = e % W + h.dx, ey = e / W + h.dy;
                if (onboard(ex, ey)) {
                    int ep = ex + ey * W;
                    if ((qa >> ep) & 1) aggs[nagg++] = {o2, e, 1};
                } else {
                    aggs[nagg++] = {o2, e, 2};
                }
            }
        }

        for (int ai = 0; ai < nagg; ai++) {
            Agg a = aggs[ai];
            uint64_t npa = pa, nqa = qa;
            int d2 = (a.o2 % W + h.dx) + (a.o2 / W + h.dy) * W;
            npa = (npa & ~(1ull << d2)) | (1ull << a.o2);
            if (a.kind == 1) {
                int ex = a.e % W + h.dx, ey = a.e / W + h.dy;
                int ep = ex + ey * W;
                nqa = (nqa & ~(1ull << ep)) | (1ull << a.e);
            } else if (a.kind == 2) {
                nqa |= 1ull << a.e;
            }
            State fixed = setMask(s, away, P, npa);
            fixed = setMask(fixed, away, Q, nqa);
            for (int pj = 0; pj < npass; pj++) {
                int o = passO[pj];
                int d = (o % W + h.dx) + (o / W + h.dy) * W;
                uint64_t nph = (ph & ~(1ull << d)) | (1ull << o);
                State pstate = setMask(fixed, home, P, nph);
                pstate = withTurn(pstate, P);
                if (terminalAny(pstate)) continue;
                if ((getMask(pstate, 0, 0) & getMask(pstate, 0, 1)) ||
                    (getMask(pstate, 1, 0) & getMask(pstate, 1, 1))) continue;
                if (applyTurn(pstate, o, a.o2, hi) == s) cb(pstate);
            }
        }
    }
}

static State initialState() {
    uint64_t wm = 0, bm = 0;
    for (int i = 0; i < K; i++) wm |= 1ull << i;
    for (int i = 0; i < K; i++) bm |= 1ull << (SQ - W + i);
    State s = State(wm) | (State(bm) << SQ) | (State(wm) << (2 * SQ)) |
              (State(bm) << (3 * SQ));
    return withTurn(s, 1); // black first
}

static string layerFilename(const string &prefix, int total) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".layer%02d.wdl", total);
    return prefix + suffix;
}

static bool writePackedLayer(const string &prefix, const PackedLayer &layer) {
    string path = layerFilename(prefix, layer.total);
    string temporary = path + ".tmp";
    FILE *f = fopen(temporary.c_str(), "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", temporary.c_str()); return false; }
    uint32_t magic = 0x4c425753; // 'SWBL': symmetry WDL material layer
    int64_t nbytes = (layer.nstates + 3) / 4;
    bool ok = fwrite(&magic, 4, 1, f) == 1 &&
              fwrite(&W, 4, 1, f) == 1 && fwrite(&H, 4, 1, f) == 1 &&
              fwrite(&K, 4, 1, f) == 1 && fwrite(&layer.total, 4, 1, f) == 1 &&
              fwrite(&layer.nstates, 8, 1, f) == 1 &&
              fwrite(layer.bytes.data(), 1, nbytes, f) == (size_t)nbytes;
    fclose(f);
    if (!ok) { fprintf(stderr, "short write: %s\n", temporary.c_str()); return false; }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "cannot publish %s\n", path.c_str()); return false;
    }
    return true;
}

static bool readPackedLayer(const string &prefix, int total, int64_t expected,
                            PackedLayer &layer) {
    string path = layerFilename(prefix, total);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
    uint32_t magic = 0; int fW, fH, fK, fT; int64_t fN;
    bool ok = fread(&magic, 4, 1, f) == 1 && fread(&fW, 4, 1, f) == 1 &&
              fread(&fH, 4, 1, f) == 1 && fread(&fK, 4, 1, f) == 1 &&
              fread(&fT, 4, 1, f) == 1 && fread(&fN, 8, 1, f) == 1;
    if (!ok || magic != 0x4c425753 || fW != W || fH != H || fK != K ||
        fT != total || fN != expected) {
        fclose(f); fprintf(stderr, "layer mismatch: %s\n", path.c_str()); return false;
    }
    layer.total = total;
    layer.nstates = fN;
    layer.bytes.resize((fN + 3) / 4);
    ok = fread(layer.bytes.data(), 1, layer.bytes.size(), f) == layer.bytes.size();
    fclose(f);
    if (!ok) fprintf(stderr, "truncated layer: %s\n", path.c_str());
    return ok;
}

static int solveLayeredSym(const string &prefix) {
    MaterialLayerIndex prevIndex;
    PackedLayer prevLayer;
    int64_t sumStates = 0, peakStates = 0, peakQueue = 0, peakQueueBytes = 0;
    int64_t totalW = 0, totalL = 0, totalD = 0;
    int resumedLayers = 0;
    uint8_t initialValue = V_UNREACHED;
    int64_t initialLayerId = -1;
    auto allStart = chrono::steady_clock::now();

    for (int material = 0; material <= 4 * K; material++) {
        MaterialLayerIndex index;
        index.build(material);
        int64_t n = index.nstates;
        sumStates += n;
        peakStates = max(peakStates, n);
        fprintf(stderr, "layer %d/%d: %lld orbits (%.3f GB val+outdeg)\n",
                material, 4 * K, (long long)n, (double)n * 3 / 1e9);

        // A published layer was written by atomic rename and is a complete
        // checkpoint.  Load consecutive checkpoints so multi-hour solves can
        // resume after interruption without replaying earlier layers.
        string checkpointPath = layerFilename(prefix, material);
        FILE *checkpoint = fopen(checkpointPath.c_str(), "rb");
        if (checkpoint) {
            fclose(checkpoint);
            PackedLayer completed;
            if (!readPackedLayer(prefix, material, n, completed)) return 1;
            int64_t nw = 0, nl = 0, nd = 0;
#pragma omp parallel for schedule(static) reduction(+ : nw, nl, nd)
            for (int64_t id = 0; id < n; id++) {
                uint8_t v = completed.get(id);
                if (v == V_WIN) nw++;
                else if (v == V_LOSS) nl++;
                else if (v == V_DRAW) nd++;
            }
            if (nw + nl + nd != n) {
                fprintf(stderr, "invalid values in checkpoint %s\n",
                        checkpointPath.c_str());
                return 1;
            }
            totalW += nw; totalL += nl; totalD += nd;
            if (material == 4 * K) {
                initialLayerId = index.rank(initialState());
                initialValue = completed.get(initialLayerId);
            }
            resumedLayers++;
            fprintf(stderr, "layer %d resumed: W=%lld L=%lld D=%lld\n", material,
                    (long long)nw, (long long)nl, (long long)nd);
            prevIndex = move(index);
            prevLayer = move(completed);
            continue;
        }
        uint8_t *layerVal = (uint8_t *)calloc(n ? n : 1, 1);
        uint16_t *remaining = (uint16_t *)calloc(n ? n : 1, 2);
        if (!layerVal || !remaining) {
            fprintf(stderr, "layer allocation failed\n");
            free(layerVal); free(remaining); return 1;
        }

        int64_t terminals = 0, stalemates = 0, lowerWins = 0;
        auto layerStart = chrono::steady_clock::now();
#pragma omp parallel for schedule(dynamic, 2048) reduction(+ : terminals, stalemates, lowerWins)
        for (int64_t id = 0; id < n; id++) {
            State s = index.unrank(id);
            if (terminalAny(s)) {
                layerVal[id] = V_LOSS;
                terminals++;
                continue;
            }
            uint64_t succ[256]; int nsucc = 0;
            genMoves(s, [&](State t) {
                int mt = totalStones(t);
                int64_t tid = -1;
                int lower = 0;
                if (mt == material) tid = index.rank(t);
                else if (mt == material - 1) { tid = prevIndex.rank(t); lower = 1; }
                else abort();
                if (tid < 0 || nsucc >= 256) abort();
                succ[nsucc++] = ((uint64_t)tid << 1) | (uint64_t)lower;
            });
            sort(succ, succ + nsucc);
            nsucc = (int)(unique(succ, succ + nsucc) - succ);
            uint16_t rem = (uint16_t)nsucc;
            bool isWin = false;
            for (int i = 0; i < nsucc; i++) {
                if (!(succ[i] & 1)) continue;
                uint8_t v = prevLayer.get((int64_t)(succ[i] >> 1));
                if (v == V_LOSS) isWin = true;
                else if (v == V_WIN) rem--;
            }
            remaining[id] = rem;
            if (isWin) { layerVal[id] = V_WIN; lowerWins++; }
            else if (rem == 0) { layerVal[id] = V_LOSS; stalemates++; }
        }

        LayerQueue queue(n);
        queue.reserve((size_t)min<int64_t>(n, 1 << 20));
        for (int64_t id = 0; id < n; id++)
            if (layerVal[id] == V_WIN || layerVal[id] == V_LOSS)
                queue.push((uint64_t)id);
        peakQueue = max<int64_t>(peakQueue, (int64_t)queue.live());
        peakQueueBytes = max<int64_t>(peakQueueBytes,
                                       (int64_t)queue.live() * queue.bytesPerId());
        while (!queue.empty()) {
            uint64_t sid = queue.pop();
            State s = index.unrank((int64_t)sid);
            vector<int64_t> preds;
            preds.reserve(512);
            genPredecessors(s, [&](State p) {
                if (totalStones(p) != material) return;
                int64_t pid = index.rank(p);
                if (pid < 0) abort();
                preds.push_back(pid);
            });
            sort(preds.begin(), preds.end());
            preds.erase(unique(preds.begin(), preds.end()), preds.end());
            for (int64_t pid : preds) {
                if (layerVal[pid] != V_UNREACHED) continue;
                if (layerVal[sid] == V_LOSS) {
                    layerVal[pid] = V_WIN;
                    queue.push((uint64_t)pid);
                } else if (--remaining[pid] == 0) {
                    layerVal[pid] = V_LOSS;
                    queue.push((uint64_t)pid);
                }
            }
            peakQueue = max<int64_t>(peakQueue, (int64_t)queue.live());
            peakQueueBytes = max<int64_t>(peakQueueBytes,
                                           (int64_t)queue.live() * queue.bytesPerId());
            if (queue.head >= (1u << 24) && queue.head > queue.live()) {
                queue.compactConsumed();
            }
        }

        int64_t nw = 0, nl = 0, nd = 0;
#pragma omp parallel for schedule(static) reduction(+ : nw, nl, nd)
        for (int64_t id = 0; id < n; id++) {
            if (layerVal[id] == V_UNREACHED) layerVal[id] = V_DRAW;
            if (layerVal[id] == V_WIN) nw++;
            else if (layerVal[id] == V_LOSS) nl++;
            else nd++;
        }

        // Local Bellman check against this layer and the already final T-1.
        int64_t errors = 0;
#pragma omp parallel for schedule(dynamic, 2048) reduction(+ : errors)
        for (int64_t id = 0; id < n; id++) {
            State s = index.unrank(id);
            uint8_t expected;
            if (terminalAny(s)) expected = V_LOSS;
            else {
                bool anyLoss = false, allWin = true;
                int moves = genMoves(s, [&](State t) {
                    int mt = totalStones(t);
                    int64_t tid = mt == material ? index.rank(t) : prevIndex.rank(t);
                    uint8_t v = mt == material ? layerVal[tid] : prevLayer.get(tid);
                    if (v == V_LOSS) anyLoss = true;
                    if (v != V_WIN) allWin = false;
                });
                expected = anyLoss ? V_WIN : ((moves == 0 || allWin) ? V_LOSS : V_DRAW);
            }
            if (layerVal[id] != expected) errors++;
        }
        if (errors) {
            fprintf(stderr, "layer %d verification failed: %lld errors\n",
                    material, (long long)errors);
            free(layerVal); free(remaining); return 1;
        }

        PackedLayer current;
        current.total = material;
        current.nstates = n;
        current.bytes.assign((n + 3) / 4, 0);
        for (int64_t id = 0; id < n; id++) current.set(id, layerVal[id]);
        if (!writePackedLayer(prefix, current)) {
            free(layerVal); free(remaining); return 1;
        }
        if (material == 4 * K) {
            initialLayerId = index.rank(initialState());
            initialValue = current.get(initialLayerId);
        }
        totalW += nw; totalL += nl; totalD += nd;
        auto layerEnd = chrono::steady_clock::now();
        fprintf(stderr,
                "layer %d done: W=%lld L=%lld D=%lld terminal=%lld seeds-from-lower=%lld "
                "verified, %.1fs\n",
                material, (long long)nw, (long long)nl, (long long)nd,
                (long long)terminals, (long long)lowerWins,
                chrono::duration<double>(layerEnd - layerStart).count());
        free(layerVal);
        free(remaining);
        prevIndex = move(index);
        prevLayer = move(current);
    }

    const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
    string summary = prefix + ".layered.summary.txt";
    FILE *g = fopen(summary.c_str(), "w");
    if (!g) { fprintf(stderr, "cannot write %s\n", summary.c_str()); return 1; }
    int64_t raw = (int64_t)MAXB * MAXB * 2;
    fprintf(g, "Mini-Shobu W=%d H=%d K=%d (symmetry quotient, material-layered WDL)\n",
            W, H, K);
    fprintf(g, "mathematical states: %lld; state orbits: %lld (%.3fx reduction)\n",
            (long long)raw, (long long)sumStates, (double)raw / sumStates);
    fprintf(g, "all-orbit WDL: WIN=%lld LOSS=%lld DRAW=%lld\n",
            (long long)totalW, (long long)totalL, (long long)totalD);
    fprintf(g, "peak layer: %lld orbits; core arrays: %.3f GB; "
               "peak live queue: %lld ids / %.3f GB\n",
            (long long)peakStates, (double)peakStates * 3 / 1e9,
            (long long)peakQueue, (double)peakQueueBytes / 1e9);
    fprintf(g, "layer files: 2 bits/orbit; no DTM; each layer Bellman-verified\n");
    fprintf(g, "layers resumed from checkpoints in this invocation: %d\n", resumedLayers);
    fprintf(g, "initial state layer=%d id=%lld value=%s\n",
            4 * K, (long long)initialLayerId, vname[initialValue]);
    fclose(g);
    auto allEnd = chrono::steady_clock::now();
    fprintf(stderr, "layered solve complete: %lld/%lld orbits, %.1fs, initial=%s\n",
            (long long)sumStates, (long long)NSTATES,
            chrono::duration<double>(allEnd - allStart).count(), vname[initialValue]);
    if (sumStates != NSTATES) {
        fprintf(stderr, "layer partition mismatch\n"); return 1;
    }
    return 0;
}

// Cross-check a layered WDL solution against every non-UNREACHED entry in a
// conventional symmetry table.  DTM is intentionally not part of the new
// representation.
static int compareLayeredSym(const string &monolithic, const string &prefix) {
    FILE *f = fopen(monolithic.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", monolithic.c_str()); return 1; }
    uint32_t magic; int fW, fH, fK; int64_t fNS;
    bool ok = fread(&magic, 4, 1, f) == 1 && fread(&fW, 4, 1, f) == 1 &&
              fread(&fH, 4, 1, f) == 1 && fread(&fK, 4, 1, f) == 1 &&
              fread(&fNS, 8, 1, f) == 1;
    if (!ok || magic != 0x53534257 || fW != W || fH != H || fK != K ||
        fNS != NSTATES) {
        fclose(f); fprintf(stderr, "monolithic table mismatch\n"); return 1;
    }
    vector<uint8_t> old(NSTATES);
    if (fread(old.data(), 1, old.size(), f) != old.size()) {
        fclose(f); fprintf(stderr, "truncated monolithic table\n"); return 1;
    }
    fclose(f);
    int64_t checked = 0, errors = 0;
    for (int material = 0; material <= 4 * K; material++) {
        MaterialLayerIndex index; index.build(material);
        PackedLayer layer;
        if (!readPackedLayer(prefix, material, index.nstates, layer)) return 1;
#pragma omp parallel for schedule(static) reduction(+ : checked, errors)
        for (int64_t id = 0; id < index.nstates; id++) {
            int64_t gid = rankState(index.unrank(id));
            if (old[gid] == V_UNREACHED) continue;
            checked++;
            if (old[gid] != layer.get(id)) errors++;
        }
    }
    fprintf(stderr, "layer comparison: checked=%lld reachable entries, errors=%lld\n",
            (long long)checked, (long long)errors);
    return errors ? 1 : 0;
}

// Memory-scalable parallel WDL solver for very large material layers.  Values
// and residual degrees occupy one byte each.  Resolved frontiers are bitmaps
// rather than id vectors, bounding the live-frontier cost at two bits/state.
// Each wave first propagates LOSS children (which establish predecessor WINs),
// then propagates WIN children (which may exhaust predecessor residuals).  The
// barrier between those passes gives WIN precedence and makes relaxed atomic
// updates exact.
static int solveParallelLayeredSym(const string &prefix) {
    int maxHeadingsFromSquare = 0;
    for (int sq = 0; sq < SQ; sq++) {
        int count = 0;
        for (const Head &h : HEADS)
            if (onboard(sq % W + h.dx, sq / W + h.dy)) count++;
        maxHeadingsFromSquare = max(maxHeadingsFromSquare, count);
    }
    int rawMoveBound = K * K * maxHeadingsFromSquare;
    if (rawMoveBound > 255) {
        fprintf(stderr, "parallel byte-degree mode unsupported: move bound %d > 255\n",
                rawMoveBound);
        return 1;
    }
    fprintf(stderr, "parallel byte-degree proof: raw move bound %d <= 255\n",
            rawMoveBound);
    MaterialLayerIndex prevIndex;
    PackedLayer prevLayer;
    int64_t sumStates = 0, peakStates = 0;
    int64_t totalW = 0, totalL = 0, totalD = 0;
    int resumedLayers = 0;
    uint8_t initialValue = V_UNREACHED;
    int64_t initialLayerId = -1;
    auto allStart = chrono::steady_clock::now();

    for (int material = 0; material <= 4 * K; material++) {
        MaterialLayerIndex index;
        index.build(material);
        int64_t n = index.nstates;
        int64_t nwords = (n + 63) / 64;
        sumStates += n;
        peakStates = max(peakStates, n);
        fprintf(stderr, "parallel layer %d/%d: %lld orbits "
                        "(%.3f GB value+degree+frontiers)\n",
                material, 4 * K, (long long)n, (double)n * 2.25 / 1e9);

        string checkpointPath = layerFilename(prefix, material);
        FILE *checkpoint = fopen(checkpointPath.c_str(), "rb");
        if (checkpoint) {
            fclose(checkpoint);
            PackedLayer completed;
            if (!readPackedLayer(prefix, material, n, completed)) return 1;
            int64_t nw = 0, nl = 0, nd = 0;
#pragma omp parallel for schedule(static) reduction(+ : nw, nl, nd)
            for (int64_t id = 0; id < n; id++) {
                uint8_t v = completed.get(id);
                if (v == V_WIN) nw++;
                else if (v == V_LOSS) nl++;
                else if (v == V_DRAW) nd++;
            }
            if (nw + nl + nd != n) {
                fprintf(stderr, "invalid values in checkpoint %s\n",
                        checkpointPath.c_str());
                return 1;
            }
            totalW += nw; totalL += nl; totalD += nd;
            if (material == 4 * K) {
                initialLayerId = index.rank(initialState());
                initialValue = completed.get(initialLayerId);
            }
            resumedLayers++;
            fprintf(stderr, "parallel layer %d resumed: W=%lld L=%lld D=%lld\n",
                    material, (long long)nw, (long long)nl, (long long)nd);
            prevIndex = move(index);
            prevLayer = move(completed);
            continue;
        }

        uint8_t *layerVal = (uint8_t *)malloc(n ? n : 1);
        uint8_t *remaining = (uint8_t *)malloc(n ? n : 1);
        if (!layerVal || !remaining) {
            fprintf(stderr, "parallel layer allocation failed\n");
            free(layerVal); free(remaining); return 1;
        }
        vector<uint64_t> frontier(nwords, 0), next(nwords, 0);
        int64_t terminals = 0, exhaustedSeeds = 0, lowerWins = 0;
        auto layerStart = chrono::steady_clock::now();

#pragma omp parallel for schedule(dynamic, 2048) reduction(+ : terminals, exhaustedSeeds, lowerWins)
        for (int64_t id = 0; id < n; id++) {
            State s = index.unrank(id);
            remaining[id] = 0;
            if (terminalAny(s)) {
                layerVal[id] = V_LOSS;
                terminals++;
                continue;
            }
            uint64_t succ[256]; int nsucc = 0;
            genMoves(s, [&](State t) {
                int mt = totalStones(t);
                int64_t tid = -1; int lower = 0;
                if (mt == material) tid = index.rank(t);
                else if (mt == material - 1) { tid = prevIndex.rank(t); lower = 1; }
                else abort();
                if (tid < 0 || nsucc >= 256) abort();
                succ[nsucc++] = ((uint64_t)tid << 1) | (uint64_t)lower;
            });
            sort(succ, succ + nsucc);
            nsucc = (int)(unique(succ, succ + nsucc) - succ);
            if (nsucc > 255) abort();
            int rem = nsucc;
            bool isWin = false;
            for (int i = 0; i < nsucc; i++) {
                if (!(succ[i] & 1)) continue;
                uint8_t v = prevLayer.get((int64_t)(succ[i] >> 1));
                if (v == V_LOSS) isWin = true;
                else if (v == V_WIN) rem--;
            }
            remaining[id] = (uint8_t)rem;
            if (isWin) { layerVal[id] = V_WIN; lowerWins++; }
            else if (rem == 0) { layerVal[id] = V_LOSS; exhaustedSeeds++; }
            else layerVal[id] = V_UNREACHED;
        }

        int64_t frontierCount = 0;
#pragma omp parallel for schedule(static) reduction(+ : frontierCount)
        for (int64_t wi = 0; wi < nwords; wi++) {
            uint64_t bits = 0;
            int64_t base = wi << 6;
            int lim = (int)min<int64_t>(64, n - base);
            for (int j = 0; j < lim; j++)
                if (layerVal[base + j] != V_UNREACHED) bits |= 1ull << j;
            frontier[wi] = bits;
            frontierCount += __builtin_popcountll(bits);
        }

        int wave = 0;
        while (frontierCount) {
            wave++;
            // LOSS successors establish predecessor WINs before any residual
            // decrements in this wave.
#pragma omp parallel for schedule(static)
            for (int64_t wi = 0; wi < nwords; wi++) {
                uint64_t bits = frontier[wi];
                while (bits) {
                    int bit = __builtin_ctzll(bits); bits &= bits - 1;
                    int64_t sid = (wi << 6) + bit;
                    if (__atomic_load_n(&layerVal[sid], __ATOMIC_RELAXED) != V_LOSS)
                        continue;
                    State s = index.unrank(sid);
                    int64_t preds[1024]; int npred = 0;
                    genPredecessors(s, [&](State p) {
                        if (totalStones(p) != material) return;
                        int64_t pid = index.rank(p);
                        if (pid < 0 || npred >= 1024) abort();
                        preds[npred++] = pid;
                    });
                    sort(preds, preds + npred);
                    npred = (int)(unique(preds, preds + npred) - preds);
                    for (int i = 0; i < npred; i++) {
                        int64_t pid = preds[i];
                        uint8_t expected = V_UNREACHED;
                        if (__atomic_compare_exchange_n(&layerVal[pid], &expected,
                                (uint8_t)V_WIN, false, __ATOMIC_RELAXED,
                                __ATOMIC_RELAXED))
                            __atomic_fetch_or(&next[pid >> 6], 1ull << (pid & 63),
                                              __ATOMIC_RELAXED);
                    }
                }
            }

            // WIN successors consume one distinct successor from every still
            // unresolved predecessor.  The last one establishes a LOSS.
#pragma omp parallel for schedule(static)
            for (int64_t wi = 0; wi < nwords; wi++) {
                uint64_t bits = frontier[wi];
                while (bits) {
                    int bit = __builtin_ctzll(bits); bits &= bits - 1;
                    int64_t sid = (wi << 6) + bit;
                    if (__atomic_load_n(&layerVal[sid], __ATOMIC_RELAXED) != V_WIN)
                        continue;
                    State s = index.unrank(sid);
                    int64_t preds[1024]; int npred = 0;
                    genPredecessors(s, [&](State p) {
                        if (totalStones(p) != material) return;
                        int64_t pid = index.rank(p);
                        if (pid < 0 || npred >= 1024) abort();
                        preds[npred++] = pid;
                    });
                    sort(preds, preds + npred);
                    npred = (int)(unique(preds, preds + npred) - preds);
                    for (int i = 0; i < npred; i++) {
                        int64_t pid = preds[i];
                        // A separate value check followed by fetch_sub can
                        // underflow: another thread may perform the last
                        // decrement and publish LOSS between those actions.
                        // CAS the positive residual instead; a contender that
                        // observes zero simply leaves the winner to publish.
                        while (__atomic_load_n(&layerVal[pid], __ATOMIC_RELAXED) ==
                               V_UNREACHED) {
                            uint8_t old = __atomic_load_n(&remaining[pid],
                                                          __ATOMIC_RELAXED);
                            if (old == 0) break;
                            uint8_t desired = (uint8_t)(old - 1);
                            if (!__atomic_compare_exchange_n(&remaining[pid], &old,
                                    desired, true, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
                                continue;
                            if (old == 1) {
                                uint8_t expected = V_UNREACHED;
                                if (__atomic_compare_exchange_n(&layerVal[pid], &expected,
                                        (uint8_t)V_LOSS, false, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
                                    __atomic_fetch_or(&next[pid >> 6],
                                                      1ull << (pid & 63),
                                                      __ATOMIC_RELAXED);
                            }
                            break;
                        }
                    }
                }
            }

            int64_t nextCount = 0;
#pragma omp parallel for schedule(static) reduction(+ : nextCount)
            for (int64_t wi = 0; wi < nwords; wi++)
                nextCount += __builtin_popcountll(next[wi]);
            if (wave == 1 || wave % 10 == 0 || nextCount == 0)
                fprintf(stderr, "  layer %d wave %d: frontier=%lld next=%lld\n",
                        material, wave, (long long)frontierCount, (long long)nextCount);
            frontier.swap(next);
#pragma omp parallel for schedule(static)
            for (int64_t wi = 0; wi < nwords; wi++) next[wi] = 0;
            frontierCount = nextCount;
        }

        int64_t nw = 0, nl = 0, nd = 0;
#pragma omp parallel for schedule(static) reduction(+ : nw, nl, nd)
        for (int64_t id = 0; id < n; id++) {
            if (layerVal[id] == V_UNREACHED) layerVal[id] = V_DRAW;
            if (layerVal[id] == V_WIN) nw++;
            else if (layerVal[id] == V_LOSS) nl++;
            else nd++;
        }
        free(remaining);
        vector<uint64_t>().swap(frontier);
        vector<uint64_t>().swap(next);

        int64_t errors = 0;
#pragma omp parallel for schedule(dynamic, 2048) reduction(+ : errors)
        for (int64_t id = 0; id < n; id++) {
            State s = index.unrank(id);
            uint8_t expected;
            if (terminalAny(s)) expected = V_LOSS;
            else {
                bool anyLoss = false, allWin = true;
                int moves = genMoves(s, [&](State t) {
                    int mt = totalStones(t);
                    int64_t tid = mt == material ? index.rank(t) : prevIndex.rank(t);
                    uint8_t v = mt == material ? layerVal[tid] : prevLayer.get(tid);
                    if (v == V_LOSS) anyLoss = true;
                    if (v != V_WIN) allWin = false;
                });
                expected = anyLoss ? V_WIN : ((moves == 0 || allWin) ? V_LOSS : V_DRAW);
            }
            if (layerVal[id] != expected) errors++;
        }
        if (errors) {
            fprintf(stderr, "parallel layer %d verification failed: %lld errors\n",
                    material, (long long)errors);
            free(layerVal); return 1;
        }

        PackedLayer current;
        current.total = material;
        current.nstates = n;
        current.bytes.assign((n + 3) / 4, 0);
#pragma omp parallel for schedule(static)
        for (int64_t wi = 0; wi < (n + 3) / 4; wi++) {
            uint8_t packed = 0;
            int64_t base = wi * 4;
            for (int j = 0; j < 4 && base + j < n; j++)
                packed |= layerVal[base + j] << (2 * j);
            current.bytes[wi] = packed;
        }
        free(layerVal);
        if (!writePackedLayer(prefix, current)) return 1;
        if (material == 4 * K) {
            initialLayerId = index.rank(initialState());
            initialValue = current.get(initialLayerId);
        }
        totalW += nw; totalL += nl; totalD += nd;
        auto layerEnd = chrono::steady_clock::now();
        fprintf(stderr, "parallel layer %d done: W=%lld L=%lld D=%lld "
                        "terminal=%lld lower-win-seeds=%lld waves=%d verified, %.1fs\n",
                material, (long long)nw, (long long)nl, (long long)nd,
                (long long)terminals, (long long)lowerWins, wave,
                chrono::duration<double>(layerEnd - layerStart).count());
        prevIndex = move(index);
        prevLayer = move(current);
    }

    const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
    string summary = prefix + ".parallel.summary.txt";
    FILE *g = fopen(summary.c_str(), "w");
    if (!g) { fprintf(stderr, "cannot write %s\n", summary.c_str()); return 1; }
    int64_t raw = (int64_t)MAXB * MAXB * 2;
    fprintf(g, "Mini-Shobu W=%d H=%d K=%d (parallel bitmap layered WDL)\n", W, H, K);
    fprintf(g, "mathematical states: %lld; state orbits: %lld (%.3fx reduction)\n",
            (long long)raw, (long long)sumStates, (double)raw / sumStates);
    fprintf(g, "all-orbit WDL: WIN=%lld LOSS=%lld DRAW=%lld\n",
            (long long)totalW, (long long)totalL, (long long)totalD);
    fprintf(g, "peak layer: %lld orbits; value+degree+frontiers: %.3f GB\n",
            (long long)peakStates, (double)peakStates * 2.25 / 1e9);
    fprintf(g, "layers resumed from checkpoints in this invocation: %d\n", resumedLayers);
    fprintf(g, "initial state layer=%d id=%lld value=%s\n",
            4 * K, (long long)initialLayerId, vname[initialValue]);
    fclose(g);
    auto allEnd = chrono::steady_clock::now();
    fprintf(stderr, "parallel layered solve complete: %lld/%lld orbits, %.1fs, initial=%s\n",
            (long long)sumStates, (long long)NSTATES,
            chrono::duration<double>(allEnd - allStart).count(), vname[initialValue]);
    return sumStates == NSTATES ? 0 : 1;
}

// globals for solve
static uint8_t *val;
static uint16_t *depth;
static uint16_t *outdeg;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
    string mode = argv[1];
    USE_SYM = mode.size() >= 4 && mode.substr(mode.size() - 4) == "-sym";
    if (mode == "count-layered-sym" && argc >= 5) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        int64_t sum = 0, peak = 0, previous = 0;
        long double peakParallelBytes = 0;
        for (int material = 0; material <= 4 * K; material++) {
            MaterialLayerIndex index; index.build(material);
            printf("material=%d orbits=%lld core_arrays=%.6f_GB\n", material,
                   (long long)index.nstates, (double)index.nstates * 3 / 1e9);
            sum += index.nstates;
            peak = max(peak, index.nstates);
            peakParallelBytes = max(peakParallelBytes,
                    (long double)index.nstates * 2.25L + (long double)previous * 0.25L);
            previous = index.nstates;
        }
        printf("total_orbits=%lld expected=%lld peak_orbits=%lld peak_core_arrays=%.6f_GB "
               "parallel_peak_with_previous=%.6Lf_GB monolithic_core_arrays=%.6f_GB "
               "reduction=%.3fx\n",
               (long long)sum, (long long)NSTATES, (long long)peak,
               (double)peak * 3 / 1e9, peakParallelBytes / 1e9L,
               (double)NSTATES * 5 / 1e9,
               (double)NSTATES * 5 / ((double)peak * 3));
        return sum == NSTATES ? 0 : 1;
    }
    if (mode == "solve-layered-sym" && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        return solveLayeredSym(argv[5]);
    }
    if (mode == "solve-parallel-sym" && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        return solveParallelLayeredSym(argv[5]);
    }
    if (mode == "compare-layered-sym" && argc >= 7) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        return compareLayeredSym(argv[5], argv[6]);
    }
    if (mode == "query-layered-sym" && argc >= 8) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        string prefix = argv[5], bstr = argv[6];
        int turn = (argv[7][0] == 'b') ? 1 : 0;
        if ((int)bstr.size() != 2 * SQ) {
            fprintf(stderr, "bad board string len\n"); return 1;
        }
        State s = 0;
        for (int b = 0; b < 2; b++)
            for (int i = 0; i < SQ; i++) {
                char c = bstr[b * SQ + i];
                if (c == 'o') s = setMask(s, b, 0,
                                           getMask(s, b, 0) | (1ull << i));
                else if (c == 'x') s = setMask(s, b, 1,
                                                getMask(s, b, 1) | (1ull << i));
                else if (c != '.') { fprintf(stderr, "bad board character\n"); return 1; }
            }
        s = withTurn(s, turn);
        int material = totalStones(s);
        MaterialLayerIndex currentIndex; currentIndex.build(material);
        PackedLayer current;
        if (!readPackedLayer(prefix, material, currentIndex.nstates, current)) return 1;
        MaterialLayerIndex lowerIndex;
        PackedLayer lower;
        if (material > 0) {
            lowerIndex.build(material - 1);
            if (!readPackedLayer(prefix, material - 1, lowerIndex.nstates, lower)) return 1;
        }
        int64_t id = currentIndex.rank(s);
        if (id < 0) { printf("position outside solved space\n"); return 0; }
        const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
        printf("material=%d layer_id=%lld value=%s\n", material,
               (long long)id, vname[current.get(id)]);
        if (terminalAny(s)) { printf("(terminal)\n"); return 0; }
        for (int psq = 0; psq < SQ; psq++)
            for (int hi = 0; hi < 16; hi++)
                for (int asq = 0; asq < SQ; asq++) {
                    State t = applyTurn(s, psq, asq, hi);
                    if (t == STATE_INVALID) continue;
                    int mt = totalStones(t);
                    int64_t tid = mt == material ? currentIndex.rank(t)
                                                 : lowerIndex.rank(t);
                    uint8_t tv = mt == material ? current.get(tid) : lower.get(tid);
                    Head h = HEADS[hi];
                    printf("  passive %c%d->%c%d h=(%+d,%+d) aggressive from %c%d -> %s\n",
                           'a' + psq % W, psq / W + 1,
                           'a' + (psq % W + h.dx), psq / W + h.dy + 1,
                           h.dx, h.dy, 'a' + asq % W, asq / W + 1, vname[tv]);
                }
        return 0;
    }
    if ((mode == "count" || mode == "count-sym") && argc >= 5) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        int64_t raw = (int64_t)MAXB * MAXB * 2;
        printf("W=%d H=%d K=%d per_board=%d mathematical_states=%lld\n",
               W, H, K, MAXB, (long long)raw);
        if (USE_SYM)
            printf("state_orbits=%lld reduction=%.6f working_arrays=%.3f_GB output=%.3f_GB\n",
                   (long long)NSTATES, (double)raw / NSTATES,
                   (double)NSTATES * 5 / 1e9, (double)NSTATES * 3 / 1e9);
        return 0;
    }
    if ((mode == "solve" || mode == "solve-sym") && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        string outprefix = argv[5];
        SQ = W * H;
        initTables();
        fprintf(stderr, "W=%d H=%d K=%d MAXB=%d NSTATES=%lld (%.2f GB val+depth+outdeg)\n",
                W, H, K, MAXB, (long long)NSTATES,
                (double)NSTATES * 5 / 1e9);
        val = (uint8_t *)calloc(NSTATES, 1);
        depth = (uint16_t *)calloc(NSTATES, 2);
        outdeg = (uint16_t *)calloc(NSTATES, 2);
        if (!val || !depth || !outdeg) { fprintf(stderr, "alloc fail\n"); return 1; }

        auto t0 = chrono::steady_clock::now();
        // Phase 1: outdegrees + terminal seeds. Parallel over id ranges.
        vector<uint64_t> queue;
        int64_t nterm = 0, nstale = 0;
#pragma omp parallel for schedule(static) reduction(+ : nterm, nstale)
        for (int64_t id = 0; id < NSTATES; id++) {
            State s = unrankState(id);
            if (terminalAny(s)) {
                // player to move has lost (a board has zero of their stones).
                val[id] = V_LOSS; // depth 0
                nterm++;
                continue;
            }
            int c = solveOutdegree(s);
            outdeg[id] = (uint16_t)c;
            if (c == 0) { val[id] = V_LOSS; nstale++; } // stalemate = loss
        }
        auto t1 = chrono::steady_clock::now();
        fprintf(stderr, "phase1 (outdeg): %.1fs  terminals=%lld stalemates=%lld\n",
                chrono::duration<double>(t1 - t0).count(),
                (long long)nterm, (long long)nstale);

        queue.reserve(1 << 20);
        for (int64_t id = 0; id < NSTATES; id++)
            if (val[id] == V_LOSS) queue.push_back((uint64_t)id);

        // Phase 2: retrograde.
        size_t qh = 0;
        uint64_t processed = 0;
        int64_t nwin = 0, nloss = nterm + nstale;
        int64_t lastReport = 0;
        while (qh < queue.size()) {
            uint64_t sid = queue[qh++];
            processed++;
            State s = unrankState(sid);
            int Q = getTurn(s);         // player to move at s
            int P = 1 - Q;              // predecessor's player
            int home = (P == 0) ? 0 : 1;
            int away = 1 - home;
            uint64_t ph = getMask(s, home, P);   // P stones on home board
            uint64_t pa = getMask(s, away, P);   // P stones on away board
            uint64_t qa = getMask(s, away, Q);   // Q stones on away board
            int64_t allPreds[4096]; int nall = 0;
            auto processPred = [&](int64_t pid) {
                if (val[pid] != 0) return; // already decided
                if (val[sid] == V_LOSS) {
                    val[pid] = V_WIN;
                    depth[pid] = depth[sid] + 1;
                    queue.push_back((uint64_t)pid);
                    nwin++;
                } else { // V_WIN: resolve p only when all succs are WIN
                    uint16_t nd = depth[sid] + 1;
                    if (nd > depth[pid]) depth[pid] = nd;
                    if (--outdeg[pid] == 0) {
                        val[pid] = V_LOSS;
                        queue.push_back((uint64_t)pid);
                        nloss++;
                    }
                }
            };
            // for each heading
            for (int hi = 0; hi < 16; hi++) {
                Head h = HEADS[hi];
                // passive reverse candidates: P stone moved o->d, o=d-h
                // collect list of o squares on home board
                int passO[16]; int npass = 0;
                {
                    uint64_t t = ph;
                    while (t) {
                        int d = __builtin_ctzll(t); t &= t - 1;
                        int dx = d % W, dy = d / W;
                        int ox = dx - h.dx, oy = dy - h.dy;
                        if (!onboard(ox, oy)) continue;
                        int o = ox + oy * W;
                        // o must be empty in s (home board), and intermediate
                        // square (dist 2) must be empty too
                        uint64_t allh = getMask(s, home, 0) | getMask(s, home, 1);
                        if ((allh >> o) & 1) continue;
                        if (h.dist == 2) {
                            int mx = dx - h.ux, my = dy - h.uy;
                            if ((allh >> (mx + my * W)) & 1) continue;
                        }
                        passO[npass++] = o;
                    }
                }
                if (npass == 0) continue;
                // aggressive reverse candidates on away board.
                // each candidate: (o2, e, kind) kind: 0=nopush, 1=push(e'->e),
                // 2=capture(add Q at e)
                struct Agg { int o2; int e; int kind; };
                Agg aggs[64]; int nagg = 0;
                {
                    uint64_t alla = getMask(s, away, 0) | getMask(s, away, 1);
                    uint64_t t = pa;
                    while (t) {
                        int d2 = __builtin_ctzll(t); t &= t - 1;
                        int dx = d2 % W, dy = d2 / W;
                        int ox = dx - h.dx, oy = dy - h.dy;
                        if (!onboard(ox, oy)) continue;
                        int o2 = ox + oy * W;
                        if ((alla >> o2) & 1) continue; // origin must be empty in s
                        if (h.dist == 2) {
                            int mx = dx - h.ux, my = dy - h.uy;
                            if ((alla >> (mx + my * W)) & 1) continue;
                        }
                        // case 0: no push
                        aggs[nagg++] = {o2, -1, 0};
                        // push cases: e in path squares {o2+u, (d2 if dist2)}
                        int pathsq[2]; int npath = 0;
                        pathsq[npath++] = (dx - h.dx + h.ux) + (dy - h.dy + h.uy) * W; // o2+u
                        if (h.dist == 2) pathsq[npath++] = d2;
                        for (int pi = 0; pi < npath; pi++) {
                            int e = pathsq[pi];
                            if (e != d2 && ((alla >> e) & 1)) continue; // must be empty in s
                            int ex = e % W + h.dx, ey = e / W + h.dy;
                            if (onboard(ex, ey)) {
                                int ep = ex + ey * W;
                                if ((qa >> ep) & 1) // Q stone at e' in s
                                    aggs[nagg++] = {o2, e, 1};
                            } else {
                                aggs[nagg++] = {o2, e, 2}; // capture
                            }
                        }
                    }
                }
                // combine and verify
                int64_t preds[256]; int npred = 0;
                for (int ai = 0; ai < nagg; ai++) {
                    // build away-board masks of predecessor
                    uint64_t npa = pa, nqa = qa;
                    Agg a = aggs[ai];
                    // find d2 = o2 + h
                    int d2 = (a.o2 % W + h.dx) + (a.o2 / W + h.dy) * W;
                    npa = (npa & ~(1ull << d2)) | (1ull << a.o2);
                    if (a.kind == 1) {
                        int ex = a.e % W + h.dx, ey = a.e / W + h.dy;
                        int ep = ex + ey * W;
                        nqa = (nqa & ~(1ull << ep)) | (1ull << a.e);
                    } else if (a.kind == 2) {
                        nqa = nqa | (1ull << a.e);
                    }
                    State s_away_fixed = setMask(s, away, P, npa);
                    s_away_fixed = setMask(s_away_fixed, away, Q, nqa);
                    for (int pj = 0; pj < npass; pj++) {
                        int o = passO[pj];
                        int d = (o % W + h.dx) + (o / W + h.dy) * W;
                        uint64_t nph = (ph & ~(1ull << d)) | (1ull << o);
                        State pstate = setMask(s_away_fixed, home, P, nph);
                        pstate = withTurn(pstate, P);
                        // verify: non-terminal, and forward move reproduces s
                        if (terminalAny(pstate)) continue;
                        if ((getMask(pstate, 0, 0) & getMask(pstate, 0, 1)) ||
                            (getMask(pstate, 1, 0) & getMask(pstate, 1, 1))) continue;
                        State fw = applyTurn(pstate, o, a.o2, hi);
                        if (fw != s) continue;
                        int64_t pid = rankState(pstate);
                        if (pid < 0 || npred >= 256) continue;
                        preds[npred++] = pid;
                    }
                }
                // dedup predecessor ids (same p can arise via symmetric stones)
                sort(preds, preds + npred);
                npred = (int)(unique(preds, preds + npred) - preds);
                for (int pi = 0; pi < npred; pi++) {
                    int64_t pid = preds[pi];
                    if (!USE_SYM) processPred(pid);
                    else {
                        if (nall >= 4096) abort();
                        allPreds[nall++] = pid;
                    }
                }
            }
            if (USE_SYM) {
                sort(allPreds, allPreds + nall);
                nall = (int)(unique(allPreds, allPreds + nall) - allPreds);
                for (int i = 0; i < nall; i++) processPred(allPreds[i]);
            }
            if ((int64_t)processed - lastReport >= 5000000) {
                lastReport = (int64_t)processed;
                auto t2 = chrono::steady_clock::now();
                fprintf(stderr, "retro %.1f%%  queue=%zu  W=%lld L=%lld  %.0fs\n",
                        100.0 * processed / (processed + queue.size() - qh),
                        queue.size() - qh,
                        (long long)nwin, (long long)nloss,
                        chrono::duration<double>(t2 - t1).count());
            }
            // A vector is much faster than a node-based queue, but retaining
            // every consumed id costs 8 bytes per resolved state.  Rebuild it
            // occasionally once the dead prefix dominates the live frontier.
            if (qh >= (1u << 24) && qh > queue.size() - qh) {
                vector<uint64_t> live;
                live.reserve(queue.size() - qh + (1u << 20));
                live.insert(live.end(), queue.begin() + qh, queue.end());
                queue.swap(live);
                qh = 0;
            }
        }
        auto t2 = chrono::steady_clock::now();
        fprintf(stderr, "phase2 (retro): %.1fs  W=%lld L=%lld resolved=%lld/%lld\n",
                chrono::duration<double>(t2 - t1).count(),
                (long long)nwin, (long long)nloss,
                (long long)(nwin + nloss), (long long)NSTATES);
        vector<uint64_t>().swap(queue); // phase-3 BFS can itself be very large

        // Phase 3: forward reachability from initial (bitset), stats + mark.
        vector<uint8_t> reach((NSTATES + 7) / 8, 0);
        State init = initialState();
        int64_t initId = rankState(init);
        vector<uint64_t> bfs;
        bfs.push_back((uint64_t)initId);
        reach[initId >> 3] |= 1 << (initId & 7);
        int64_t nreach = 0;
        int64_t rw = 0, rl = 0, rd = 0;
        int64_t nreachRaw = 0, rwRaw = 0, rlRaw = 0, rdRaw = 0;
        int maxdw = 0, maxdl = 0;
        size_t layerEnd = bfs.size();
        int ply = 0;
        fprintf(stderr, "reach depth %d: total=%zu\n", ply, bfs.size());
        for (size_t i = 0; i < bfs.size(); i++) {
            if (i == layerEnd) {
                layerEnd = bfs.size();
                fprintf(stderr, "reach depth %d: total=%zu\n", ++ply, bfs.size());
            }
            uint64_t id = bfs[i];
            nreach++;
            uint8_t v = val[id];
            State s = unrankState(id);
            int orbit = stateOrbitSize(s);
            nreachRaw += orbit;
            if (v == V_WIN) { rw++; rwRaw += orbit; if (depth[id] > maxdw) maxdw = depth[id]; }
            else if (v == V_LOSS) { rl++; rlRaw += orbit; if (depth[id] > maxdl) maxdl = depth[id]; }
            else { rd++; rdRaw += orbit; }
            if (terminalAny(s)) continue;
            genMoves(s, [&](State t) {
                int64_t tid = rankState(t);
                if (tid >= 0 && !((reach[tid >> 3] >> (tid & 7)) & 1)) {
                    reach[tid >> 3] |= 1 << (tid & 7);
                    bfs.push_back((uint64_t)tid);
                }
            });
        }
        auto t3 = chrono::steady_clock::now();
        fprintf(stderr, "phase3 (reach): %.1fs  reachable=%lld\n",
                chrono::duration<double>(t3 - t2).count(), (long long)nreach);

        // mark unreachable states as V_UNREACHED in output: val currently
        // 0(=unknown->draw) or WIN/LOSS. We rewrite val: unknown->DRAW,
        // unreachable->UNREACHED.
        int64_t uw = 0, ul = 0; // unreachable resolved (solved anyway)
#pragma omp parallel for schedule(static) reduction(+ : uw, ul)
        for (int64_t id = 0; id < NSTATES; id++) {
            bool r = (reach[id >> 3] >> (id & 7)) & 1;
            if (!r) {
                if (val[id] == V_WIN) uw++;
                if (val[id] == V_LOSS) ul++;
                val[id] = V_UNREACHED;
            } else if (val[id] != V_WIN && val[id] != V_LOSS) {
                val[id] = V_DRAW;
            }
        }

        // write binary table: magic, N, K, NSTATES, then val[] and depth[]
        string binfile = outprefix + ".wdl";
        FILE *f = fopen(binfile.c_str(), "wb");
        uint32_t magic = USE_SYM ? 0x53534257 : 0x53484257; // 'WBSS' / 'WBHS'
        fwrite(&magic, 4, 1, f);
        fwrite(&W, 4, 1, f);
        fwrite(&H, 4, 1, f);
        fwrite(&K, 4, 1, f);
        fwrite(&NSTATES, 8, 1, f);
        fwrite(val, 1, NSTATES, f);
        fwrite(depth, 2, NSTATES, f);
        fclose(f);

        // summary
        string sumfile = outprefix + ".summary.txt";
        FILE *g = fopen(sumfile.c_str(), "w");
        fprintf(g, "Mini-Shobu W=%d H=%d K=%d%s\n", W, H, K,
                USE_SYM ? " (symmetry quotient)" : "");
        if (USE_SYM) {
            int64_t rawStates = (int64_t)MAXB * MAXB * 2;
            fprintf(g, "mathematical state space (<=%d stones/color/board): %lld\n",
                    K, (long long)rawStates);
            fprintf(g, "state-orbit space: %lld (%.3fx reduction)\n",
                    (long long)NSTATES, (double)rawStates / NSTATES);
            fprintf(g, "reachable positions: %lld (%.1f%%), in %lld orbits\n",
                    (long long)nreachRaw, 100.0 * nreachRaw / rawStates, (long long)nreach);
            fprintf(g, "reachable WIN:  %lld positions, %lld orbits (max depth %d)\n",
                    (long long)rwRaw, (long long)rw, maxdw);
            fprintf(g, "reachable LOSS: %lld positions, %lld orbits (max depth %d)\n",
                    (long long)rlRaw, (long long)rl, maxdl);
            fprintf(g, "reachable DRAW: %lld positions, %lld orbits\n",
                    (long long)rdRaw, (long long)rd);
        } else {
            fprintf(g, "state space (product, <=%d stones/color/board): %lld\n", K,
                    (long long)NSTATES);
            fprintf(g, "reachable from initial: %lld (%.1f%%)\n",
                    (long long)nreach, 100.0 * nreach / NSTATES);
            fprintf(g, "reachable WIN:  %lld (max depth %d)\n", (long long)rw, maxdw);
            fprintf(g, "reachable LOSS: %lld (max depth %d)\n", (long long)rl, maxdl);
            fprintf(g, "reachable DRAW: %lld\n", (long long)rd);
        }
        fprintf(g, "terminals (incl. unreachable): %lld, stalemates: %lld\n",
                (long long)nterm, (long long)nstale);
        const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
        fprintf(g, "initial state id=%lld value=%s", (long long)initId, vname[val[initId]]);
        if (val[initId] == V_WIN || val[initId] == V_LOSS)
            fprintf(g, " depth=%d", depth[initId]);
        fputc('\n', g);
        fclose(g);
        fprintf(stderr, "wrote %s and %s\n", binfile.c_str(), sumfile.c_str());
        fprintf(stderr, "initial state: %s", vname[val[initId]]);
        if (val[initId] == V_WIN || val[initId] == V_LOSS)
            fprintf(stderr, " depth %d", depth[initId]);
        fputc('\n', stderr);
        return 0;
    }
    if ((mode == "query" || mode == "query-sym") && argc >= 8) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        string binfile = argv[5];
        string bstr = argv[6];
        int turn = (argv[7][0] == 'b') ? 1 : 0;
        FILE *f = fopen(binfile.c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", binfile.c_str()); return 1; }
        uint32_t magic; int fW, fH, fK; int64_t fNS;
        if (fread(&magic, 4, 1, f) != 1 || fread(&fW, 4, 1, f) != 1 ||
            fread(&fH, 4, 1, f) != 1 || fread(&fK, 4, 1, f) != 1 ||
            fread(&fNS, 8, 1, f) != 1) { fprintf(stderr, "bad file\n"); return 1; }
        uint32_t wantMagic = USE_SYM ? 0x53534257 : 0x53484257;
        if (magic != wantMagic || fW != W || fH != H || fK != K || fNS != NSTATES) {
            fprintf(stderr, "table mismatch\n"); return 1;
        }
        val = (uint8_t *)malloc(NSTATES);
        depth = (uint16_t *)malloc(NSTATES * 2);
        if (!val || !depth || fread(val, 1, NSTATES, f) != (size_t)NSTATES ||
            fread(depth, 2, NSTATES, f) != (size_t)NSTATES) {
            fprintf(stderr, "truncated table\n"); return 1;
        }
        fclose(f);
        if ((int)bstr.size() != 2 * SQ) { fprintf(stderr, "bad board string len\n"); return 1; }
        State s = 0;
        for (int b = 0; b < 2; b++)
            for (int i = 0; i < SQ; i++) {
                char c = bstr[b * SQ + i];
                if (c == 'o') s = setMask(s, b, 0, getMask(s, b, 0) | (1ull << i));
                if (c == 'x') s = setMask(s, b, 1, getMask(s, b, 1) | (1ull << i));
            }
        s = withTurn(s, turn);
        int64_t id = rankState(s);
        const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
        if (id < 0) { printf("position outside solved space\n"); return 0; }
        printf("value=%s", vname[val[id]]);
        if (val[id] == V_WIN || val[id] == V_LOSS) printf(" depth=%d", depth[id]);
        putchar('\n');
        if (terminalAny(s)) { printf("(terminal)\n"); return 0; }
        // list moves with values
        for (int psq = 0; psq < SQ; psq++)
            for (int hi = 0; hi < 16; hi++)
                for (int asq = 0; asq < SQ; asq++) {
                    State t = applyTurn(s, psq, asq, hi);
                    if (t == STATE_INVALID) continue;
                    int64_t tid = rankState(t);
                    Head h = HEADS[hi];
                    printf("  passive %c%d->%c%d h=(%+d,%+d) aggressive from %c%d -> %s",
                           'a' + psq % W, psq / W + 1,
                           'a' + (psq % W + h.dx), psq / W + h.dy + 1,
                           h.dx, h.dy,
                           'a' + asq % W, asq / W + 1,
                           tid >= 0 ? vname[val[tid]] : "?");
                    if (tid >= 0 && (val[tid] == V_WIN || val[tid] == V_LOSS))
                        printf(" d%d", depth[tid]);
                    putchar('\n');
                }
        return 0;
    }
    if ((mode == "verify" || mode == "verify-sym") && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        string binfile = argv[5];
        FILE *f = fopen(binfile.c_str(), "rb");
        uint32_t magic; int fW, fH, fK; int64_t fNS;
        if (fread(&magic, 4, 1, f) != 1 || fread(&fW, 4, 1, f) != 1 ||
            fread(&fH, 4, 1, f) != 1 || fread(&fK, 4, 1, f) != 1 ||
            fread(&fNS, 8, 1, f) != 1) { fprintf(stderr, "bad file\n"); return 1; }
        uint32_t wantMagic = USE_SYM ? 0x53534257 : 0x53484257;
        if (magic != wantMagic || fW != W || fH != H || fK != K || fNS != NSTATES) { fprintf(stderr, "mismatch\n"); return 1; }
        val = (uint8_t *)malloc(NSTATES);
        depth = (uint16_t *)malloc(NSTATES * 2);
        if (!val || !depth || fread(val, 1, NSTATES, f) != (size_t)NSTATES ||
            fread(depth, 2, NSTATES, f) != (size_t)NSTATES) {
            fprintf(stderr, "truncated table\n"); return 1;
        }
        fclose(f);
        int64_t errors = 0, checked = 0;
        int64_t errW = 0, errL = 0, errD = 0, errT = 0, errWd = 0, errLd = 0;
#pragma omp parallel for schedule(dynamic, 4096) reduction(+ : errors, checked, errW, errL, errD, errT, errWd, errLd)
        for (int64_t id = 0; id < NSTATES; id++) {
            uint8_t v = val[id];
            if (v == V_UNREACHED) continue;
            checked++;
            State s = unrankState(id);
            if (terminalAny(s)) {
                if (v != V_LOSS || depth[id] != 0) { errors++; errT++; }
                continue;
            }
            int nloss = 0, nwin = 0, ndef = 0; int maxdl = 0, mindw = 1 << 28;
            genMoves(s, [&](State t) {
                int64_t tid = rankState(t);
                uint8_t tv = val[tid];
                if (tv == V_LOSS) { nloss++; if (depth[tid] < mindw) mindw = depth[tid]; }
                else if (tv == V_WIN) { nwin++; if (depth[tid] > maxdl) maxdl = depth[tid]; }
                else ndef++;
            });
            if (v == V_WIN) {
                if (nloss == 0) { errors++; errW++;
#pragma omp critical
                    if (errW <= 3) {
                        fprintf(stderr, "WIN-no-loss-succ id=%lld depth=%d turn=%d\n",
                                (long long)id, depth[id], getTurn(s));
                        genMoves(s, [&](State t) {
                            int64_t tid = rankState(t);
                            fprintf(stderr, "    succ id=%lld val=%d depth=%d\n",
                                    (long long)tid, val[tid], depth[tid]);
                        });
                    }
                }
                else if (depth[id] != mindw + 1) { errors++; errW++; errWd++; }
            } else if (v == V_LOSS) {
                if (ndef > 0 || nloss > 0) { errors++; errL++;
#pragma omp critical
                    if (errL <= 3) {
                        fprintf(stderr, "LOSS-bad-succ id=%lld depth=%d turn=%d nwin=%d ndef=%d nloss=%d\n",
                                (long long)id, depth[id], getTurn(s), nwin, ndef, nloss);
                        genMoves(s, [&](State t) {
                            int64_t tid = rankState(t);
                            fprintf(stderr, "    succ id=%lld val=%d depth=%d\n",
                                    (long long)tid, val[tid], depth[tid]);
                        });
                    }
                }
                else if (nwin > 0 && depth[id] != maxdl + 1) { errors++; errL++; errLd++; }
            } else { // DRAW
                if (nloss > 0 || ndef == 0) { errors++; errD++; }
            }
        }
        fprintf(stderr, "verify: checked=%lld errors=%lld (W=%lld[depth-only=%lld] L=%lld[depth-only=%lld] D=%lld T=%lld)\n",
                (long long)checked, (long long)errors, (long long)errW,
                (long long)errWd, (long long)errL, (long long)errLd,
                (long long)errD, (long long)errT);
        return errors ? 1 : 0;
    }
    if ((mode == "selftest" || mode == "selftest-sym") && argc >= 5) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        // 1) rank/unrank roundtrip over all ids
        int64_t bad = 0;
#pragma omp parallel for schedule(static) reduction(+ : bad)
        for (int64_t id = 0; id < NSTATES; id++) {
            if (rankState(unrankState(id)) != id) bad++;
        }
        fprintf(stderr, "roundtrip bad=%lld / %lld\n", (long long)bad, (long long)NSTATES);
        int64_t symbad = 0;
        if (USE_SYM) {
#pragma omp parallel for schedule(static) reduction(+ : symbad)
            for (int64_t id = 0; id < NSTATES; id++) {
                State s = unrankState(id);
                if (rankState(swapPlayers(s)) != id) symbad++;
                int a = rankBoard(getMask(s, 0, 0), getMask(s, 0, 1));
                int b = rankBoard(getMask(s, 1, 0), getMask(s, 1, 1));
                for (int g = 0; g < NSYM; g++) {
                    uint64_t wm0, bm0, wm1, bm1;
                    unrankBoard(boardSym[g][a], wm0, bm0);
                    unrankBoard(boardSym[g][b], wm1, bm1);
                    State t = State(wm0) | (State(bm0) << SQ) |
                              (State(wm1) << (2 * SQ)) | (State(bm1) << (3 * SQ));
                    if (rankState(t) != id) symbad++;
                }
            }
            fprintf(stderr, "symmetry invariance bad=%lld\n", (long long)symbad);
        }
        // 2) genMoves vs applyTurn consistency on sample states
        int64_t mism = 0;
#pragma omp parallel for schedule(static) reduction(+ : mism)
        for (int64_t id = 0; id < NSTATES; id += 97) {
            State s = unrankState(id);
            if (terminalAny(s)) continue;
            vector<State> a, b;
            genMoves(s, [&](State t) { a.push_back(t); });
            for (int psq = 0; psq < SQ; psq++)
                for (int hi = 0; hi < 16; hi++)
                    for (int asq = 0; asq < SQ; asq++) {
                        State t = applyTurn(s, psq, asq, hi);
                        if (t != STATE_INVALID) b.push_back(t);
                    }
            sort(a.begin(), a.end()); sort(b.begin(), b.end());
            if (a != b) mism++;
        }
        fprintf(stderr, "genMoves vs applyTurn mismatches=%lld\n", (long long)mism);
        return (bad || symbad || mism) ? 1 : 0;
    }
    if (mode == "debugstate" && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        int64_t id = atoll(argv[5]);
        State s = unrankState(id);
        vector<State> a, b;
        genMoves(s, [&](State t) { a.push_back(t); });
        for (int psq = 0; psq < SQ; psq++)
            for (int hi = 0; hi < 16; hi++)
                for (int asq = 0; asq < SQ; asq++) {
                    State t = applyTurn(s, psq, asq, hi);
                    if (t != STATE_INVALID) b.push_back(t);
                }
        sort(a.begin(), a.end()); sort(b.begin(), b.end());
        fprintf(stderr, "genMoves=%zu applyTurn=%zu\n", a.size(), b.size());
        // specific call inspection
        if (argc >= 9) {
            int psq = atoi(argv[6]), hi = atoi(argv[7]), asq = atoi(argv[8]);
            State s0 = unrankState(id);
            fprintf(stderr, "state masks: %03llx %03llx %03llx %03llx turn %d\n",
                    (unsigned long long)getMask(s0,0,0), (unsigned long long)getMask(s0,0,1),
                    (unsigned long long)getMask(s0,1,0), (unsigned long long)getMask(s0,1,1),
                    getTurn(s0));
            State t = applyTurn(s0, psq, asq, hi);
            if (t == STATE_INVALID) fprintf(stderr, "applyTurn -> INVALID\n");
            else {
                fprintf(stderr, "applyTurn -> id=%lld masks %03llx %03llx %03llx %03llx turn %d\n",
                        (long long)rankState(t),
                        (unsigned long long)getMask(t,0,0), (unsigned long long)getMask(t,0,1),
                        (unsigned long long)getMask(t,1,0), (unsigned long long)getMask(t,1,1),
                        getTurn(t));
            }
        }
        vector<State> onlyB;
        set_difference(b.begin(), b.end(), a.begin(), a.end(), back_inserter(onlyB));
        for (State t : onlyB) {
            fprintf(stderr, "only in applyTurn: id=%lld masks %llx %llx %llx %llx turn %d\n",
                    (long long)rankState(t),
                    (unsigned long long)getMask(t,0,0), (unsigned long long)getMask(t,0,1),
                    (unsigned long long)getMask(t,1,0), (unsigned long long)getMask(t,1,1),
                    getTurn(t));
        }
        vector<State> onlyA;
        set_difference(a.begin(), a.end(), b.begin(), b.end(), back_inserter(onlyA));
        for (State t : onlyA)
            fprintf(stderr, "only in genMoves: id=%lld\n", (long long)rankState(t));
        return 0;
    }
    fprintf(stderr, "usage: solver count[-sym] W H K | count-layered-sym W H K | "
                    "solve[-sym] W H K outprefix | solve-layered-sym W H K outprefix | "
                    "solve-parallel-sym W H K outprefix | "
                    "compare-layered-sym W H K monolithic.wdl layered_prefix | "
                    "query-layered-sym W H K prefix board w|b | "
                    "verify[-sym] W H K file | query[-sym] W H K file board w|b\n");
    return 1;
}
