// Symmetry-reduced Quoridor WDL solver.
//
// This is intentionally a separate executable and table format.  The legacy
// quoridor_wdl.cpp source and qwdl tables remain unchanged.
//
// The four-element symmetry group is:
//   0: identity
//   1: reflect left/right
//   2: rotate 180 degrees and swap Player1/Player2
//   3: reflect top/bottom and swap Player1/Player2
//
// Each wall-configuration orbit is represented by its numerically smallest
// transformed mask.  A lookup transforms pawn positions, side to move, the
// walls-in-hand split, and (for player-swapping symmetries) the winner.

#define main qwdl_legacy_main
#include "quoridor_wdl.cpp"
#undef main

#include <filesystem>
#include <limits>

struct Symmetry {
    const Board &b;
    std::array<std::array<int, 64>, 4> sq{};
    std::array<std::array<int, 128>, 4> slot{};

    struct Canonical {
        u128 mask;
        u8 transform;  // maps the original mask/state to mask
    };

    explicit Symmetry(const Board &board) : b(board) {
        for (int s = 0; s < 4; s++) {
            for (int q = 0; q < b.S; q++) {
                int r = q / b.W, c = q % b.W;
                int nr = r, nc = c;
                if (s == 1) nc = b.W - 1 - c;
                if (s == 2) { nr = b.H - 1 - r; nc = b.W - 1 - c; }
                if (s == 3) nr = b.H - 1 - r;
                sq[s][q] = nr * b.W + nc;
            }
            for (int e = 0; e < b.E; e++) {
                const Slot &x = b.slots[e];
                int nr = x.row, nc = x.col;
                if (s == 1) nc = b.W - 2 - x.col;
                if (s == 2) { nr = b.H - 2 - x.row; nc = b.W - 2 - x.col; }
                if (s == 3) nr = b.H - 2 - x.row;
                slot[s][e] = (nr * (b.W - 1) + nc) * 2 + x.dir;
            }
        }
    }

    static bool swapsPlayers(int s) { return s >= 2; }

    static u8 mapWinner(int s, u8 value) {
        if (!swapsPlayers(s) || value == 0) return value;
        return u8(3 - value);
    }

    u128 mapMask(int s, u128 mask) const {
        u128 out = 0;
        while (mask) {
            int e = u64(mask) ? __builtin_ctzll(u64(mask))
                              : 64 + __builtin_ctzll(u64(mask >> 64));
            out |= u128(1) << slot[s][e];
            mask &= mask - 1;
        }
        return out;
    }

    u64 mapSquares(int s, u64 mask) const {
        u64 out = 0;
        while (mask) {
            int q = __builtin_ctzll(mask);
            out |= u64(1) << sq[s][q];
            mask &= mask - 1;
        }
        return out;
    }

    Canonical canonical(u128 mask) const {
        Canonical best{mask, 0};
        for (int s = 1; s < 4; s++) {
            u128 candidate = mapMask(s, mask);
            if (candidate < best.mask) best = {candidate, u8(s)};
        }
        return best;
    }
};

// Count configurations fixed by one involutive geometry.  A fixed wall set is
// a union of slot orbits.  The identity count comes from the existing ZDD;
// non-identity transformations have at most ceil(E/2) slot orbits.
static void fixedConfigRec(const Board &b, const std::vector<u128> &orbits, size_t i,
                           u128 selected, int used, int K, std::vector<u128> &counts) {
    if (i == orbits.size()) {
        counts[used]++;
        return;
    }
    fixedConfigRec(b, orbits, i + 1, selected, used, K, counts);

    u128 orbit = orbits[i];
    int weight = __builtin_popcountll(u64(orbit)) + __builtin_popcountll(u64(orbit >> 64));
    if (used + weight > K) return;
    u128 members = orbit;
    bool legal = true;
    while (members) {
        int e = u64(members) ? __builtin_ctzll(u64(members))
                             : 64 + __builtin_ctzll(u64(members >> 64));
        u128 other = orbit & ~(u128(1) << e);
        if ((b.conflicts[e] & (selected | other)) != 0) { legal = false; break; }
        members &= members - 1;
    }
    if (legal) fixedConfigRec(b, orbits, i + 1, selected | orbit, used + weight, K, counts);
}

static std::vector<u128> fixedConfigCounts(const Board &b, const Symmetry &sym, int s, int K) {
    std::vector<u128> orbits;
    std::array<bool, 128> seen{};
    for (int e = 0; e < b.E; e++) {
        if (seen[e]) continue;
        int f = sym.slot[s][e];
        seen[e] = seen[f] = true;
        orbits.push_back((u128(1) << e) | (u128(1) << f));
    }
    std::vector<u128> counts(K + 1, 0);
    fixedConfigRec(b, orbits, 0, 0, 0, K, counts);
    return counts;
}

static std::vector<u128> symmetryOrbitCounts(const Board &b, int K) {
    Symmetry sym(b);
    ZddResult z = zddWallFamily(b, K);
    std::array<std::vector<u128>, 4> fixed;
    fixed[0] = z.countByCard;
    for (int s = 1; s < 4; s++) fixed[s] = fixedConfigCounts(b, sym, s, K);
    std::vector<u128> result(K + 1);
    for (int k = 0; k <= K; k++) {
        u128 sum = 0;
        for (int s = 0; s < 4; s++) sum += fixed[s][k];
        assert(sum % 4 == 0);
        result[k] = sum / 4;
    }
    return result;
}

static long double asLongDouble(u128 n) {
    return (long double)u64(n >> 64) * 18446744073709551616.0L + (long double)u64(n);
}

