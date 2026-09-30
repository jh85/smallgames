// wtraj: DFA sizes along a set-based retrograde iteration of one Oware layer.
//
//   wtraj <table dir> <seeds> <n> <t> [order] [threads] [every]
//
// Mimics what a DFA based solver (Considine's W_i / L_i recursion) would have
// to hold while solving layer n for the target "the side to move forces at
// least t of the n seeds on the board".  Lower layers come from the finished
// tables (a capture always leaves the layer, so those values are final), and
// the in-layer recursion is iterated from the empty sets:
//
//   W_{i+1} = { no move, mover row >= t }
//           u { exists move: capture ? cap + gN(child) >= t : child in N_i }
//   N_{i+1} = { no move, opponent row >= t }
//           u { all moves:  capture ? gM(child) >= t      : child in W_i }
//
// (W = "mover forces >= t", N = "the other side forces >= t", the two
// quantities of the layer files).  The sets are computed explicitly as bitmaps
// and each W_i / N_i is converted to its minimal DFA to record how many states
// a compressed solver would carry at that ply.  At the fixpoint the sets are
// checked against the table's {gM >= t} and {gN >= t}.  Requires n < SEEDS/2+1
// (unclamped layers).
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../src/layer.hpp"
#include "mvdfa.hpp"

using namespace oware;
using namespace mvdfa;

static Index IX;
static Layer L[MAX_SEEDS + 1];
static std::string DIR;

