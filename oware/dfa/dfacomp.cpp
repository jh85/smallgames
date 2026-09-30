// dfacomp: measure how well a finished Oware layer compresses as a minimal
// layered DFA (the representation of "Compressed Game Solving", Considine 2024).
//
//   dfacomp <table dir> <seeds> <n> [order] [threads] [max_nodes]
//
// Every indexed board of layer n is enumerated in the pit order given by
// <order> (a comma separated permutation of 0..PITS-1 in mover-normalised
// numbering: 0..ROW-1 mover, ROW..PITS-1 opponent; default 0,1,...,PITS-1),
// its (gM, gN) pair code is read from the table, and the decision tree over
// the pits is hash-consed bottom-up into the minimal multi-valued layered DFA
// (one terminal per pair code).  From that automaton the minimal binary DFA
// of every threshold set  {gM >= t}  and  {gN >= t}  is derived by relabelling
// the terminals and re-minimising, which is what a DFA based solver would have
// to store for the layer.  Nodes and non-rejecting transitions are reported
// per layer and per set, with byte estimates for a dense (framework style,
// 4 bytes per character per node) and a sparse (4 bytes per edge + 8 per
// node) encoding, next to the size of the existing table.
//
// Parallel over the first two pits of the order; building aborts once the
// node count exceeds max_nodes (default 3e8), which already says "no
// compression".
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/layer.hpp"
#include "mvdfa.hpp"

using namespace oware;
using namespace mvdfa;

static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "usage: dfacomp <dir> <seeds> <n> [order] [threads] [max_nodes]\n"); return 1; }
  std::string dir = argv[1];
  SEEDS = atoi(argv[2]);
  int n = atoi(argv[3]);
  int order[PITS];
  for (int i = 0; i < PITS; ++i) order[i] = i;
  if (argc > 4 && strcmp(argv[4], "-")) {
    std::vector<int> o; char* s = strdup(argv[4]);
    for (char* t = strtok(s, ","); t; t = strtok(nullptr, ",")) o.push_back(atoi(t));
    if ((int)o.size() != PITS) { fprintf(stderr, "order needs %d pits\n", PITS); return 1; }
    for (int i = 0; i < PITS; ++i) order[i] = o[i];
  }
  int threads = argc > 5 ? atoi(argv[5]) : 16;
  uint64_t maxNodes = argc > 6 ? strtoull(argv[6], nullptr, 10) : 300000000ull;

  Index IX;
  Layer ly;
  ly.describe(n, IX.layerSize(n));
  std::string path = layerPath(dir, n);
  int fd = open(path.c_str(), O_RDONLY);
  FileHeader h;
  if (fd < 0 || read(fd, &h, sizeof h) != sizeof h || memcmp(h.magic, "OWARELH1", 8) || h.n != n ||
      !headerSeedsMatch(h) || h.size != ly.size) {
    fprintf(stderr, "cannot use %s for a %d-seed game\n", path.c_str(), SEEDS); return 1;
  }
  ly.mode = h.mode; ly.bytes = h.bytes;
  void* p = mmap(nullptr, sizeof h + h.bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { perror("mmap"); return 1; }
  ly.data = static_cast<uint8_t*>(p) + sizeof h;

  printf("layer %d: %" PRIu64 " boards, V=%d P=%d, table %" PRIu64 " bytes (%.3f bytes/board)\n",
         n, ly.size, ly.V, ly.P, ly.bytes, double(ly.bytes) / double(ly.size));
  printf("order:"); for (int i = 0; i < PITS; ++i) printf(" %d", order[i]); printf("\n");
  fflush(stdout);

  double t0 = now();
  MVDFA D;
  std::atomic<uint64_t> boards{0};
  bool ok = buildMV(IX, n, order, threads, maxNodes,
                    [&](const Board&, uint64_t r) { boards.fetch_add(1, std::memory_order_relaxed); return ly.get(r); }, D);
  if (!ok) {
    printf("ABORTED: more than %" PRIu64 " nodes after %" PRIu64 " boards (%.1f%% of layer) -> no useful compression\n",
           maxNodes, boards.load(), 100.0 * double(boards.load()) / double(ly.size));
    return 2;
  }
  double t1 = now();
  if (boards.load() != ly.size) { fprintf(stderr, "enumerated %" PRIu64 " boards, layer has %" PRIu64 "\n", boards.load(), ly.size); return 1; }

  {  // verify a sample
    std::mt19937_64 rng(12345);
    for (int s = 0; s < 100000; ++s) {
      uint64_t idx = rng() % ly.size;
      uint32_t st = D.lookup(IX.unrank(idx, n));
      if (st < TERM_BASE || st - TERM_BASE != uint32_t(ly.get(idx))) { fprintf(stderr, "VERIFY FAILED at %" PRIu64 "\n", idx); return 1; }
    }
  }

  uint64_t N = D.nodes(), E = D.edges();
  printf("multi-valued DFA (whole table, %d terminals), built in %.1fs:\n", ly.P, t1 - t0);
  for (int pos = 0; pos < PITS; ++pos)
    printf("  pos %2d (pit %2d): %12zu nodes %14" PRIu64 " edges\n", pos, order[pos], D.G[pos].size(), D.G[pos].edges());
  printf("  total %" PRIu64 " nodes, %" PRIu64 " edges; boards/node %.1f; dense %.3g B, sparse %.3g B (table %.3g B)\n",
         N, E, double(ly.size) / double(N), double(N) * (SEEDS + 1) * 4, double(E) * 4 + double(N) * 8, double(ly.bytes));

  printf("threshold sets (minimal binary DFAs; A=%d, codes 0..%d):\n", ly.A, ly.V - 1);
  uint64_t sumN = 0, sumE = 0;
  for (int which = 0; which < 2; ++which)
    for (int t = 1; t < ly.V; ++t) {
      std::vector<char> acc(ly.P);
      for (int q = 0; q < ly.P; ++q) acc[q] = (which == 0 ? ly.decM[q] : ly.decN[q]) >= t;
      Stats s = threshold(D, acc);
      printf("  {%s >= %2d}: %12" PRIu64 " nodes %14" PRIu64 " edges\n", which == 0 ? "gM" : "gN", ly.A + t, s.nodes, s.edges);
      sumN += s.nodes; sumE += s.edges;
    }
  printf("  all %d sets: %" PRIu64 " nodes, %" PRIu64 " edges; dense %.3g B, sparse %.3g B\n",
         2 * (ly.V - 1), sumN, sumE, double(sumN) * (SEEDS + 1) * 4, double(sumE) * 4 + double(sumN) * 8);
  {
    std::vector<char> acc(ly.P, 1);
    Stats s = threshold(D, acc);
    printf("  indexed-board set alone: %" PRIu64 " nodes %" PRIu64 " edges\n", s.nodes, s.edges);
  }
  printf("done in %.1fs\n", now() - t0);
  return 0;
}