static void printSymmetryCountTable(int W, int H, int K) {
    Board b(W, H);
    Symmetry sym(b);
    ZddResult z = zddWallFamily(b, K);
    auto reps = symmetryOrbitCounts(b, K);
    auto fixedLeftRight = fixedConfigCounts(b, sym, 1, K);
    int w = K / 2;
    size_t blockStates = size_t(b.S) * b.S * 2;
    size_t blockBytes = ((blockStates * 2 + 63) / 64) * 8;
    u128 totalFull = 0, totalReps = 0, savedBytes = 0;
    u128 fullStates = 0, storedStates = 0, fixedStateCount = 0;

    printf("board %dx%d, D2+player-swap symmetry, w=%d per player\n", W, H, w);
    printf("%-4s %20s %20s %10s %22s\n", "k", "full configs", "representatives",
           "reduction", "packed value bytes");
    for (int k = 0; k <= K; k++) {
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        u128 bytes = reps[k] * u128(splits) * blockBytes;
        totalFull += z.countByCard[k];
        totalReps += reps[k];
        savedBytes += bytes;
        u128 pawnStates = u128(b.S) * (b.S - 1) * 2;
        fullStates += z.countByCard[k] * splits * pawnStates;
        storedStates += reps[k] * splits * pawnStates;
        int fixedSquares = (b.W % 2) ? b.H : 0;
        u128 fixedPawnStates = fixedSquares >= 2 ? u128(fixedSquares) * (fixedSquares - 1) * 2 : 0;
        fixedStateCount += fixedLeftRight[k] * splits * fixedPawnStates;
        printf("%-4d %20s %20s %9.3Lfx %22s\n", k, u128_str(z.countByCard[k]).c_str(),
               u128_str(reps[k]).c_str(), asLongDouble(z.countByCard[k]) / asLongDouble(reps[k]),
               u128_str(bytes).c_str());
    }

    u128 topTwo = 0;
    if (K >= 0) {
        int splits = std::min(K, w) - std::max(0, K - w) + 1;
        topTwo += reps[K] * u128(splits) * blockBytes;
    }
    if (K >= 1) {
        int k = K - 1;
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        topTwo += reps[k] * u128(splits) * blockBytes;
    }
    u128 metadata = totalReps * 32;  // u128 mask + two u64 reachability masks
    u128 recomputePeak = 0;
    u128 recomputeMetadata = 0;
    for (int k = 0; k < K; k++) recomputeMetadata += reps[k] * 32;
    for (int k = 0; k < K; k++) {
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        u128 here = reps[k] * u128(splits) * blockBytes;
        if (k + 1 < K) {
            int ns = std::min(k + 1, w) - std::max(0, k + 1 - w) + 1;
            here += reps[k + 1] * u128(ns) * blockBytes;
        }
        recomputePeak = std::max(recomputePeak, here);
    }
    printf("\ntotal configurations: %s -> %s representatives (%.3Lfx)\n",
           u128_str(totalFull).c_str(), u128_str(totalReps).c_str(),
           asLongDouble(totalFull) / asLongDouble(totalReps));
    printf("saved representative WDL bytes: %s (%.2Lf GiB)\n", u128_str(savedBytes).c_str(),
           asLongDouble(savedBytes) / (1024.0L * 1024 * 1024));
    printf("unreduced legal states: %s\n", u128_str(fullStates).c_str());
    printf("stored representative states: %s\n", u128_str(storedStates).c_str());
    printf("exact state orbits under symmetry: %s\n",
           u128_str((fullStates + fixedStateCount) / 4).c_str());
    printf("two-layer values + masks/reach lower bound: %s (%.2Lf GiB)\n",
           u128_str(topTwo + metadata).c_str(),
           asLongDouble(topTwo + metadata) / (1024.0L * 1024 * 1024));
    printf("recompute-top peak lower bound: %s (%.2Lf GiB; top layer not retained)\n",
           u128_str(recomputePeak + recomputeMetadata).c_str(),
           asLongDouble(recomputePeak + recomputeMetadata) / (1024.0L * 1024 * 1024));
}

struct SymSolver : Solver {
    Symmetry sym;

    struct Transition {
        u32 config;
        u8 transform;
        u64 reach1, reach2;  // in the current/original orientation
    };

    struct SymScratch : Solver::Scratch {
        std::vector<Transition> symTrans;
    };

    struct TopWorkspace {
        Layer generated;
        SymScratch scratch;
    };

    SymSolver(int W, int H, int wallsPerPlayer, int nthreads)
        : Solver(W, H, wallsPerPlayer, nthreads), sym(b) {}

