// Mini-Shobu strong solver (retrograde analysis -> WDL table).
//
// Mini-Shobu: 2-board reduced variant of Shobu, faithful to the rules of the
// Java engine in ../Shobu (validated by replaying all 104,396 dataset games):
//   - 2 boards, each N x N. Board 0 = White's home, board 1 = Black's home.
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
// Method: exhaustive retrograde analysis over the full product state space
// (all per-board configs with <=K stones per color), no BFS reachability
// needed for correctness (the space is closed under legal moves). States are
// indexed by combinatorial ranking, so no hash tables are used. ZDDs are not
// needed at this size (~1e8 states fit comfortably in RAM as flat arrays).
//
// Usage:
//   solver solve  N K outprefix     -> writes outprefix.wdl (binary table),
//                                      outprefix.summary.txt
//   solver verify N K file.wdl      -> re-checks table consistency
//   solver query  N K file.wdl "boardstring" turn
//        boardstring: 2*N*N chars, 'o'=white 'x'=black '.'=empty,
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
using namespace std;

static int W, H, K, SQ;                 // board dim, stones per color, N*N
static int64_t Cb[20][20];           // binomials
static int MAXB;                     // configs per board
static int64_t NSTATES;

// value encoding in table
enum : uint8_t { V_UNREACHED = 0, V_DRAW = 1, V_WIN = 2, V_LOSS = 3 };

struct Head { int dx, dy, ux, uy, dist; };
static Head HEADS[16];

// state layout in uint64: wm0 | bm0<<9.. generalized: 4 masks of SQ bits, then turn
// masks: index b (0/1), color c (0=white,1=black): bits [ (b*2+c)*SQ , +SQ )
static inline uint64_t getMask(uint64_t s, int board, int color) {
    return (s >> ((board * 2 + color) * SQ)) & ((1ull << SQ) - 1);
}
static inline uint64_t setMask(uint64_t s, int board, int color, uint64_t m) {
    int sh = (board * 2 + color) * SQ;
    return (s & ~(((1ull << SQ) - 1) << sh)) | (m << sh);
}
static inline int getTurn(uint64_t s) { return (int)(s >> (4 * SQ)) & 1; }
static inline uint64_t withTurn(uint64_t s, int t) {
    return (s & ~(1ull << (4 * SQ))) | ((uint64_t)t << (4 * SQ));
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
    NSTATES = (int64_t)MAXB * MAXB * 2;
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

static inline int64_t rankState(uint64_t s) {
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

static inline uint64_t unrankState(int64_t id) {
    int turn = (int)(id & 1);
    int64_t v = id >> 1;
    int r1 = (int)(v % MAXB), r0 = (int)(v / MAXB);
    uint64_t wm0, bm0, wm1, bm1;
    unrankBoard(r0, wm0, bm0);
    unrankBoard(r1, wm1, bm1);
    uint64_t s = wm0 | (bm0 << SQ) | (wm1 << (2 * SQ)) | (bm1 << (3 * SQ));
    return withTurn(s, turn);
}

// ---- rules ----
static inline bool onboard(int x, int y) { return x >= 0 && x < W && y >= 0 && y < H; }

// terminal: some board missing a color. Returns true if game over.
static inline bool terminalAny(uint64_t s) {
    for (int b = 0; b < 2; b++)
        if (getMask(s, b, 0) == 0 || getMask(s, b, 1) == 0) return true;
    return false;
}

// Apply a specific turn: passive psq on P's home board, aggressive asq on the
// other board, heading hidx. Returns new state or UINT64_MAX if illegal.
static uint64_t applyTurn(uint64_t s, int psq, int asq, int hidx) {
    int P = getTurn(s);
    int home = (P == 0) ? 0 : 1;   // white home board 0, black home board 1
    int away = 1 - home;
    uint64_t oh = getMask(s, home, P), xh = getMask(s, home, 1 - P);
    uint64_t oa = getMask(s, away, P), xa = getMask(s, away, 1 - P);
    if (!((oh >> psq) & 1) || !((oa >> asq) & 1)) return UINT64_MAX;
    Head h = HEADS[hidx];
    int px = psq % W, py = psq / W;
    int ax = asq % W, ay = asq / W;
    // passive: path empty
    int tx = px + h.dx, ty = py + h.dy;
    if (!onboard(tx, ty)) return UINT64_MAX;
    {
        int cx = px + h.ux, cy = py + h.uy;
        for (int i = 0; i < h.dist; i++, cx += h.ux, cy += h.uy) {
            int csq = cx + cy * W;
            if (((oh | xh) >> csq) & 1) return UINT64_MAX;
        }
    }
    int pd = tx + ty * W;
    // aggressive
    int bx = ax + h.dx, by = ay + h.dy;
    if (!onboard(bx, by)) return UINT64_MAX;
    int esq = -1; // pushed enemy square
    {
        int cx = ax + h.ux, cy = ay + h.uy;
        for (int i = 0; i < h.dist; i++, cx += h.ux, cy += h.uy) {
            int csq = cx + cy * W;
            if ((oa >> csq) & 1) return UINT64_MAX;       // pushes own stone
            if ((xa >> csq) & 1) {
                if (esq >= 0) return UINT64_MAX;          // 2 stones in path
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
            if (((oa | xa) >> ed) & 1) return UINT64_MAX; // would push a 2nd stone
            nxa |= 1ull << ed;
        } // else captured (off board)
    }
    uint64_t noh = (oh & ~(1ull << psq)) | (1ull << pd);
    uint64_t r = setMask(s, home, P, noh);
    r = setMask(r, away, P, noa);
    r = setMask(r, away, 1 - P, nxa);
    return withTurn(r, 1 - P);
}

// Generate all legal successors, calling cb(successor). Returns move count.
template <typename F>
static int genMoves(uint64_t s, F &&cb) {
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
                uint64_t r = setMask(s, home, P, noh);
                r = setMask(r, away, P, noa);
                r = setMask(r, away, 1 - P, nxa);
                cb(withTurn(r, 1 - P));
                cnt++;
            }
        }
    }
    return cnt;
}

