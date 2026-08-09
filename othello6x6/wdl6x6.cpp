// wdl6x6.cpp — exact WDL/score oracle for 6x6 Othello.
//
// Tier 1: lookup in the semi-strong solution tablebase of Takizawa
//         ("Semi-Strongly Solved", arXiv:2411.01029; Zenodo record 18843225).
// Tier 2: built-in exact alpha-beta endgame solver for positions the table does
//         not decide (outside the certified region, or bounds-only records).
//         The solver probes the tablebase during search (exact hits terminate
//         branches, bounds give cutoffs) and keeps a persistent in-RAM
//         transposition table across queries, so batch evaluation is fast.
//
// Every legal position therefore gets an exact answer.
//
// Table format (optimal_reopening_ab_table_all_{n}.txt, n = disc count 4..35):
//   13 bytes per record: 9-char base-81 position key + node_kind char + lower
//   char + upper char + '\n'; value = codebook_index - 36. Records are sorted
//   ascending by the solver's encode_bb() uint64 code of the position (NOT by
//   the ASCII key bytes). Positions are stored canonically (board_unique
//   minimum over the 8 symmetries) in mover/opponent bitboard form. node_kind
//   is an OR of {PV=1, ALL'=2, PV'=4, ALL=8, CUT=16}; kind&7 => exact record
//   (lower==upper), else auxiliary bounds. Pass-forced and terminal positions
//   are contracted out of the tables; this utility passes / scores directly.
//
// Scores are final disc differentials from the side to move with empty squares
// awarded to the winner: v>0 win, v==0 draw, v<0 loss.
//
// Build:  g++ -O2 -std=c++17 -march=native -o wdl6x6 wdl6x6.cpp
//
// Usage:
//   ./wdl6x6 [options] --pos012 <36 chars>          value of one position
//   ./wdl6x6 [options] --pos012 <36 chars> --moves  exact value of every move
//   ./wdl6x6 [options] --server                     one query per stdin line:
//       "<pos012>"        -> "<W|D|L> <lower> <upper> <kind> [pass] [terminal] [solved]"
//       "moves <pos012>"  -> "M <idx>:<score> ..."   (mover-perspective scores)
//       "ERR <msg>" on invalid input; "exit"/"quit" to stop.
// Options:
//   --data-dir DIR   table directory (default ".")
//   --wdl            decide win/draw/loss only (null-window; much faster for
//                    hard solves; bounds in output are then one-sided)
//   --tt-mb N        transposition table size in MiB (default 4096)
//   --probe-min N    probe the tablebase inside the search only at nodes with
//                    >= N empty squares (default 8)
//   --no-table       disable ALL table use, pure search (for testing)
//
// pos012: inner 6x6 board (B2..G7) row-major; '0' empty, '1' side to move,
// '2' opponent. Index i corresponds to square (row i/6, col i%6); the NN
// policy index convention matches this ordering.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <array>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

#if defined(__BMI2__)
#include <immintrin.h>
#endif