    void solveConfigSym(Layer &lay, const Layer *next, size_t ci, SymScratch &sc) {
        const int S = b.S;
        u128 mask = lay.masks[ci];
        b.clearDirs(mask, sc.clear.data());

        for (int p = 0; p < S; p++)
            for (int o = 0; o < S; o++) {
                if (p == o) { sc.M[p * S + o] = 0; sc.pcnt[p * S + o] = 0; continue; }
                u64 m = b.pawnMoves(sc.clear.data(), p, o);
                sc.M[p * S + o] = m;
                sc.pcnt[p * S + o] = u8(__builtin_popcountll(m));
            }
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

        sc.symTrans.clear();
        if (lay.k < kmax && next) {
            for (int e = 0; e < b.E; e++) {
                if ((mask >> e) & 1) continue;
                if (mask & b.conflicts[e]) continue;
                auto can = sym.canonical(mask | (u128(1) << e));
                auto it = std::lower_bound(next->masks.begin(), next->masks.end(), can.mask);
                assert(it != next->masks.end() && *it == can.mask);
                size_t ni = size_t(it - next->masks.begin());
                assert(ni <= std::numeric_limits<u32>::max());
                bool swap = Symmetry::swapsPlayers(can.transform);
                u64 r1 = sym.mapSquares(can.transform, next->reach[swap ? 1 : 0][ni]);
                u64 r2 = sym.mapSquares(can.transform, next->reach[swap ? 0 : 1][ni]);
                sc.symTrans.push_back({u32(ni), can.transform, r1, r2});
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

            for (int p1 = 0; p1 < S; p1++)
                for (int p2 = 0; p2 < S; p2++) {
                    if (p1 == p2) continue;
                    size_t s0 = (size_t(p1) * S + p2) * 2;
                    if ((g1 >> p1) & 1) {
                        value[s0] = value[s0 + 1] = 1;
                        continue;
                    }
                    if ((g2 >> p2) & 1) {
                        value[s0] = value[s0 + 1] = 2;
                        continue;
                    }
                    u8 c0 = sc.pcnt[p1 * S + p2];
                    u8 c1 = sc.pcnt[p2 * S + p1];
                    total[s0] = c0; pending[s0] = c0;
                    total[s0 + 1] = c1; pending[s0 + 1] = c1;
                }

            if (!sc.symTrans.empty() && (l1 > 0 || l2 > 0)) {
                for (const Transition &tr : sc.symTrans) {
                    u64 r1 = tr.reach1 & ~g1 & all;
                    u64 r2 = tr.reach2 & ~g2 & all;
                    bool swap = Symmetry::swapsPlayers(tr.transform);
                    while (r1) {
                        int p1 = __builtin_ctzll(r1);
                        r1 &= r1 - 1;
                        u64 p2s = r2 & ~(u64(1) << p1);
                        while (p2s) {
                            int p2 = __builtin_ctzll(p2s);
                            p2s &= p2s - 1;
                            size_t parent0 = (size_t(p1) * S + p2) * 2;
                            for (int t = 0; t < 2; t++) {
                                if ((t == 0 && l1 == 0) || (t == 1 && l2 == 0)) continue;
                                int childUsed1 = used1 + (t == 0);
                                int canonicalUsed1 = swap ? (lay.k + 1 - childUsed1) : childUsed1;
                                int childSp = canonicalUsed1 - next->splitLo;
                                assert(childSp >= 0 && childSp < next->splitCount);
                                int cp1 = swap ? sym.sq[tr.transform][p2]
                                               : sym.sq[tr.transform][p1];
                                int cp2 = swap ? sym.sq[tr.transform][p1]
                                               : sym.sq[tr.transform][p2];
                                int childTurn = 1 - t;
                                int canonicalTurn = swap ? 1 - childTurn : childTurn;
                                size_t child = (size_t(cp1) * S + cp2) * 2 + canonicalTurn;
                                const u8 *blk = next->values.data() +
                                    (size_t(tr.config) * next->splitCount + childSp) * blockBytes;
                                u8 vc = Symmetry::mapWinner(tr.transform, getv(blk, child));
                                size_t parent = parent0 + t;
                                total[parent]++;
                                if (vc == u8(t + 1)) {
                                    value[parent] = u8(t + 1);
                                } else if (vc != u8(2 - t)) {
                                    pending[parent]++;
                                }
                            }
                        }
                    }
                }
            }

            u16 *qs = sc.queue.data();
            size_t qh = 0, qt = 0;
            for (int p1 = 0; p1 < S; p1++)
                for (int p2 = 0; p2 < S; p2++) {
                    if (p1 == p2) continue;
                    size_t s0 = (size_t(p1) * S + p2) * 2;
                    bool term = ((g1 >> p1) & 1) || ((g2 >> p2) & 1);
                    for (int t = 0; t < 2; t++) {
                        size_t state = s0 + t;
                        if (term) {
                            if (t == 0) qs[qt++] = u16(s0), qs[qt++] = u16(s0 + 1);
                            break;
                        }
                        if (value[state] != 0) {
                            qs[qt++] = u16(state);
                        } else if (total[state] > 0 && pending[state] == 0) {
                            value[state] = u8(2 - t);
                            qs[qt++] = u16(state);
                        }
                    }
                }

            while (qh < qt) {
                size_t state = qs[qh++];
                u8 v = value[state];
                int tc = int(state & 1);
                int q1 = int((state >> 1) / S), q2 = int((state >> 1) % S);
                int tp = 1 - tc;
                u64 pm = (tp == 0) ? sc.R[size_t(q1) * S + q2]
                                   : sc.R[size_t(q2) * S + q1];
                while (pm) {
                    int p = __builtin_ctzll(pm);
                    pm &= pm - 1;
                    size_t parent = (tp == 0) ? ((size_t(p) * S + q2) * 2)
                                              : ((size_t(q1) * S + p) * 2 + 1);
                    if (value[parent] != 0) continue;
                    if (v == u8(tp + 1)) {
                        value[parent] = u8(tp + 1);
                        qs[qt++] = u16(parent);
                    } else if (--pending[parent] == 0) {
                        value[parent] = u8(2 - tp);
                        qs[qt++] = u16(parent);
                    }
                }
            }

            std::fill(sc.packed.begin(), sc.packed.end(), 0);
            for (size_t state = 0; state < blockStates; state++) {
                int p1 = int((state >> 1) / S), p2 = int((state >> 1) % S);
                if (p1 == p2) continue;
                sc.cnt[value[state]]++;
                sc.packed[state >> 2] |= u8(value[state] << ((state & 3) * 2));
            }
            std::memcpy(lay.values.data() + (ci * lay.splitCount + sp) * blockBytes,
                        sc.packed.data(), blockBytes);
        }
    }

    std::vector<LayerStats> solveAllSym(bool keepAll, const std::string &saveDir, bool quiet) {
        std::vector<LayerStats> stats(kmax + 1);
        std::vector<SymScratch> scratch(threads);
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
                solveConfigSym(L, next, ci, scratch[t]);
            }
            LayerStats &st = stats[k];
            st.cnt[0] = st.cnt[1] = st.cnt[2] = st.cnt[3] = 0;
            for (auto &s : scratch)
                for (int v = 0; v < 4; v++) st.cnt[v] += s.cnt[v];
            st.states = st.cnt[0] + st.cnt[1] + st.cnt[2];
            if (!quiet)
                fprintf(stderr,
                        "sym layer k=%d representatives=%zu splits=%d stored_states=%zu "
                        "(%.1fs)\n", k, N, L.splitCount, st.states, now() - t0);
            if (!saveDir.empty()) saveLayer(saveDir, k);
            if (!keepAll && next) {
                layers[k + 1].values.clear();
                layers[k + 1].values.shrink_to_fit();
            }
        }
        return stats;
    }

    // Solve a kmax-1 configuration after generating and solving each distinct
    // kmax child locally.  This avoids retaining the enormous final layer.  The
    // generated layer is thread-local and its allocations are reused.
    void solveConfigRecomputeTop(Layer &lay, size_t ci, SymScratch &parent,
                                 TopWorkspace &workspace) {
        Layer &top = workspace.generated;
        top.k = kmax;
        top.splitLo = w;
        top.splitCount = 1;
        top.masks.clear();
        u128 mask = lay.masks[ci];
        for (int e = 0; e < b.E; e++) {
            if ((mask >> e) & 1) continue;
            if (mask & b.conflicts[e]) continue;
            top.masks.push_back(sym.canonical(mask | (u128(1) << e)).mask);
        }
        std::sort(top.masks.begin(), top.masks.end());
        top.masks.erase(std::unique(top.masks.begin(), top.masks.end()), top.masks.end());
        size_t n = top.masks.size();
        top.reach[0].resize(n);
        top.reach[1].resize(n);
        top.values.resize(n * blockBytes);
        for (size_t i = 0; i < n; i++) {
            b.clearDirs(top.masks[i], workspace.scratch.clear.data());
            top.reach[0][i] = b.reachRow(workspace.scratch.clear.data(), b.goalRow[0]);
            top.reach[1][i] = b.reachRow(workspace.scratch.clear.data(), b.goalRow[1]);
            solveConfigSym(top, nullptr, i, workspace.scratch);
        }
        solveConfigSym(lay, &top, ci, parent);
    }

    std::vector<LayerStats> solveAllSymRecomputeTop(bool quiet, bool keepAll = false,
                                                     const std::string &saveDir = "") {
        assert(kmax >= 1);
        std::vector<LayerStats> stats(kmax + 1);
        std::vector<SymScratch> scratch(threads);
        std::vector<TopWorkspace> topWork(threads);
        for (auto &s : scratch) s.init(*this);
        for (auto &wspace : topWork) wspace.scratch.init(*this);

        for (int k = kmax - 1; k >= 0; k--) {
            Layer &L = layers[k];
            const Layer *next = (k + 1 < kmax) ? &layers[k + 1] : nullptr;
            size_t N = L.masks.size();
            L.values.assign(N * L.splitCount * blockBytes, 0);
            for (auto &s : scratch) s.cnt[0] = s.cnt[1] = s.cnt[2] = s.cnt[3] = 0;
            double t0 = now();
            std::atomic<size_t> completed{0};
            size_t reportEvery = std::max<size_t>(1, N / 100);
#pragma omp parallel for schedule(dynamic, 8) num_threads(threads)
            for (size_t ci = 0; ci < N; ci++) {
#ifdef _OPENMP
                int t = omp_get_thread_num();
#else
                int t = 0;
#endif
                if (k == kmax - 1)
                    solveConfigRecomputeTop(L, ci, scratch[t], topWork[t]);
                else
                    solveConfigSym(L, next, ci, scratch[t]);
                if (!quiet && k == kmax - 1 && N >= 100) {
                    size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (done % reportEvery == 0)
                        fprintf(stderr, "sym layer k=%d progress %.0f%% (%zu/%zu, %.1fs)\n", k,
                                100.0 * double(done) / double(N), done, N, now() - t0);
                }
            }
            LayerStats &st = stats[k];
            st.cnt[0] = st.cnt[1] = st.cnt[2] = st.cnt[3] = 0;
            for (auto &s : scratch)
                for (int v = 0; v < 4; v++) st.cnt[v] += s.cnt[v];
            st.states = st.cnt[0] + st.cnt[1] + st.cnt[2];
            if (!quiet)
                fprintf(stderr,
                        "sym layer k=%d representatives=%zu splits=%d stored_states=%zu "
                        "%s(%.1fs)\n", k, N, L.splitCount, st.states,
                        k == kmax - 1 ? "[top recomputed] " : "", now() - t0);
            if (!saveDir.empty()) saveLayer(saveDir, k);
            if (next && !keepAll) {
                layers[k + 1].values.clear();
                layers[k + 1].values.shrink_to_fit();
            }
        }
        return stats;
    }

    // The maximum layer has no wall exits, so its representative blocks can be
    // solved and appended in bounded-memory chunks.  A block-aligned partial file
    // is resumable: completed blocks are retained and generation continues there.
    int streamTopLayer(const std::vector<u128> &topMasks, const std::string &saveDir,
                       size_t chunkConfigs, bool quiet) {
        namespace fs = std::filesystem;
        const std::string path = saveDir + "/layer_" + std::to_string(kmax) + ".wdl";
        u128 expected128 = u128(topMasks.size()) * blockBytes;
        if (expected128 > std::numeric_limits<uintmax_t>::max()) {
            fprintf(stderr, "top-layer file size is not representable\n");
            return 2;
        }
        uintmax_t expected = uintmax_t(expected128);
        uintmax_t existing = fs::exists(path) ? fs::file_size(path) : 0;
        if (existing > expected || existing % blockBytes != 0) {
            fprintf(stderr, "%s has invalid size %ju (expected at most %ju, blockBytes=%zu)\n",
                    path.c_str(), existing, expected, blockBytes);
            return 2;
        }
        size_t completed = size_t(existing / blockBytes);
        if (completed == topMasks.size()) {
            if (!quiet) fprintf(stderr, "top layer already complete: %s\n", path.c_str());
            return 0;
        }

        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) {
            fprintf(stderr, "cannot append %s\n", path.c_str());
            return 2;
        }
        std::vector<SymScratch> scratch(threads);
        for (auto &s : scratch) s.init(*this);
        Layer chunk;
        chunk.k = kmax;
        chunk.splitLo = w;
        chunk.splitCount = 1;
        if (chunkConfigs == 0) chunkConfigs = 1;
        double started = now();
        for (size_t begin = completed; begin < topMasks.size(); begin += chunkConfigs) {
            size_t end = std::min(topMasks.size(), begin + chunkConfigs);
            chunk.masks.assign(topMasks.begin() + begin, topMasks.begin() + end);
            chunk.values.assign((end - begin) * blockBytes, 0);
#pragma omp parallel for schedule(dynamic, 64) num_threads(threads)
            for (size_t ci = 0; ci < end - begin; ci++) {
#ifdef _OPENMP
                int t = omp_get_thread_num();
#else
                int t = 0;
#endif
                solveConfigSym(chunk, nullptr, ci, scratch[t]);
            }
            out.write(reinterpret_cast<const char *>(chunk.values.data()),
                      std::streamsize(chunk.values.size()));
            out.flush();
            if (!out) {
                fprintf(stderr, "write failed for %s at config %zu\n", path.c_str(), begin);
                return 2;
            }
            if (!quiet)
                fprintf(stderr, "sym layer k=%d streamed %zu/%zu (%.2f%%, %.1fs)\n", kmax,
                        end, topMasks.size(), 100.0 * double(end) / double(topMasks.size()),
                        now() - started);
        }
        return 0;
    }

    int lookupSym(int turn, int p1, int p2, int l1, int l2, u128 mask) const {
        int k = 0;
        for (u128 m = mask; m; m &= m - 1) k++;
        if (k > kmax) return -1;
        int used1 = w - l1, used2 = w - l2;
        if (used1 < 0 || used2 < 0 || used1 + used2 != k) return -1;
        auto can = sym.canonical(mask);
        const Layer &L = layers[k];
        auto it = std::lower_bound(L.masks.begin(), L.masks.end(), can.mask);
        if (it == L.masks.end() || *it != can.mask) return -1;
        bool swap = Symmetry::swapsPlayers(can.transform);
        int cp1 = swap ? sym.sq[can.transform][p2] : sym.sq[can.transform][p1];
        int cp2 = swap ? sym.sq[can.transform][p1] : sym.sq[can.transform][p2];
        int ct = swap ? 1 - turn : turn;
        int cused1 = swap ? used2 : used1;
        int sp = cused1 - L.splitLo;
        if (sp < 0 || sp >= L.splitCount) return -1;
        size_t ci = size_t(it - L.masks.begin());
        size_t idx = (size_t(cp1) * b.S + cp2) * 2 + ct;
        u8 v = getv(L.values.data() + (ci * L.splitCount + sp) * blockBytes, idx);
        return Symmetry::mapWinner(can.transform, v);
    }
};

