// crosscheck.cpp -- Compare every full-space state value from the
// independent brute-force solver's dump against the optimized solver's
// canonical 2-bit table. Validates move generation, rules, ranking, both
// symmetry maps, and the retrograde solve, state by state.
//
// Usage: crosscheck <m> <p> <bruteforce.val> <solver_run_dir> [--areas-sym]
#include "cc_core.hpp"

using namespace cc;

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s <m> <p> <bf.val> <run_dir> [--areas-sym]\n",
            argv[0]);
    return 2;
  }
  int m = atoi(argv[1]), p = atoi(argv[2]);
  Game::AreaMode am = Game::FIRSTK;
  for (int i = 5; i < argc; i++)
    if (std::string(argv[i]) == "--areas-sym") am = Game::SYMMETRIC;
  Game g(m, p, am);
  Index ix(g, true);
  Table table;
  table.alloc(ix.totalStored);
  if (!table.loadFrom(std::string(argv[4]) + "/table.bin")) {
    fprintf(stderr, "cannot load table\n");
    return 1;
  }

  FILE* f = fopen(argv[3], "rb");
  if (!f) { perror("open dump"); return 1; }
  u64 hdr[4];
  if (fread(hdr, 8, 4, f) != 4 || hdr[0] != 0xCCB0F0CEULL ||
      hdr[1] != (u64)m || hdr[2] != (u64)p) {
    fprintf(stderr, "bad dump header\n");
    return 1;
  }
  u64 total = hdr[3];
  std::vector<u8> bf(total);
  if (fread(bf.data(), 1, total, f) != total) {
    fprintf(stderr, "short dump\n");
    return 1;
  }
  fclose(f);

  // The brute-force index is stm-major with LEX ranks; enumerate states the
  // same way here and map each to the canonical index.
  Ranker rk;
  u64 C1 = rk.choose(g.N, p), C2 = rk.choose(g.N - p, p);
  if (2 * C1 * C2 != total) { fprintf(stderr, "size mismatch\n"); return 1; }
  u64 mismatches = 0, checked = 0;

  // lex enumeration: subsets in lexicographic order of sorted cell lists.
  std::vector<int> a(p), b(p);
  for (int i = 0; i < p; i++) a[i] = i;
  u64 bfIdx1 = 0;  // running lex rank of P1 placement
  while (true) {
    u64 amask = 0;
    for (int i = 0; i < p; i++) amask |= bit(a[i]);
    std::vector<int> comp;
    for (int c = 0; c < g.N; c++)
      if (!(amask >> c & 1)) comp.push_back(c);
    for (int i = 0; i < p; i++) b[i] = i;
    u64 bfIdx2 = 0;
    while (true) {
      u64 bmask = 0;
      for (int i = 0; i < p; i++) bmask |= bit(comp[b[i]]);
      // stm = P1: canonical index directly
      u8 v1 = bf[(0 * C1 + bfIdx1) * C2 + bfIdx2];
      u8 c1 = table.get(ix.indexOf(amask, bmask));
      if (v1 != c1) mismatches++;
      // stm = P2: canonicalize via rotation + color swap
      u8 v2 = bf[(1 * C1 + bfIdx1) * C2 + bfIdx2];
      u8 c2 = table.get(ix.indexOfP2ToMove(amask, bmask));
      if (v2 != c2) mismatches++;
      checked += 2;
      // next lex subset of comp
      int i = p - 1;
      while (i >= 0 && b[i] == (int)comp.size() - p + i) i--;
      if (i < 0) break;
      b[i]++;
      for (int j = i + 1; j < p; j++) b[j] = b[j - 1] + 1;
      bfIdx2++;
    }
    int i = p - 1;
    while (i >= 0 && a[i] == g.N - p + i) i--;
    if (i < 0) break;
    a[i]++;
    for (int j = i + 1; j < p; j++) a[j] = a[j - 1] + 1;
    bfIdx1++;
  }
  printf("crosscheck %dx%d p=%d: checked=%llu mismatches=%llu -> %s\n", m, m,
         p, (unsigned long long)checked, (unsigned long long)mismatches,
         mismatches == 0 ? "PASS" : "FAIL");
  return mismatches == 0 ? 0 : 1;
}