static const Layer& layer(int n) {
  Layer& ly = L[n];
  if (ly.data) return ly;
  ly.describe(n, IX.layerSize(n));
  std::string path = layerPath(DIR, n);
  int fd = open(path.c_str(), O_RDONLY);
  FileHeader h;
  if (fd < 0 || read(fd, &h, sizeof h) != sizeof h || memcmp(h.magic, "OWARELH1", 8) || h.n != n ||
      !headerSeedsMatch(h) || h.size != ly.size) {
    fprintf(stderr, "cannot use %s for a %d-seed game\n", path.c_str(), SEEDS); exit(1);
  }
  ly.mode = h.mode; ly.bytes = h.bytes;
  void* p = mmap(nullptr, sizeof h + h.bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  close(fd);
  ly.data = static_cast<uint8_t*>(p) + sizeof h;
  return ly;
}

struct Bits {
  std::vector<uint64_t> w;
  uint64_t count = 0;
  explicit Bits(uint64_t n) : w((n + 63) / 64, 0) {}
  bool get(uint64_t i) const { return w[i >> 6] >> (i & 63) & 1; }
  void set(uint64_t i) { w[i >> 6] |= 1ull << (i & 63); }  // thread-owns-word discipline
  uint64_t popcount() const { uint64_t c = 0; for (uint64_t x : w) c += __builtin_popcountll(x); return c; }
};

static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  if (argc < 5) { fprintf(stderr, "usage: wtraj <dir> <seeds> <n> <t> [order] [threads] [every]\n"); return 1; }
  DIR = argv[1];
  SEEDS = atoi(argv[2]);
  int n = atoi(argv[3]), t = atoi(argv[4]);
  int order[PITS];
  for (int i = 0; i < PITS; ++i) order[i] = i;
  if (argc > 5 && strcmp(argv[5], "-")) {
    std::vector<int> o; char* s = strdup(argv[5]);
    for (char* q = strtok(s, ","); q; q = strtok(nullptr, ",")) o.push_back(atoi(q));
    if ((int)o.size() != PITS) { fprintf(stderr, "order needs %d pits\n", PITS); return 1; }
    for (int i = 0; i < PITS; ++i) order[i] = o[i];
  }
  int threads = argc > 6 ? atoi(argv[6]) : 16;
  int every = argc > 7 ? atoi(argv[7]) : 1;
  if (n >= winSeeds()) { fprintf(stderr, "layer %d is clamped (n >= %d); use a smaller layer\n", n, winSeeds()); return 1; }
  const Layer& ly = layer(n);
  // preload lower layers that captures can reach
  for (int m = 0; m < n; ++m) if (m != SEEDS - 1) layer(m);
  uint64_t size = ly.size;
  printf("layer %d: %" PRIu64 " boards, target t=%d; table {gM>=t} and {gN>=t} sizes:", n, size, t);
  uint64_t tabW = 0, tabN = 0;
  for (uint64_t i = 0; i < size; ++i) { int p = ly.get(i); tabW += ly.gM(p) >= t; tabN += ly.gN(p) >= t; }
  printf(" %" PRIu64 " %" PRIu64 "\n", tabW, tabN);
  fflush(stdout);

  Bits W(size), N(size);
  // per-board static info: legal-move-less boards are resolved once
  auto step = [&](const Bits& Wi, const Bits& Ni, Bits& Wo, Bits& No) {
    std::vector<std::thread> th;
    uint64_t words = W.w.size();
    for (int ti = 0; ti < threads; ++ti) th.emplace_back([&, ti]() {
      uint64_t w0 = words * ti / threads, w1 = words * (ti + 1) / threads;
      for (uint64_t wd = w0; wd < w1; ++wd) {
        uint64_t wbits = 0, nbits = 0;
        for (int bit = 0; bit < 64; ++bit) {
          uint64_t i = wd * 64 + bit;
          if (i >= size) break;
          Board b = IX.unrank(i, n);
          int mask = legalMask(b);
          bool win, nwin;
          if (!mask) { win = b.rowSum(0) >= t; nwin = b.rowSum(ROW) >= t; }
          else {
            win = false; nwin = true;
            for (int m = 0; m < ROW; ++m) if (mask >> m & 1) {
              Board c; int cap = play(b, m, c);
              if (cap) {
                const Layer& lc = layer(n - cap);
                int p = lc.get(IX.rank(c, n - cap));
                if (cap + lc.gN(p) >= t) win = true;
                if (lc.gM(p) < t) nwin = false;
              } else {
                uint64_t r = IX.rank(c, n);
                if (Ni.get(r)) win = true;
                if (!Wi.get(r)) nwin = false;
              }
            }
          }
          if (win) wbits |= 1ull << bit;
          if (nwin) nbits |= 1ull << bit;
        }
        Wo.w[wd] = wbits; No.w[wd] = nbits;
      }
    });
    for (auto& x : th) x.join();
  };

  auto dfaSize = [&](const Bits& S) {
    MVDFA D;
    bool ok = buildMV(IX, n, order, threads, 2000000000ull, [&](const Board&, uint64_t r) { return S.get(r) ? 0 : -1; }, D);
    if (!ok) return Stats{};
    return Stats{D.nodes(), D.edges()};
  };

  printf("%5s %14s %12s %12s %14s %12s %12s %8s\n", "ply", "|W_i|", "W nodes", "W edges", "|N_i|", "N nodes", "N edges", "sec");
  double t0 = now();
  for (int i = 0;; ++i) {
    uint64_t cw = W.popcount(), cn = N.popcount();
    if (i % every == 0) {
      Stats sw = dfaSize(W), sn = dfaSize(N);
      printf("%5d %14" PRIu64 " %12" PRIu64 " %12" PRIu64 " %14" PRIu64 " %12" PRIu64 " %12" PRIu64 " %8.1f\n",
             i, cw, sw.nodes, sw.edges, cn, sn.nodes, sn.edges, now() - t0);
    } else {
      printf("%5d %14" PRIu64 " %12s %12s %14" PRIu64 " %12s %12s %8.1f\n", i, cw, "", "", cn, "", "", now() - t0);
    }
    fflush(stdout);
    Bits W2(size), N2(size);
    step(W, N, W2, N2);
    if (W2.w == W.w && N2.w == N.w) break;
    W.w.swap(W2.w); N.w.swap(N2.w);
  }
  // check against the table
  uint64_t bad = 0;
  for (uint64_t i = 0; i < size; ++i) { int p = ly.get(i); bad += (ly.gM(p) >= t) != W.get(i); bad += (ly.gN(p) >= t) != N.get(i); }
  printf("fixpoint reached; mismatches against the table: %" PRIu64 "\n", bad);
  return bad ? 1 : 0;
}