static int runSymSolve(int W, int H, int w, int threads, const std::string &saveDir,
                       bool keepAll, bool quiet, bool recomputeTop = false,
                       SymSolver **out = nullptr) {
    SymSolver *sv = new SymSolver(W, H, w, threads);
    double t0 = Solver::now();
    auto expected = symmetryOrbitCounts(sv->b, sv->kmax);
    int enumerateThrough = recomputeTop ? sv->kmax - 1 : sv->kmax;
    if (enumerateThrough < 0) enumerateThrough = 0;
    auto configs = enumerateConfigs(sv->b, enumerateThrough, threads);
    configs.resize(sv->kmax + 1);
    for (int k = 0; k <= enumerateThrough; k++) {
        auto &v = configs[k];
        v.erase(std::remove_if(v.begin(), v.end(), [&](u128 mask) {
                    return sv->sym.canonical(mask).mask != mask;
                }), v.end());
        v.shrink_to_fit();
        if (u128(v.size()) != expected[k]) {
            fprintf(stderr, "FATAL: representative count mismatch at k=%d: enum=%zu expected=%s\n",
                    k, v.size(), u128_str(expected[k]).c_str());
            delete sv;
            return 2;
        }
    }
    if (!quiet)
        fprintf(stderr, "symmetry representatives validated through k=%d; %.1fs\n",
                enumerateThrough, Solver::now() - t0);

    if (recomputeTop && !saveDir.empty()) {
        fprintf(stderr, "--recompute-top currently supports outcome solving only; omit --save\n");
        delete sv;
        return 2;
    }
    if (!saveDir.empty()) {
        std::string cmd = "mkdir -p '" + saveDir + "'";
        if (system(cmd.c_str()) != 0) {
            fprintf(stderr, "cannot create save dir\n");
            delete sv;
            return 2;
        }
    }

    sv->prepareLayers(std::move(configs));
    auto stats = recomputeTop ? sv->solveAllSymRecomputeTop(quiet, keepAll)
                              : sv->solveAllSym(keepAll, saveDir, quiet);
    int value = sv->lookupSym(0, sv->b.start[0], sv->b.start[1], w, w, 0);
    const char *names[] = {"Draw", "Player1", "Player2"};
    u64 storedStates = 0;
    for (const auto &st : stats) storedStates += st.states;
    printf("width=%d height=%d walls=%d winner=%s method=retrograde_sym4 "
           "stored_states=%llu recompute_top=%s time=%.1fs\n", W, H, w, names[value],
           (unsigned long long)storedStates, recomputeTop ? "yes" : "no", Solver::now() - t0);

    if (!saveDir.empty()) {
        std::ofstream f(saveDir + "/meta.txt");
        f << "format qwdl_sym_v1\nW " << W << "\nH " << H << "\nw " << w
          << "\nkmax " << sv->kmax << "\nblockBytes " << sv->blockBytes << "\n";
        for (int k = 0; k <= sv->kmax; k++)
            f << "layer " << k << " configs " << sv->layers[k].masks.size() << " splitLo "
              << sv->layers[k].splitLo << " splitCount " << sv->layers[k].splitCount << "\n";
        f << "winner " << names[value] << "\n";
    }
    if (out) *out = sv;
    else delete sv;
    return 0;
}