namespace wdl6x6 {

constexpr uint64_t MASK_INNER6x6 = ~0xFF818181818181FFULL;

// ---------- pext/pdep (hardware if available, portable fallback otherwise) ----------
inline uint64_t pext64(uint64_t x, uint64_t mask) {
#if defined(__BMI2__)
    return _pext_u64(x, mask);
#else
    uint64_t res = 0;
    for (uint64_t bit = 1; mask; mask &= mask - 1, bit <<= 1)
        if (x & (mask & -mask)) res |= bit;
    return res;
#endif
}
inline uint64_t pdep64(uint64_t x, uint64_t mask) {
#if defined(__BMI2__)
    return _pdep_u64(x, mask);
#else
    uint64_t res = 0;
    for (uint64_t bit = 1; mask; mask &= mask - 1, bit <<= 1)
        if (x & bit) res |= mask & -mask;
    return res;
#endif
}
inline int popcount64(uint64_t x) { return __builtin_popcountll(x); }

// ---------- board symmetries (identical to the solver) ----------
inline uint64_t transpose8x8(uint64_t b) {
    uint64_t t;
    t = (b ^ (b >> 7)) & 0x00AA00AA00AA00AAULL; b ^= t ^ (t << 7);
    t = (b ^ (b >> 14)) & 0x0000CCCC0000CCCCULL; b ^= t ^ (t << 14);
    t = (b ^ (b >> 28)) & 0x00000000F0F0F0F0ULL; b ^= t ^ (t << 28);
    return b;
}
inline uint64_t vertical_mirror8x8(uint64_t b) {
    b = ((b >> 8) & 0x00FF00FF00FF00FFULL) | ((b << 8) & 0xFF00FF00FF00FF00ULL);
    b = ((b >> 16) & 0x0000FFFF0000FFFFULL) | ((b << 16) & 0xFFFF0000FFFF0000ULL);
    b = (b >> 32) | (b << 32);
    return b;
}
inline uint64_t horizontal_mirror8x8(uint64_t b) {
    b = ((b >> 1) & 0x5555555555555555ULL) | ((b << 1) & 0xAAAAAAAAAAAAAAAAULL);
    b = ((b >> 2) & 0x3333333333333333ULL) | ((b << 2) & 0xCCCCCCCCCCCCCCCCULL);
    b = ((b >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((b << 4) & 0xF0F0F0F0F0F0F0F0ULL);
    return b;
}
inline void board_symmetry(uint64_t& p, uint64_t& o, int code) {
    if (code & 1) { p = horizontal_mirror8x8(p); o = horizontal_mirror8x8(o); }
    if (code & 2) { p = vertical_mirror8x8(p); o = vertical_mirror8x8(o); }
    if (code & 4) { p = transpose8x8(p); o = transpose8x8(o); }
}
inline void board_unique(uint64_t& p, uint64_t& o) {
    uint64_t bp = p, bo = o;
    for (int code = 1; code < 8; ++code) {
        uint64_t cp = p, co = o;
        board_symmetry(cp, co, code);
        if (cp < bp || (cp == bp && co < bo)) { bp = cp; bo = co; }
    }
    p = bp; o = bo;
}

// ---------- key/code encodings (identical to the solver) ----------
constexpr char CODEBOOK[82] = "56789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#_";

struct Tables {
    uint8_t inv[128];
    uint8_t enc5[32][32];   // 5-cell ternary chunk -> [0,242]
    uint8_t dec4[81][2];    // base-81 char index -> 4-cell (mover_bits, opp_bits)
    Tables() {
        memset(inv, 127, sizeof(inv));
        for (int i = 0; i < 81; ++i) inv[(int)(unsigned char)CODEBOOK[i]] = (uint8_t)i;
        for (int i = 0; i < 243; ++i) {
            int b1 = 0, b2 = 0;
            for (int n = i, j = 0; j < 5; ++j, n /= 3) {
                int d = n % 3;
                if (d == 1) b1 |= 1 << j; else if (d == 2) b2 |= 1 << j;
            }
            enc5[b1][b2] = (uint8_t)i;
        }
        for (int i = 0; i < 81; ++i) {
            int b1 = 0, b2 = 0;
            for (int n = i, j = 0; j < 4; ++j, n /= 3) {
                int d = n % 3;
                if (d == 1) b1 |= 1 << j; else if (d == 2) b2 |= 1 << j;
            }
            dec4[i][0] = (uint8_t)b1; dec4[i][1] = (uint8_t)b2;
        }
    }
};
inline const Tables& tables() { static const Tables t; return t; }

// uint64 sort code of a position (solver's encode_bb)
inline uint64_t encode_bb(uint64_t p, uint64_t o) {
    const Tables& T = tables();
    uint64_t bb0 = pext64(p, MASK_INNER6x6), bb1 = pext64(o, MASK_INNER6x6);
    uint64_t ans = 0;
    for (int i = 0; i < 36; i += 5) {
        ans = ans * 256 + T.enc5[bb0 & 31][bb1 & 31];
        bb0 >>= 5; bb1 >>= 5;
    }
    return ans;
}
// 9-byte ASCII record key -> uint64 sort code
inline bool key9_to_code(const unsigned char* key, uint64_t& out) {
    const Tables& T = tables();
    uint64_t bb0 = 0, bb1 = 0;
    for (int i = 8; i >= 0; --i) {
        if (key[i] >= 128) return false;
        unsigned idx = T.inv[key[i]];
        if (idx >= 81) return false;
        bb0 = bb0 * 16 + T.dec4[idx][0];
        bb1 = bb1 * 16 + T.dec4[idx][1];
    }
    uint64_t ans = 0;
    for (int i = 0; i < 36; i += 5) {
        ans = ans * 256 + T.enc5[bb0 & 31][bb1 & 31];
        bb0 >>= 5; bb1 >>= 5;
    }
    out = ans;
    return true;
}

// ---------- rules: move generation, flips, scoring ----------
inline uint64_t shift_dir(uint64_t b, int d) {
    uint64_t s = d > 0 ? (b << d) : (b >> -d);
    return s & MASK_INNER6x6; // inner mask kills wrap-around (8x8 border is a moat)
}
inline uint64_t legal_moves(uint64_t p, uint64_t o) {
    static const int DIRS[8] = { 1, -1, 8, -8, 7, -7, 9, -9 };
    const uint64_t empty = MASK_INNER6x6 & ~(p | o);
    uint64_t moves = 0;
    for (int d : DIRS) {
        uint64_t x = shift_dir(p, d) & o;
        for (int k = 0; k < 4; ++k) x |= shift_dir(x, d) & o;
        moves |= shift_dir(x, d) & empty;
    }
    return moves;
}
inline uint64_t flips(uint64_t p, uint64_t o, int sq) {
    static const int DIRS[8] = { 1, -1, 8, -8, 7, -7, 9, -9 };
    const uint64_t m = 1ULL << sq;
    uint64_t fl = 0;
    for (int d : DIRS) {
        uint64_t x = shift_dir(m, d) & o;
        for (int k = 0; k < 4; ++k) x |= shift_dir(x, d) & o;
        if (shift_dir(x, d) & p) fl |= x;
    }
    return fl;
}
// apply move; returns (new mover, new opponent) = roles swapped
inline void play(uint64_t p, uint64_t o, int sq, uint64_t& np, uint64_t& no) {
    const uint64_t fl = flips(p, o, sq);
    np = o ^ fl;
    no = p | fl | (1ULL << sq);
}

// ---------- move-ordering heuristics (ported from the reference solver) ----------
inline uint64_t get_full_lines(uint64_t line, int dir) {
    uint64_t full_l, full_r, edge_l, edge_r;
    const uint64_t edge = 0xFF818181818181FFULL;
    const int dir2 = dir << 1, dir4 = dir << 2;
    full_l = line & (edge | (line >> dir)); full_r = line & (edge | (line << dir));
    edge_l = edge | (edge >> dir);          edge_r = edge | (edge << dir);
    full_l &= edge_l | (full_l >> dir2);    full_r &= edge_r | (full_r << dir2);
    edge_l |= edge_l >> dir2;               edge_r |= edge_r << dir2;
    full_l &= edge_l | (full_l >> dir4);    full_r &= edge_r | (full_r << dir4);
    return full_r & full_l;
}
inline uint64_t get_full_lines_h(uint64_t full) {
    full &= full >> 1; full &= full >> 2; full &= full >> 4;
    return (full & 0x0101010101010101ULL) * 0xFF;
}
inline uint64_t get_full_lines_v(uint64_t full) {
    full &= (full >> 8) | (full << 56);
    full &= (full >> 16) | (full << 48);
    full &= (full >> 32) | (full << 32);
    return full;
}
// number of P's discs that can never be flipped again (edge ring counts as filled)
inline int get_stability(uint64_t P, uint64_t O) {
    const uint64_t disc = P | O | 0xFF818181818181FFULL;
    const uint64_t full_h = get_full_lines_h(disc), full_v = get_full_lines_v(disc);
    const uint64_t full_d7 = get_full_lines(disc, 7), full_d9 = get_full_lines(disc, 9);
    uint64_t stable = 0xFF818181818181FFULL | (full_h & full_v & full_d7 & full_d9 & P);
    uint64_t old_stable;
    do {
        old_stable = stable;
        const uint64_t sh = (stable >> 1) | (stable << 1) | full_h;
        const uint64_t sv = (stable >> 8) | (stable << 8) | full_v;
        const uint64_t s7 = (stable >> 7) | (stable << 7) | full_d7;
        const uint64_t s9 = (stable >> 9) | (stable << 9) | full_d9;
        stable |= sh & sv & s7 & s9 & P;
    } while (stable != old_stable);
    return popcount64(stable) - 28;
}
inline uint64_t get_potential_moves(uint64_t P, uint64_t O) {
    const uint64_t oh = O & 0x7E7E7E7E7E7E7E7EULL, ov = O & 0x00FFFFFFFFFFFF00ULL,
                   od = O & 0x007E7E7E7E7E7E00ULL;
    return ((oh << 1) | (oh >> 1) | (ov << 8) | (ov >> 8) |
            (od << 7) | (od >> 7) | (od << 9) | (od >> 9))
           & ~(P | O) & MASK_INNER6x6;
}
constexpr uint64_t CORNERS6x6 = (1ULL << 9) | (1ULL << 14) | (1ULL << 49) | (1ULL << 54);
inline int bit_weighted_count(uint64_t v) {
    return popcount64(v) + popcount64(v & CORNERS6x6);
}
inline const uint8_t SQUARE_VALUE[64] = {
    0,  0, 0,  0,  0, 0,  0, 0,
    0, 18, 4, 12, 12, 4, 18, 0,
    0,  4, 2,  8,  8, 2,  4, 0,
    0, 12, 8,  0,  0, 8, 12, 0,
    0, 12, 8,  0,  0, 8, 12, 0,
    0,  4, 2,  8,  8, 2,  4, 0,
    0, 18, 4, 12, 12, 4, 18, 0,
    0,  0, 0,  0,  0, 0,  0, 0,
};
// higher = try first; np/no = position after the move (np to move)
inline int move_score(int sq, uint64_t np, uint64_t no) {
    if (np == 0) return 1 << 30; // wipeout
    int score = SQUARE_VALUE[sq];
    score += (36 - bit_weighted_count(get_potential_moves(np, no))) * (1 << 5);
    score += get_stability(no, np) * (1 << 11);
    score += (36 - bit_weighted_count(legal_moves(np, no))) * (1 << 15);
    return score;
}
// final score from the side to move, empty squares awarded to the winner
inline int final_score(uint64_t p, uint64_t o) {
    int np = popcount64(p), no = popcount64(o);
    int s = np - no;
    if (s < 0) s -= 36 - np - no;
    else if (s > 0) s += 36 - np - no;
    return s;
}

// ---------- tablebase access (mmap + binary search by encode_bb code) ----------
struct Record { int kind; int lower; int upper; };

class Table {
public:
    explicit Table(std::string data_dir = ".") : dir_(std::move(data_dir)) {}
    ~Table() {
        for (auto& kv : files_) {
            if (kv.second.map) munmap((void*)kv.second.map, kv.second.bytes);
            if (kv.second.fd >= 0) ::close(kv.second.fd);
        }
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // p,o must be non-terminal with the mover having a legal move.
    // Canonicalizes internally. Returns false if the position is not stored.
    bool probe(uint64_t p, uint64_t o, Record& out) {
        board_unique(p, o);
        const int n_disc = popcount64(p | o);
        if (n_disc < 4 || n_disc > 35) return false;
        File* f = open_file(n_disc);
        if (!f) return false;
        const uint64_t target = encode_bb(p, o);
        uint64_t lo = 0, hi = f->n_records;
        while (lo < hi) {
            const uint64_t mid = lo + (hi - lo) / 2;
            const unsigned char* rec = f->map + mid * 13;
            uint64_t code;
            if (!key9_to_code(rec, code)) return false; // corrupt
            if (code < target) lo = mid + 1;
            else if (code > target) hi = mid;
            else {
                const Tables& T = tables();
                unsigned k = rec[9] < 128 ? T.inv[rec[9]] : 127;
                unsigned lb = rec[10] < 128 ? T.inv[rec[10]] : 127;
                unsigned ub = rec[11] < 128 ? T.inv[rec[11]] : 127;
                if (k >= 81 || lb >= 81 || ub >= 81) return false;
                out.kind = (int)k;
                out.lower = (int)lb - 36;
                out.upper = (int)ub - 36;
                return true;
            }
        }
        return false;
    }

private:
    struct File { int fd = -1; const unsigned char* map = nullptr; uint64_t bytes = 0, n_records = 0; };
    std::string dir_;
    std::map<int, File> files_;

    File* open_file(int n_disc) {
        auto it = files_.find(n_disc);
        if (it != files_.end()) return it->second.map ? &it->second : nullptr;
        File f;
        std::string path = dir_ + "/optimal_reopening_ab_table_all_" + std::to_string(n_disc) + ".txt";
        f.fd = ::open(path.c_str(), O_RDONLY);
        if (f.fd >= 0) {
            struct stat st;
            if (fstat(f.fd, &st) == 0 && st.st_size > 0 && st.st_size % 13 == 0) {
                void* m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, f.fd, 0);
                if (m != MAP_FAILED) {
                    madvise(m, (size_t)st.st_size, MADV_RANDOM);
                    f.map = (const unsigned char*)m;
                    f.bytes = (uint64_t)st.st_size;
                    f.n_records = f.bytes / 13;
                }
            }
            if (!f.map) { ::close(f.fd); f.fd = -1; }
        }
        auto res = files_.emplace(n_disc, f);
        return res.first->second.map ? &res.first->second : nullptr;
    }
};

// ---------- transposition table (persistent across queries) ----------
class TT {
public:
    void init(size_t mb) {
        size_t want = std::max<size_t>(1, mb) * 1024 * 1024 / sizeof(Entry);
        size_t p2 = 1;
        while (p2 * 2 <= want) p2 *= 2;
        entries_.assign(p2, Entry{});
        mask_ = p2 - 1;
    }
    struct Entry {
        uint64_t p = 0, o = 0;
        int8_t lo = 0, hi = 0;
        int8_t best = -1;      // square index or -1
        uint8_t empties = 0;   // 0 => slot unused
    };
    // returns entry for (p,o) or nullptr
    Entry* find(uint64_t p, uint64_t o) {
        const uint64_t h = hash(p, o) & mask_;
        Entry* a = &entries_[h & ~1ULL];
        if (a->empties && a->p == p && a->o == o) return a;
        Entry* b = a + 1;
        if (b->empties && b->p == p && b->o == o) return b;
        return nullptr;
    }
    void store(uint64_t p, uint64_t o, int empties, int lo, int hi, int best) {
        const uint64_t h = hash(p, o) & mask_;
        Entry* a = &entries_[h & ~1ULL]; // depth-preferred slot
        Entry* b = a + 1;                // always-replace slot
        Entry* e;
        if (a->empties && a->p == p && a->o == o) e = a;
        else if (b->empties && b->p == p && b->o == o) e = b;
        else if (!a->empties || (int)a->empties <= empties) e = a;
        else e = b;
        if (e->empties && e->p == p && e->o == o) { // merge bounds
            lo = std::max(lo, (int)e->lo);
            hi = std::min(hi, (int)e->hi);
            if (best < 0) best = e->best;
        }
        *e = Entry{ p, o, (int8_t)lo, (int8_t)hi, (int8_t)best, (uint8_t)empties };
    }
private:
    static uint64_t hash(uint64_t p, uint64_t o) {
        uint64_t x = p ^ (o * 0x9E3779B97F4A7C15ULL);
        x ^= x >> 32; x *= 0xD6E8FEB86659FD93ULL;
        x ^= x >> 32; x *= 0xD6E8FEB86659FD93ULL;
        return x ^ (x >> 32);
    }
    std::vector<Entry> entries_;
    uint64_t mask_ = 0;
};

// ---------- exact solver (tier 2) ----------
class Solver {
public:
    Solver(Table* table, size_t tt_mb, int probe_min_empties)
        : table_(table), probe_min_(probe_min_empties) { tt_.init(tt_mb); }

    uint64_t nodes = 0;

    // Exact fail-soft negamax with PVS, TT, and tablebase child-probing.
    // Returns v with the usual alpha-beta guarantee: result in (alpha,beta) is
    // exact; <=alpha is a valid upper bound; >=beta a valid lower bound.
    // Handles passes and terminals internally.
    int solve(uint64_t p, uint64_t o, int alpha, int beta) {
        const uint64_t mv = legal_moves(p, o);
        if (!mv) {
            if (!legal_moves(o, p)) return final_score(p, o);
            return -solve(o, p, -beta, -alpha);
        }
        const int empties = 36 - popcount64(p | o);
        ++nodes;
        if (empties <= SMALL_EMPTIES) return solve_small(p, o, alpha, beta, mv);

        const int a0 = alpha;
        int tt_best = -1;

        if (TT::Entry* e = tt_.find(p, o)) {
            if (e->lo == e->hi) return e->lo;
            if (e->lo >= beta) return e->lo;
            if (e->hi <= alpha) return e->hi;
            alpha = std::max(alpha, (int)e->lo);
            beta = std::min(beta, (int)e->hi);
            tt_best = e->best;
        }
        const bool probing = table_ && empties >= probe_min_;
        if (probing) {
            Record r;
            if (table_->probe(p, o, r)) {
                if (r.lower == r.upper) {
                    tt_.store(p, o, empties, r.lower, r.upper, -1);
                    return r.lower;
                }
                if (r.lower >= beta) return r.lower;
                if (r.upper <= alpha) return r.upper;
                alpha = std::max(alpha, r.lower);
                beta = std::min(beta, r.upper);
            }
        }

        // Generate children. Each child may be resolved without recursion via a
        // tablebase/TT probe (exact hit) or pruned by its probed bounds.
        struct Child { uint64_t p, o; int sq, key; };
        Child ch[36];
        int n = 0;
        int best = -64, best_sq = -1;
        int soft_ub = -64; // max upper bound over probe-pruned children (fail-soft)
        for (uint64_t m = mv; m; m &= m - 1) {
            const int sq = __builtin_ctzll(m);
            uint64_t np, no;
            play(p, o, sq, np, no);
            int vlo = -36, vhi = 36; // bounds on this move's value, our perspective
            if (TT::Entry* e = tt_.find(np, no)) { vlo = -e->hi; vhi = -e->lo; }
            if (probing && vlo != vhi) {
                Record r;
                bool got = false;
                const uint64_t cm = legal_moves(np, no);
                if (cm) {
                    if (table_->probe(np, no, r)) {
                        got = true;
                        vlo = std::max(vlo, -r.upper);
                        vhi = std::min(vhi, -r.lower);
                    }
                } else if (legal_moves(no, np)) { // child must pass: roles return to us
                    if (table_->probe(no, np, r)) {
                        got = true;
                        vlo = std::max(vlo, r.lower);
                        vhi = std::min(vhi, r.upper);
                    }
                } else { // child is terminal
                    got = true;
                    vlo = vhi = -final_score(np, no);
                }
                (void)got;
            }
            if (vlo >= beta) { // this move alone refutes the position
                tt_.store(p, o, empties, vlo, 36, sq);
                return vlo;
            }
            if (vlo == vhi) { // exact value known without recursion
                if (vlo > best) { best = vlo; best_sq = sq; }
                continue;
            }
            if (vhi <= std::max(alpha, best)) { // cannot influence the result
                soft_ub = std::max(soft_ub, vhi);
                continue;
            }
            Child& c = ch[n++];
            c.p = np; c.o = no; c.sq = sq;
            c.key = move_score(sq, np, no);
            if (sq == tt_best) c.key = 1 << 29;
        }
        for (int i = 1; i < n; ++i) { // insertion sort, descending key
            Child c = ch[i];
            int j = i;
            for (; j > 0 && ch[j - 1].key < c.key; --j) ch[j] = ch[j - 1];
            ch[j] = c;
        }

        // PVS: first child full window, others null-window scout + re-search
        for (int i = 0; i < n && best < beta; ++i) {
            const int a = std::max(alpha, best);
            int s;
            if (i == 0) {
                s = -solve(ch[i].p, ch[i].o, -beta, -a);
            } else {
                s = -solve(ch[i].p, ch[i].o, -(a + 1), -a);
                if (s > a && s < beta) s = -solve(ch[i].p, ch[i].o, -beta, -s);
            }
            if (s > best) { best = s; best_sq = ch[i].sq; }
        }
        if (best >= beta) tt_.store(p, o, empties, best, 36, best_sq);
        else if (best <= a0) {
            const int ub = std::max(best, soft_ub);
            tt_.store(p, o, empties, -36, ub, best_sq);
            return ub;
        }
        else tt_.store(p, o, empties, best, best, best_sq);
        return best;
    }

private:
    static constexpr int SMALL_EMPTIES = 5;
    Table* table_;
    TT tt_;
    int probe_min_;

    int solve_small(uint64_t p, uint64_t o, int alpha, int beta, uint64_t mv) {
        int best = -64;
        for (uint64_t m = mv; m; m &= m - 1) {
            const int sq = __builtin_ctzll(m);
            uint64_t np, no;
            play(p, o, sq, np, no);
            int s;
            const uint64_t cm = legal_moves(np, no);
            if (cm) s = -solve_small_inner(np, no, -beta, -std::max(alpha, best), cm);
            else if (legal_moves(no, np)) s = solve_small_pass(no, np, std::max(alpha, best), beta);
            else s = -final_score(np, no);
            if (s > best) best = s;
            if (best >= beta) break;
        }
        return best;
    }
    int solve_small_inner(uint64_t p, uint64_t o, int alpha, int beta, uint64_t mv) {
        ++nodes;
        return solve_small(p, o, alpha, beta, mv);
    }
    int solve_small_pass(uint64_t p, uint64_t o, int alpha, int beta) {
        ++nodes;
        return solve_small(p, o, alpha, beta, legal_moves(p, o));
    }
};
// ---------- oracle: table lookup with solver fallback ----------
enum class Wdl { WIN, DRAW, LOSS };

struct Result {
    enum class Status { OK, INVALID } status = Status::OK;
    Wdl wdl = Wdl::DRAW;
    bool exact = false;       // score is the exact value
    int score = 0;            // valid when exact
    int lower = -36, upper = 36; // always-valid bounds on the true value
    int node_kind = 0;        // table node kind; 0 for solved/terminal results
    bool terminal = false;
    bool passed = false;
    bool solved = false;      // tier-2 search was needed
    uint64_t nodes = 0;       // search nodes spent on this query
    std::string error;
};

class Oracle {
public:
    Oracle(std::string data_dir, bool use_table, size_t tt_mb, int probe_min)
        : table_(std::move(data_dir)),
          use_table_(use_table),
          solver_(use_table ? &table_ : nullptr, tt_mb, probe_min) {}

    // wdl_only: only decide win/draw/loss (cheaper null-window search)
    Result evaluate(uint64_t p, uint64_t o, bool wdl_only) {
        Result r;
        if ((p & o) || ((p | o) & ~MASK_INNER6x6)) {
            r.status = Result::Status::INVALID;
            r.error = "overlapping or out-of-board discs";
            return r;
        }
        bool passed = false;
        if (!legal_moves(p, o)) {
            if (!legal_moves(o, p)) {
                const int s = final_score(p, o);
                r.exact = true; r.terminal = true;
                r.score = s; r.lower = s; r.upper = s;
                r.wdl = s > 0 ? Wdl::WIN : s < 0 ? Wdl::LOSS : Wdl::DRAW;
                return r;
            }
            std::swap(p, o);
            passed = true;
        }
        r = evaluate_active(p, o, wdl_only);
        r.passed = passed;
        if (passed && r.status == Result::Status::OK) {
            r.wdl = r.wdl == Wdl::WIN ? Wdl::LOSS : r.wdl == Wdl::LOSS ? Wdl::WIN : Wdl::DRAW;
            r.score = -r.score;
            const int lo = -r.upper, hi = -r.lower;
            r.lower = lo; r.upper = hi;
        }
        return r;
    }

    // exact value (or WDL) of every legal move, mover's perspective.
    // Returns pairs (pos012 index 0..35, score). In wdl_only mode the score is
    // +1/0/-1 for a winning/drawing/losing move.
    Result evaluate_moves(uint64_t p, uint64_t o, bool wdl_only,
                          std::vector<std::pair<int, int>>& out) {
        Result r;
        out.clear();
        if ((p & o) || ((p | o) & ~MASK_INNER6x6)) {
            r.status = Result::Status::INVALID;
            r.error = "overlapping or out-of-board discs";
            return r;
        }
        if (!legal_moves(p, o)) {
            r.status = Result::Status::INVALID;
            r.error = "side to move has no legal move (pass or terminal)";
            return r;
        }
        const uint64_t nodes0 = solver_.nodes;
        for (uint64_t m = legal_moves(p, o); m; m &= m - 1) {
            const int sq = __builtin_ctzll(m);
            uint64_t np, no;
            play(p, o, sq, np, no);
            const Result cr = evaluate(np, no, wdl_only);
            int v;
            if (wdl_only)
                v = cr.wdl == Wdl::WIN ? -1 : cr.wdl == Wdl::LOSS ? 1 : 0;
            else
                v = -cr.score;
            const int idx = (sq / 8 - 1) * 6 + (sq % 8 - 1);
            out.emplace_back(idx, v);
        }
        std::sort(out.begin(), out.end());
        r.exact = !wdl_only;
        r.solved = solver_.nodes != nodes0;
        r.nodes = solver_.nodes - nodes0;
        int best = -64;
        for (auto& mv : out) best = std::max(best, mv.second);
        r.score = best; r.lower = best; r.upper = best;
        r.wdl = best > 0 ? Wdl::WIN : best < 0 ? Wdl::LOSS : Wdl::DRAW;
        return r;
    }

private:
    Table table_;
    bool use_table_;
    Solver solver_;

    // p,o: mover has at least one legal move
    Result evaluate_active(uint64_t p, uint64_t o, bool wdl_only) {
        Result r;
        Record rec{ 0, -36, 36 };
        bool found = false;
        if (use_table_) found = table_.probe(p, o, rec);
        if (found) {
            r.node_kind = rec.kind;
            r.lower = rec.lower; r.upper = rec.upper;
            if (rec.lower == rec.upper) {
                r.exact = true; r.score = rec.lower;
                r.wdl = r.score > 0 ? Wdl::WIN : r.score < 0 ? Wdl::LOSS : Wdl::DRAW;
                return r;
            }
            if (rec.lower > 0) { r.wdl = Wdl::WIN; if (wdl_only) return r; }
            if (rec.upper < 0) { r.wdl = Wdl::LOSS; if (wdl_only) return r; }
        }
        // tier 2 search
        const uint64_t nodes0 = solver_.nodes;
        r.solved = true;
        r.node_kind = found ? rec.kind : 0;
        if (wdl_only) {
            // single narrow window around zero decides W/D/L
            const int alpha = std::max(rec.lower - 1, -1), beta = std::min(rec.upper + 1, 1);
            const int s = solver_.solve(p, o, alpha, beta);
            r.nodes = solver_.nodes - nodes0;
            if (s <= alpha) { r.lower = rec.lower; r.upper = std::min(rec.upper, s); }
            else if (s >= beta) { r.lower = std::max(rec.lower, s); r.upper = rec.upper; }
            else { r.exact = true; r.score = s; r.lower = s; r.upper = s; }
            if (r.exact) r.wdl = s > 0 ? Wdl::WIN : s < 0 ? Wdl::LOSS : Wdl::DRAW;
            else if (r.lower > 0) r.wdl = Wdl::WIN;
            else if (r.upper < 0) r.wdl = Wdl::LOSS;
            else r.wdl = Wdl::DRAW; // window [-1,1] interior => exact 0 handled above
            return r;
        }
        // exact score via MTD(f): converge with null-window passes (cheap with
        // the persistent TT) instead of one expensive wide-window search.
        int lo = rec.lower, hi = rec.upper;
        int g = 0; // first guess: draw
        while (lo < hi) {
            g = std::min(std::max(g, lo), hi);
            const int b = (g == lo) ? g + 1 : g;
            const int s = solver_.solve(p, o, b - 1, b);
            if (s < b) { hi = std::min(hi, s); } else { lo = std::max(lo, s); }
            g = s;
        }
        r.nodes = solver_.nodes - nodes0;
        r.exact = true; r.score = lo; r.lower = lo; r.upper = lo;
        r.wdl = lo > 0 ? Wdl::WIN : lo < 0 ? Wdl::LOSS : Wdl::DRAW;
        return r;
    }
};

inline const char* wdl_name(Wdl w) {
    switch (w) {
        case Wdl::WIN: return "WIN";
        case Wdl::LOSS: return "LOSS";
        default: return "DRAW";
    }
}

inline bool parse_pos012(const std::string& s, uint64_t& p, uint64_t& o, std::string& err) {
    if (s.size() != 36) { err = "pos012 must be exactly 36 characters"; return false; }
    p = o = 0;
    for (int i = 0; i < 36; ++i) {
        const int sq = (i / 6 + 1) * 8 + (i % 6 + 1); // B2..G7 row-major
        if (s[i] == '1') p |= 1ULL << sq;
        else if (s[i] == '2') o |= 1ULL << sq;
        else if (s[i] != '0') { err = "pos012 may contain only '0', '1', '2'"; return false; }
    }
    return true;
}

} // namespace wdl6x6

// ---------- CLI ----------
static void print_value_line(const wdl6x6::Result& r) { // machine format
    using namespace wdl6x6;
    if (r.status == Result::Status::INVALID) { printf("ERR %s\n", r.error.c_str()); return; }
    printf("%c %d %d %d%s%s%s\n", wdl_name(r.wdl)[0], r.lower, r.upper, r.node_kind,
           r.passed ? " pass" : "", r.terminal ? " terminal" : "", r.solved ? " solved" : "");
}

static void print_value_human(const wdl6x6::Result& r, double ms) {
    using namespace wdl6x6;
    if (r.status == Result::Status::INVALID) { printf("error: %s\n", r.error.c_str()); return; }
    printf("WDL (side to move): %s\n", wdl_name(r.wdl));
    if (r.exact)
        printf("exact score: %+d  (final disc differential, empties to winner)\n", r.score);
    else
        printf("score bounds: [%d, %d]  (WDL decided; exact score not requested)\n", r.lower, r.upper);
    if (r.terminal) printf("note: terminal position, scored directly\n");
    if (r.passed) printf("note: side to move must pass; value from the passed position\n");
    if (r.solved)
        printf("source: tier-2 search (%llu nodes, %.1f ms)%s\n",
               (unsigned long long)r.nodes, ms,
               r.node_kind ? " seeded by table bounds" : "");
    else if (!r.terminal)
        printf("source: tablebase record, node_kind=%d (%s)\n", r.node_kind,
               (r.node_kind & 7) ? "certified exact / solution artifact"
                                 : "auxiliary bounds / proof certificate");
}

int main(int argc, char** argv) {
    std::string data_dir = ".", pos;
    bool server = false, wdl_only = false, moves_mode = false, no_table = false;
    long tt_mb = 4096, probe_min = 8;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data-dir" && i + 1 < argc) data_dir = argv[++i];
        else if (a == "--pos012" && i + 1 < argc) pos = argv[++i];
        else if (a == "--server") server = true;
        else if (a == "--wdl") wdl_only = true;
        else if (a == "--moves") moves_mode = true;
        else if (a == "--no-table") no_table = true;
        else if (a == "--tt-mb" && i + 1 < argc) tt_mb = atol(argv[++i]);
        else if (a == "--probe-min" && i + 1 < argc) probe_min = atol(argv[++i]);
        else {
            fprintf(stderr,
                "usage: %s [--data-dir DIR] [--wdl] [--tt-mb N] [--probe-min N] [--no-table]\n"
                "          --pos012 <36 chars> [--moves]   |   --server\n"
                "pos012: inner 6x6 row-major, 0=empty 1=side-to-move 2=opponent\n"
                "server lines: \"<pos012>\" or \"moves <pos012>\"\n", argv[0]);
            return 2;
        }
    }
    using namespace wdl6x6;
    Oracle oracle(data_dir, !no_table, (size_t)tt_mb, (int)probe_min);

    auto run_moves = [&](uint64_t p, uint64_t o, bool machine) {
        std::vector<std::pair<int, int>> mvs;
        Result r = oracle.evaluate_moves(p, o, wdl_only, mvs);
        if (r.status == Result::Status::INVALID) {
            printf(machine ? "ERR %s\n" : "error: %s\n", r.error.c_str());
            return 1;
        }
        if (machine) {
            printf("M");
            for (auto& mv : mvs) {
                if (wdl_only) printf(" %d:%c", mv.first, mv.second > 0 ? 'W' : mv.second < 0 ? 'L' : 'D');
                else printf(" %d:%d", mv.first, mv.second);
            }
            printf("\n");
        } else {
            printf("position value: %s", wdl_name(r.wdl));
            if (!wdl_only) printf(" (%+d)", r.score);
            printf("\nmoves (index = pos012 slot, square, value for the mover):\n");
            for (auto& mv : mvs) {
                const char col = (char)('b' + mv.first % 6);
                const int row = 2 + mv.first / 6;
                if (wdl_only)
                    printf("  %2d  %c%d  %c%s\n", mv.first, col, row,
                           mv.second > 0 ? 'W' : mv.second < 0 ? 'L' : 'D',
                           mv.second == r.score ? "   <- optimal" : "");
                else
                    printf("  %2d  %c%d  %+d%s\n", mv.first, col, row, mv.second,
                           mv.second == r.score ? "  <- optimal" : "");
            }
            if (r.solved)
                printf("(%llu search nodes)\n", (unsigned long long)r.nodes);
        }
        return 0;
    };

    if (server) {
        char line[512];
        while (fgets(line, sizeof(line), stdin)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty()) continue;
            if (s == "exit" || s == "quit") break;
            bool want_moves = false;
            if (s.rfind("moves ", 0) == 0) { want_moves = true; s = s.substr(6); }
            uint64_t p, o; std::string err;
            if (!parse_pos012(s, p, o, err)) { printf("ERR %s\n", err.c_str()); fflush(stdout); continue; }
            if (want_moves) run_moves(p, o, true);
            else print_value_line(oracle.evaluate(p, o, wdl_only));
            fflush(stdout);
        }
        return 0;
    }
    if (pos.empty()) {
        fprintf(stderr, "specify --pos012 or --server\n");
        return 2;
    }
    uint64_t p, o; std::string err;
    if (!parse_pos012(pos, p, o, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 2; }
    if (moves_mode) return run_moves(p, o, false);
    const auto t0 = std::chrono::steady_clock::now();
    Result r = oracle.evaluate(p, o, wdl_only);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    print_value_human(r, ms);
    return r.status == Result::Status::OK ? 0 : 1;
}