static uint64_t initialState() {
    uint64_t wm = 0, bm = 0;
    for (int i = 0; i < K; i++) wm |= 1ull << i;
    for (int i = 0; i < K; i++) bm |= 1ull << (SQ - W + i);
    uint64_t s = wm | (bm << SQ) | (wm << (2 * SQ)) | (bm << (3 * SQ));
    return withTurn(s, 1); // black first
}

// globals for solve
static uint8_t *val;
static uint16_t *depth;
static uint16_t *outdeg;

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage\n"); return 1; }
    string mode = argv[1];
    if (mode == "solve" && argc >= 6) {
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
            uint64_t s = unrankState(id);
            if (terminalAny(s)) {
                // player to move has lost (a board has zero of their stones).
                val[id] = V_LOSS; // depth 0
                nterm++;
                continue;
            }
            int c = genMoves(s, [](uint64_t) {});
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
        int64_t nwin = 0, nloss = nterm + nstale;
        int64_t lastReport = 0;
        while (qh < queue.size()) {
            uint64_t sid = queue[qh++];
            uint64_t s = unrankState(sid);
            int Q = getTurn(s);         // player to move at s
            int P = 1 - Q;              // predecessor's player
            int home = (P == 0) ? 0 : 1;
            int away = 1 - home;
            uint64_t ph = getMask(s, home, P);   // P stones on home board
            uint64_t pa = getMask(s, away, P);   // P stones on away board
            uint64_t qa = getMask(s, away, Q);   // Q stones on away board
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
                    uint64_t s_away_fixed = setMask(s, away, P, npa);
                    s_away_fixed = setMask(s_away_fixed, away, Q, nqa);
                    for (int pj = 0; pj < npass; pj++) {
                        int o = passO[pj];
                        int d = (o % W + h.dx) + (o / W + h.dy) * W;
                        uint64_t nph = (ph & ~(1ull << d)) | (1ull << o);
                        uint64_t pstate = setMask(s_away_fixed, home, P, nph);
                        pstate = withTurn(pstate, P);
                        // verify: non-terminal, and forward move reproduces s
                        if (terminalAny(pstate)) continue;
                        if ((getMask(pstate, 0, 0) & getMask(pstate, 0, 1)) ||
                            (getMask(pstate, 1, 0) & getMask(pstate, 1, 1))) continue;
                        uint64_t fw = applyTurn(pstate, o, a.o2, hi);
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
                    if (val[pid] != 0) continue; // already decided
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
                }
            }
            if (qh - lastReport >= 5000000) {
                lastReport = qh;
                auto t2 = chrono::steady_clock::now();
                fprintf(stderr, "retro %.1f%%  queue=%zu  W=%lld L=%lld  %.0fs\n",
                        100.0 * qh / queue.size(), queue.size(),
                        (long long)nwin, (long long)nloss,
                        chrono::duration<double>(t2 - t1).count());
            }
        }
        auto t2 = chrono::steady_clock::now();
        fprintf(stderr, "phase2 (retro): %.1fs  W=%lld L=%lld resolved=%lld/%lld\n",
                chrono::duration<double>(t2 - t1).count(),
                (long long)nwin, (long long)nloss,
                (long long)(nwin + nloss), (long long)NSTATES);

        // Phase 3: forward reachability from initial (bitset), stats + mark.
        vector<uint8_t> reach((NSTATES + 7) / 8, 0);
        uint64_t init = initialState();
        int64_t initId = rankState(init);
        vector<uint64_t> bfs;
        bfs.push_back((uint64_t)initId);
        reach[initId >> 3] |= 1 << (initId & 7);
        int64_t nreach = 0;
        int64_t rw = 0, rl = 0, rd = 0;
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
            if (v == V_WIN) { rw++; if (depth[id] > maxdw) maxdw = depth[id]; }
            else if (v == V_LOSS) { rl++; if (depth[id] > maxdl) maxdl = depth[id]; }
            else rd++;
            uint64_t s = unrankState(id);
            if (terminalAny(s)) continue;
            genMoves(s, [&](uint64_t t) {
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
        uint32_t magic = 0x53484257; // 'WBHS'
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
        fprintf(g, "Mini-Shobu W=%d H=%d K=%d\n", W, H, K);
        fprintf(g, "state space (product, <=%d stones/color/board): %lld\n", K, (long long)NSTATES);
        fprintf(g, "reachable from initial: %lld (%.1f%%)\n",
                (long long)nreach, 100.0 * nreach / NSTATES);
        fprintf(g, "reachable WIN:  %lld (max depth %d)\n", (long long)rw, maxdw);
        fprintf(g, "reachable LOSS: %lld (max depth %d)\n", (long long)rl, maxdl);
        fprintf(g, "reachable DRAW: %lld\n", (long long)rd);
        fprintf(g, "terminals (incl. unreachable): %lld, stalemates: %lld\n",
                (long long)nterm, (long long)nstale);
        const char *vname[] = {"UNREACHED", "DRAW", "WIN", "LOSS"};
        fprintf(g, "initial state id=%lld value=%s depth=%d\n",
                (long long)initId, vname[val[initId]], depth[initId]);
        fclose(g);
        fprintf(stderr, "wrote %s and %s\n", binfile.c_str(), sumfile.c_str());
        fprintf(stderr, "initial state: %s depth %d\n", vname[val[initId]], depth[initId]);
        return 0;
    }
    if (mode == "query" && argc >= 7) {
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
        if (magic != 0x53484257 || fW != W || fH != H || fK != K) {
            fprintf(stderr, "table mismatch\n"); return 1;
        }
        val = (uint8_t *)malloc(NSTATES);
        depth = (uint16_t *)malloc(NSTATES * 2);
        fread(val, 1, NSTATES, f);
        fread(depth, 2, NSTATES, f);
        fclose(f);
        if ((int)bstr.size() != 2 * SQ) { fprintf(stderr, "bad board string len\n"); return 1; }
        uint64_t s = 0;
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
        printf("value=%s depth=%d\n", vname[val[id]], depth[id]);
        if (terminalAny(s)) { printf("(terminal)\n"); return 0; }
        // list moves with values
        for (int psq = 0; psq < SQ; psq++)
            for (int hi = 0; hi < 16; hi++)
                for (int asq = 0; asq < SQ; asq++) {
                    uint64_t t = applyTurn(s, psq, asq, hi);
                    if (t == UINT64_MAX) continue;
                    int64_t tid = rankState(t);
                    Head h = HEADS[hi];
                    printf("  passive %c%d->%c%d h=(%+d,%+d) aggressive from %c%d -> %s d%d\n",
                           'a' + psq % W, psq / W + 1,
                           'a' + (psq % W + h.dx), psq / W + h.dy + 1,
                           h.dx, h.dy,
                           'a' + asq % W, asq / W + 1,
                           tid >= 0 ? vname[val[tid]] : "?", tid >= 0 ? depth[tid] : 0);
                }
        return 0;
    }
    if (mode == "verify" && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        string binfile = argv[5];
        FILE *f = fopen(binfile.c_str(), "rb");
        uint32_t magic; int fW, fH, fK; int64_t fNS;
        if (fread(&magic, 4, 1, f) != 1 || fread(&fW, 4, 1, f) != 1 ||
            fread(&fH, 4, 1, f) != 1 || fread(&fK, 4, 1, f) != 1 ||
            fread(&fNS, 8, 1, f) != 1) { fprintf(stderr, "bad file\n"); return 1; }
        if (magic != 0x53484257 || fW != W || fH != H || fK != K) { fprintf(stderr, "mismatch\n"); return 1; }
        val = (uint8_t *)malloc(NSTATES);
        depth = (uint16_t *)malloc(NSTATES * 2);
        fread(val, 1, NSTATES, f);
        fread(depth, 2, NSTATES, f);
        fclose(f);
        int64_t errors = 0, checked = 0;
        int64_t errW = 0, errL = 0, errD = 0, errT = 0, errWd = 0, errLd = 0;
#pragma omp parallel for schedule(dynamic, 4096) reduction(+ : errors, checked, errW, errL, errD, errT, errWd, errLd)
        for (int64_t id = 0; id < NSTATES; id++) {
            uint8_t v = val[id];
            if (v == V_UNREACHED) continue;
            checked++;
            uint64_t s = unrankState(id);
            if (terminalAny(s)) {
                if (v != V_LOSS || depth[id] != 0) { errors++; errT++; }
                continue;
            }
            int nloss = 0, nwin = 0, ndef = 0; int maxdl = 0, mindw = 1 << 28;
            genMoves(s, [&](uint64_t t) {
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
                        genMoves(s, [&](uint64_t t) {
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
                        genMoves(s, [&](uint64_t t) {
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
    if (mode == "selftest" && argc >= 5) {
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
        // 2) genMoves vs applyTurn consistency on sample states
        int64_t mism = 0;
#pragma omp parallel for schedule(static) reduction(+ : mism)
        for (int64_t id = 0; id < NSTATES; id += 97) {
            uint64_t s = unrankState(id);
            if (terminalAny(s)) continue;
            vector<uint64_t> a, b;
            genMoves(s, [&](uint64_t t) { a.push_back(t); });
            for (int psq = 0; psq < SQ; psq++)
                for (int hi = 0; hi < 16; hi++)
                    for (int asq = 0; asq < SQ; asq++) {
                        uint64_t t = applyTurn(s, psq, asq, hi);
                        if (t != UINT64_MAX) b.push_back(t);
                    }
            sort(a.begin(), a.end()); sort(b.begin(), b.end());
            if (a != b) mism++;
        }
        fprintf(stderr, "genMoves vs applyTurn mismatches=%lld\n", (long long)mism);
        return 0;
    }
    if (mode == "debugstate" && argc >= 6) {
        W = atoi(argv[2]); H = atoi(argv[3]); K = atoi(argv[4]);
        SQ = W * H;
        initTables();
        int64_t id = atoll(argv[5]);
        uint64_t s = unrankState(id);
        vector<uint64_t> a, b;
        genMoves(s, [&](uint64_t t) { a.push_back(t); });
        for (int psq = 0; psq < SQ; psq++)
            for (int hi = 0; hi < 16; hi++)
                for (int asq = 0; asq < SQ; asq++) {
                    uint64_t t = applyTurn(s, psq, asq, hi);
                    if (t != UINT64_MAX) b.push_back(t);
                }
        sort(a.begin(), a.end()); sort(b.begin(), b.end());
        fprintf(stderr, "genMoves=%zu applyTurn=%zu\n", a.size(), b.size());
        // specific call inspection
        if (argc >= 9) {
            int psq = atoi(argv[6]), hi = atoi(argv[7]), asq = atoi(argv[8]);
            uint64_t s0 = unrankState(id);
            fprintf(stderr, "state masks: %03llx %03llx %03llx %03llx turn %d\n",
                    (unsigned long long)getMask(s0,0,0), (unsigned long long)getMask(s0,0,1),
                    (unsigned long long)getMask(s0,1,0), (unsigned long long)getMask(s0,1,1),
                    getTurn(s0));
            uint64_t t = applyTurn(s0, psq, asq, hi);
            if (t == UINT64_MAX) fprintf(stderr, "applyTurn -> INVALID\n");
            else {
                fprintf(stderr, "applyTurn -> id=%lld masks %03llx %03llx %03llx %03llx turn %d\n",
                        (long long)rankState(t),
                        (unsigned long long)getMask(t,0,0), (unsigned long long)getMask(t,0,1),
                        (unsigned long long)getMask(t,1,0), (unsigned long long)getMask(t,1,1),
                        getTurn(t));
            }
        }
        vector<uint64_t> onlyB;
        set_difference(b.begin(), b.end(), a.begin(), a.end(), back_inserter(onlyB));
        for (uint64_t t : onlyB) {
            fprintf(stderr, "only in applyTurn: id=%lld masks %llx %llx %llx %llx turn %d\n",
                    (long long)rankState(t),
                    (unsigned long long)getMask(t,0,0), (unsigned long long)getMask(t,0,1),
                    (unsigned long long)getMask(t,1,0), (unsigned long long)getMask(t,1,1),
                    getTurn(t));
        }
        vector<uint64_t> onlyA;
        set_difference(a.begin(), a.end(), b.begin(), b.end(), back_inserter(onlyA));
        for (uint64_t t : onlyA)
            fprintf(stderr, "only in genMoves: id=%lld\n", (long long)rankState(t));
        return 0;
    }
    fprintf(stderr, "usage: solver solve W H K outprefix | verify W H K file | query W H K file board w|b\n");
    return 1;
}