static bool fileHasSize(const std::string &path, uintmax_t size) {
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists(path, ec) && !ec && fs::file_size(path, ec) == size && !ec;
}

static int writeMasksAtomic(const std::string &path, const std::vector<u128> &masks) {
    namespace fs = std::filesystem;
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return 2;
        out.write(reinterpret_cast<const char *>(masks.data()),
                  std::streamsize(masks.size() * sizeof(u128)));
        if (!out) return 2;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    return ec ? 2 : 0;
}

static int readMasks(const std::string &path, size_t count, std::vector<u128> &masks) {
    masks.resize(count);
    std::ifstream in(path, std::ios::binary);
    if (!in) return 2;
    in.read(reinterpret_cast<char *>(masks.data()),
            std::streamsize(count * sizeof(u128)));
    return in ? 0 : 2;
}

static int runPersistentSolve(int W, int H, int w, int threads, const std::string &saveDir,
                              size_t chunkConfigs, bool quiet) {
    namespace fs = std::filesystem;
    SymSolver *sv = new SymSolver(W, H, w, threads);
    double t0 = Solver::now();
    auto expected = symmetryOrbitCounts(sv->b, sv->kmax);
    ZddResult z = zddWallFamily(sv->b, sv->kmax);
    fs::create_directories(saveDir);
    if (!fs::is_directory(saveDir)) {
        fprintf(stderr, "cannot create %s\n", saveDir.c_str());
        delete sv;
        return 2;
    }

    size_t topCount = size_t(expected[sv->kmax]);
    const std::string topMaskPath = saveDir + "/layer_" + std::to_string(sv->kmax) + ".masks";
    const std::string topValuePath = saveDir + "/layer_" + std::to_string(sv->kmax) + ".wdl";
    uintmax_t topMaskBytes = uintmax_t(topCount) * sizeof(u128);
    uintmax_t topValueBytes = uintmax_t(topCount) * sv->blockBytes;
    bool haveTopMasks = fileHasSize(topMaskPath, topMaskBytes);
    bool haveTopValues = fileHasSize(topValuePath, topValueBytes);
    int enumerateThrough = haveTopMasks ? sv->kmax - 1 : sv->kmax;
    auto configs = enumerateConfigs(sv->b, std::max(0, enumerateThrough), threads);
    configs.resize(sv->kmax + 1);
    for (int k = 0; k <= enumerateThrough; k++) {
        auto &v = configs[k];
        v.erase(std::remove_if(v.begin(), v.end(), [&](u128 mask) {
                    return sv->sym.canonical(mask).mask != mask;
                }), v.end());
        v.shrink_to_fit();
        if (u128(v.size()) != expected[k]) {
            fprintf(stderr, "FATAL: representative count mismatch at k=%d: enum=%zu expected=%s\n",
                    k, v.size(), u128_str(expected[k]).c_str());
            delete sv;
            return 2;
        }
    }
    if (!quiet)
        fprintf(stderr, "persistent representatives validated through k=%d; %.1fs\n",
                enumerateThrough, Solver::now() - t0);

    if (!haveTopMasks) {
        if (writeMasksAtomic(topMaskPath, configs[sv->kmax]) != 0) {
            fprintf(stderr, "cannot write %s\n", topMaskPath.c_str());
            delete sv;
            return 2;
        }
        haveTopMasks = true;
    } else if (!haveTopValues) {
        if (readMasks(topMaskPath, topCount, configs[sv->kmax]) != 0) {
            fprintf(stderr, "cannot read %s\n", topMaskPath.c_str());
            delete sv;
            return 2;
        }
    }

    if (!haveTopValues) {
        int rc = sv->streamTopLayer(configs[sv->kmax], saveDir, chunkConfigs, quiet);
        if (rc != 0) {
            delete sv;
            return rc;
        }
    } else if (!quiet) {
        fprintf(stderr, "persistent top layer already complete\n");
    }
    configs[sv->kmax].clear();
    configs[sv->kmax].shrink_to_fit();

    sv->prepareLayers(std::move(configs));
    auto stats = sv->solveAllSymRecomputeTop(quiet, false, saveDir);
    int value = sv->lookupSym(0, sv->b.start[0], sv->b.start[1], w, w, 0);
    const char *names[] = {"Draw", "Player1", "Player2"};
    u128 pawnStates = u128(sv->b.S) * (sv->b.S - 1) * 2;
    u128 unreducedStates = 0, representativeStates = 0, fixedStateCount = 0;
    auto fixedLeftRight = fixedConfigCounts(sv->b, sv->sym, 1, sv->kmax);
    for (int k = 0; k <= sv->kmax; k++) {
        int splits = std::min(k, w) - std::max(0, k - w) + 1;
        unreducedStates += z.countByCard[k] * splits * pawnStates;
        representativeStates += expected[k] * splits * pawnStates;
        int fixedSquares = (sv->b.W % 2) ? sv->b.H : 0;
        u128 fixedPawnStates = fixedSquares >= 2 ? u128(fixedSquares) * (fixedSquares - 1) * 2 : 0;
        fixedStateCount += fixedLeftRight[k] * splits * fixedPawnStates;
    }
    u128 stateOrbits = (unreducedStates + fixedStateCount) / 4;

    const std::string metaTmp = saveDir + "/meta.txt.tmp";
    const std::string metaPath = saveDir + "/meta.txt";
    {
        std::ofstream f(metaTmp);
        f << "format qwdl_sym_v1\ncomplete 1\nW " << W << "\nH " << H << "\nw " << w
          << "\nkmax " << sv->kmax << "\nblockBytes " << sv->blockBytes << "\n";
        for (int k = 0; k <= sv->kmax; k++)
            f << "layer " << k << " configs " << u128_str(expected[k]) << " splitLo "
              << sv->splitLoOf(k) << " splitCount " << sv->splitCountOf(k) << "\n";
        f << "winner " << names[value] << "\n"
          << "unreducedStates " << u128_str(unreducedStates) << "\n"
          << "symmetryReducedStateOrbits " << u128_str(stateOrbits) << "\n"
          << "symmetryReducedStoredStates " << u128_str(representativeStates) << "\n"
          << "symmetry D2_left_right_and_player_swapping_top_bottom_rotation\n"
          << "reachability all_nonoverlapping_wall_configurations_and_all_distinct_pawn_squares\n";
        if (W == 7 && H == 7 && w == 4)
            f << "reachableFractionEstimate 0.823\n";
        else if (W == 8 && H == 8 && (w == 2 || w == 3))
            f << "reachableFractionEstimateRange 0.85 0.90\n";
        else
            f << "reachableFractionEstimate unknown\n";
        f << "stateDefinition side_to_move,pawn1,pawn2,walls_left_1,walls_left_2,wall_configuration\n";
        if (!f) {
            fprintf(stderr, "cannot write %s\n", metaTmp.c_str());
            delete sv;
            return 2;
        }
    }
    std::error_code ec;
    fs::rename(metaTmp, metaPath, ec);
    if (ec) {
        fs::remove(metaPath, ec);
        ec.clear();
        fs::rename(metaTmp, metaPath, ec);
    }
    if (ec) {
        fprintf(stderr, "cannot install %s: %s\n", metaPath.c_str(), ec.message().c_str());
        delete sv;
        return 2;
    }
    printf("width=%d height=%d walls=%d winner=%s method=retrograde_sym4_persistent "
           "unreduced_states=%s representative_states=%s time=%.1fs dir=%s\n", W, H, w,
           names[value], u128_str(unreducedStates).c_str(), u128_str(representativeStates).c_str(),
           Solver::now() - t0, saveDir.c_str());
    (void)stats;
    delete sv;
    return 0;
}

