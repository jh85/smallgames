// rulecount.cpp -- Fast static illegal-state counter used to reverse-
// engineer the exact part-2 (goal blocking) rule from the paper's Table 1
// illegal counts. Enumerates every piece configuration of the m x m game
// (both sides to move) and counts |part1 U part2_candidate|.
//
// Candidate rule: for each goal, "blocked" = tip cell empty AND all cells
// of a given set S occupied by the goal owner's opponent (optionally: some
// cells required empty / own). S is given as coordinates relative to the
// bottom-right goal corner; the top goal uses the 180-degree rotation.
//
// Usage: rulecount <m> <p> <spec>
//   spec = comma-separated cells "dx.dy" with a prefix flag:
//     o = must be opponent, e = must be empty, w = must be own
//   coordinates: (m-1-dx, m-1-dy) for the bottom goal.
//   Example R0: "o1.0,o0.1,o2.0,o0.2"  (edges opponent)
#include "cc_core.hpp"

#include <atomic>
#include <cstring>
#include <thread>

using namespace cc;

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <m> <p> <spec>\n", argv[0]);
    return 2;
  }
  int m = atoi(argv[1]), p = atoi(argv[2]);
  Game g(m, p);
  Ranker rk;

  // Parse spec into three masks for the bottom goal.
  u64 oppMaskB = 0, emptyMaskB = 0, ownMaskB = 0;
  {
    char* spec = strdup(argv[3]);
    for (char* tok = strtok(spec, ","); tok; tok = strtok(nullptr, ",")) {
      char f = tok[0];
      int dx, dy;
      if (sscanf(tok + 1, "%d.%d", &dx, &dy) != 2) {
        fprintf(stderr, "bad token %s\n", tok);
        return 2;
      }
      int id = g.idOf(m - 1 - dx, m - 1 - dy);
      if (id < 0) { fprintf(stderr, "off-board token %s\n", tok); return 2; }
      if (f == 'o') oppMaskB |= bit(id);
      else if (f == 'e') emptyMaskB |= bit(id);
      else if (f == 'w') ownMaskB |= bit(id);
      else { fprintf(stderr, "bad flag %c\n", f); return 2; }
    }
    free(spec);
  }
  u64 tipB = bit(g.N - 1), tipT = bit(0);
  u64 oppMaskT = g.rotMask(oppMaskB), emptyMaskT = g.rotMask(emptyMaskB),
      ownMaskT = g.rotMask(ownMaskB);

  u32 C1 = (u32)rk.choose(g.N, p);
  std::atomic<u64> illegal{0}, part2only{0};
  std::atomic<u32> next{0};

  auto worker = [&]() {
    u64 myIll = 0, myP2 = 0;
    while (true) {
      u32 r1 = next.fetch_add(1, std::memory_order_relaxed);
      if (r1 >= C1) break;
      u64 a = rk.unrankK(r1, p, g.N);  // P1 pieces
      int comp[49], nc = 0;
      for (int c = 0; c < g.N; c++)
        if (!(a >> c & 1)) comp[nc++] = c;
      int sel[8];
      for (int i = 0; i < p; i++) sel[i] = i;
      while (true) {
        u64 b = 0;
        for (int i = 0; i < p; i++) b |= bit(comp[sel[i]]);
        u64 occ = a | b;
        // bottom goal owned by P1, blocked by P2; top goal by P1.
        bool blockedB = !(occ & tipB) && (b & oppMaskB) == oppMaskB &&
                        !(occ & emptyMaskB) && (a & ownMaskB) == ownMaskB;
        bool blockedT = !(occ & tipT) && (a & oppMaskT) == oppMaskT &&
                        !(occ & emptyMaskT) && (b & ownMaskT) == ownMaskT;
        bool part2 = blockedB || blockedT;
        bool p1cond = (occ & g.goalMask) == g.goalMask && (a & g.goalMask);
        bool p2cond = (occ & g.startMask) == g.startMask && (b & g.startMask);
        // P1 to move: part1 = p1cond; P2 to move: part1 = p2cond.
        myIll += (part2 || p1cond) + (part2 || p2cond);
        myP2 += (part2 && !p1cond) + (part2 && !p2cond);
        int i = 0;
        while (i < p) {
          sel[i]++;
          int lim = (i + 1 < p) ? sel[i + 1] : nc;
          if (sel[i] < lim) break;
          i++;
        }
        if (i == p) break;
        for (int j = 0; j < i; j++) sel[j] = j;
      }
    }
    illegal += myIll;
    part2only += myP2;
  };
  int nt = std::thread::hardware_concurrency();
  std::vector<std::thread> ts;
  for (int i = 0; i < nt; i++) ts.emplace_back(worker);
  for (auto& t : ts) t.join();

  printf("m=%d p=%d spec=%s illegal=%llu part2_only=%llu\n", m, p, argv[3],
         (unsigned long long)illegal.load(),
         (unsigned long long)part2only.load());
  return 0;
}