static int runSymSelftest(int W, int H, int w, int threads) {
    printf("symmetry selftest %dx%d walls=%d\n", W, H, w);
    SymSolver *sv = nullptr;
    int rc = runSymSolve(W, H, w, threads, "", true, true, false, &sv);
    if (rc != 0) return rc;
    Board b(W, H);
    auto ref = naive::solveReachable(b, w);
    size_t checked = 0, mismatches = 0;
    for (const auto &kv : ref) {
        const naive::State &s = kv.first;
        u128 mask = 0;
        for (int e : s.placed) mask |= u128(1) << e;
        int got = sv->lookupSym(s.turn, s.p[0], s.p[1], s.walls[0], s.walls[1], mask);
        if (got != kv.second) {
            if (mismatches < 10)
                printf("MISMATCH turn=%d p1=%d p2=%d l1=%d l2=%d k=%zu ref=%d sym=%d\n",
                       s.turn, s.p[0], s.p[1], s.walls[0], s.walls[1], s.placed.size(),
                       kv.second, got);
            mismatches++;
        }
        checked++;
    }
    printf("symmetry selftest %dx%d walls=%d: reachable states checked=%zu mismatches=%zu -> %s\n",
           W, H, w, checked, mismatches, mismatches ? "FAIL" : "OK");

    if (sv->kmax > 0) {
        SymSolver *recomputed = nullptr;
        int rrc = runSymSolve(W, H, w, threads, "", true, true, true, &recomputed);
        if (rrc != 0) {
            delete sv;
            return rrc;
        }
        size_t differingLayers = 0;
        for (int k = 0; k < sv->kmax; k++) {
            const Layer &a = sv->layers[k], &b = recomputed->layers[k];
            if (a.masks != b.masks || a.values != b.values) {
                printf("RECOMPUTE MISMATCH layer k=%d\n", k);
                differingLayers++;
            }
        }
        printf("recompute-top comparison: layers checked=%d differing=%zu -> %s\n", sv->kmax,
               differingLayers, differingLayers ? "FAIL" : "OK");
        mismatches += differingLayers;
        delete recomputed;
    }
    delete sv;
    return mismatches ? 1 : 0;
}

struct SymProbeCtx {
    ProbeCtx raw;

    bool load(const std::string &dir) { return raw.load(dir); }

    int value(const Board &b, const Symmetry &sym, int turn, int p1, int p2, int l1, int l2,
              u128 mask) {
        int k = 0;
        for (u128 m = mask; m; m &= m - 1) k++;
        if (k > raw.kmax) return -1;
        int used1 = raw.w - l1, used2 = raw.w - l2;
        if (used1 < 0 || used2 < 0 || used1 + used2 != k) return -1;
        auto can = sym.canonical(mask);
        const auto &masks = raw.layerMasks(k);
        auto it = std::lower_bound(masks.begin(), masks.end(), can.mask);
        if (it == masks.end() || *it != can.mask) return -1;
        bool swap = Symmetry::swapsPlayers(can.transform);
        int cp1 = swap ? sym.sq[can.transform][p2] : sym.sq[can.transform][p1];
        int cp2 = swap ? sym.sq[can.transform][p1] : sym.sq[can.transform][p2];
        int ct = swap ? 1 - turn : turn;
        int canonicalUsed1 = swap ? used2 : used1;
        int sp = canonicalUsed1 - raw.splitLo[k];
        if (sp < 0 || sp >= raw.splitCount[k]) return -1;
        size_t ci = size_t(it - masks.begin());
        if (!raw.wdl[k]) {
            raw.wdl[k] = fopen((raw.dir + "/layer_" + std::to_string(k) + ".wdl").c_str(), "rb");
            if (!raw.wdl[k]) return -1;
        }
        size_t idx = (size_t(cp1) * b.S + cp2) * 2 + ct;
        size_t off = (ci * raw.splitCount[k] + sp) * raw.blockBytes + (idx >> 2);
        u8 byte;
        if (fseeko(raw.wdl[k], off, SEEK_SET) != 0 || fread(&byte, 1, 1, raw.wdl[k]) != 1)
            return -1;
        u8 stored = (byte >> ((idx & 3) * 2)) & 3;
        return Symmetry::mapWinner(can.transform, stored);
    }
};

static int runSymProbe(const std::string &dir) {
    SymProbeCtx ctx;
    if (!ctx.load(dir)) {
        fprintf(stderr, "cannot read %s/meta.txt\n", dir.c_str());
        return 2;
    }
    Board b(ctx.raw.W, ctx.raw.H);
    Symmetry sym(b);
    const char *names[] = {"Draw", "Player1Win", "Player2Win"};
    printf("symmetry table %dx%d walls=%d  "
           "(query: t p1r p1c p2r p2c l1 l2 [Hr,c Vr,c ...])\n",
           ctx.raw.W, ctx.raw.H, ctx.raw.w);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        int t, p1r, p1c, p2r, p2c, l1, l2;
        if (!(in >> t >> p1r >> p1c >> p2r >> p2c >> l1 >> l2)) continue;
        t--;
        u128 mask = 0;
        std::string tok;
        bool ok = true;
        while (in >> tok) {
            int r, c;
            if (tok.size() >= 4 && (tok[0] == 'H' || tok[0] == 'V') &&
                sscanf(tok.c_str() + 1, "%d,%d", &r, &c) == 2 && r >= 0 &&
                r < b.H - 1 && c >= 0 && c < b.W - 1) {
                int e = (r * (b.W - 1) + c) * 2 + (tok[0] == 'V');
                mask |= u128(1) << e;
            } else {
                ok = false;
            }
        }
        if (!ok || t < 0 || t > 1 || p1r < 0 || p1r >= b.H || p1c < 0 || p1c >= b.W ||
            p2r < 0 || p2r >= b.H || p2c < 0 || p2c >= b.W) {
            printf("bad query\n");
            continue;
        }
        int p1 = p1r * b.W + p1c, p2 = p2r * b.W + p2c;
        int v = ctx.value(b, sym, t, p1, p2, l1, l2, mask);
        if (v < 0) {
            printf("state outside table\n");
            continue;
        }
        printf("value=%s\n", names[v]);
        std::vector<u8> clear(b.S);
        b.clearDirs(mask, clear.data());
        int from = t == 0 ? p1 : p2, opp = t == 0 ? p2 : p1;
        u64 moves = b.pawnMoves(clear.data(), from, opp);
        while (moves) {
            int q = __builtin_ctzll(moves);
            moves &= moves - 1;
            int c1 = t == 0 ? q : p1, c2 = t == 0 ? p2 : q;
            int cv = ctx.value(b, sym, 1 - t, c1, c2, l1, l2, mask);
            printf("  move %d,%d -> %s\n", q / b.W, q % b.W,
                   cv < 0 ? "?" : names[cv]);
        }
        int wallsLeft = t == 0 ? l1 : l2;
        if (wallsLeft > 0) {
            for (int e = 0; e < b.E; e++) {
                if ((mask >> e) & 1 || (mask & b.conflicts[e])) continue;
                u128 nextMask = mask | (u128(1) << e);
                std::vector<u8> nextClear(b.S);
                b.clearDirs(nextMask, nextClear.data());
                if (!((b.reachRow(nextClear.data(), b.goalRow[0]) >> p1) & 1)) continue;
                if (!((b.reachRow(nextClear.data(), b.goalRow[1]) >> p2) & 1)) continue;
                int nl1 = l1 - (t == 0), nl2 = l2 - (t == 1);
                int cv = ctx.value(b, sym, 1 - t, p1, p2, nl1, nl2, nextMask);
                const Slot &slot = b.slots[e];
                printf("  wall %c%d,%d -> %s\n", slot.dir ? 'V' : 'H', slot.row, slot.col,
                       cv < 0 ? "?" : names[cv]);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n  %s count W H MAXWALLSTOTAL\n"
                "  %s solve W H WALLS [--threads N] [--save DIR] [--keep] [--quiet-layers] "
                "[--recompute-top]\n"
                "  %s persistent W H WALLS DIR [--threads N] [--chunk-configs N]\n"
                "  %s probe DIR\n  %s selftest W H WALLS\n", argv[0], argv[0], argv[0],
                argv[0], argv[0]);
        return 2;
    }
    std::string mode = argv[1];
    if (mode == "probe" && argc >= 3) return runSymProbe(argv[2]);
    if (mode == "count" && argc >= 5) {
        printSymmetryCountTable(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
    if ((mode == "solve" || mode == "selftest") && argc >= 5) {
        int W = atoi(argv[2]), H = atoi(argv[3]), w = atoi(argv[4]);
        int threads = defaultThreads();
        std::string saveDir;
        bool keep = false, quiet = false, recomputeTop = false;
        for (int i = 5; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
            else if (a == "--save" && i + 1 < argc) saveDir = argv[++i];
            else if (a == "--keep") keep = true;
            else if (a == "--quiet-layers") quiet = true;
            else if (a == "--recompute-top") recomputeTop = true;
        }
        if (mode == "selftest") return runSymSelftest(W, H, w, threads);
        return runSymSolve(W, H, w, threads, saveDir, keep, quiet, recomputeTop);
    }
    if (mode == "persistent" && argc >= 6) {
        int W = atoi(argv[2]), H = atoi(argv[3]), w = atoi(argv[4]);
        std::string saveDir = argv[5];
        int threads = defaultThreads();
        size_t chunkConfigs = 1u << 20;
        bool quiet = false;
        for (int i = 6; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
            else if (a == "--chunk-configs" && i + 1 < argc)
                chunkConfigs = size_t(strtoull(argv[++i], nullptr, 10));
            else if (a == "--quiet-layers") quiet = true;
        }
        return runPersistentSolve(W, H, w, threads, saveDir, chunkConfigs, quiet);
    }
    fprintf(stderr, "bad arguments\n");
    return 2;
}
