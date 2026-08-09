// bruteforce.cpp -- Independent reference solver for small Chinese Checkers
// variants. Deliberately shares NO move-generation, rule, or canonicalization
// code with the optimized solver: explicit board arrays, recursive hop
// search, its own lexicographic ranking, full state space (both sides to
// move), no symmetry reduction, and plain value-iteration sweeps.
//
// Usage: bruteforce <m> <p> [dumpfile]
//   Prints full-space win/loss/draw/illegal counts (paper Table 1 layout)
//   and optionally dumps the value array (1 byte per state) for per-state
//   cross-checking against the optimized solver.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

typedef uint64_t u64;
typedef uint8_t u8;

static int m, p, N;
static bool allowNullMove = false;  // hop chains returning to origin = pass
static bool noGoalExit = false;     // pieces in their own goal may not leave it
static bool part2Soft = false;      // part-2 successors block loss proofs
                                    // instead of being forbidden move targets
static int ownStartEntry = 0;       // 0 = allowed, 1 = forbidden from outside,
                                    // 2 = always forbidden (dest in own start)
static bool passAlways = false;     // every state has a pass successor
static bool originOccupied = false; // origin stays occupied during hop chains
static bool layered = false;        // value iteration reads the previous pass's
                                    // snapshot instead of updating in place
static int gridId[7][7];        // (x,y) -> id, -1 outside
static int cx[49], cy[49];      // id -> coords
static u64 binom[64][16];

enum { V_UNK = 0, V_WIN = 1, V_LOSS = 2, V_ILL = 3 };

static void buildBoard() {
  N = m * m;
  for (int x = 0; x < 7; x++)
    for (int y = 0; y < 7; y++) gridId[x][y] = -1;
  int id = 0;
  for (int k = 0; k <= 2 * (m - 1); k++)
    for (int x = 0; x < m; x++) {
      int y = k - x;
      if (y < 0 || y >= m) continue;
      gridId[x][y] = id;
      cx[id] = x;
      cy[id] = y;
      id++;
    }
  for (int n = 0; n < 64; n++)
    for (int k = 0; k < 16; k++)
      binom[n][k] = (k == 0) ? 1 : (n == 0 ? 0 : binom[n - 1][k - 1] + binom[n - 1][k]);
}

// Lexicographic rank of the sorted cell list `cells` (k of them) among
// subsets of {0..n-1}.
static u64 lexRank(const int* cells, int k, int n) {
  u64 r = 0;
  int prev = -1;
  for (int i = 0; i < k; i++) {
    for (int c = prev + 1; c < cells[i]; c++) r += binom[n - 1 - c][k - 1 - i];
    prev = cells[i];
  }
  return r;
}

struct State {
  u8 board[49];  // 0 empty, 1 = player1 piece, 2 = player2 piece
  int stm;       // 1 or 2
};

// Full-space index: stm-major, then P1 placement (lex rank among N), then
// P2 placement (lex rank among the N-p cells not used by P1).
static u64 C1, C2;
static u64 stateIndex(const State& s) {
  int p1[8], p2[8], n1 = 0, n2 = 0;
  for (int c = 0; c < N; c++) {
    if (s.board[c] == 1) p1[n1++] = c;
    else if (s.board[c] == 2) p2[n2++] = c;
  }
  // compress p2 cells to positions among non-p1 cells
  int comp[8];
  for (int i = 0; i < n2; i++) {
    int pos = 0;
    for (int c = 0; c < p2[i]; c++)
      if (s.board[c] != 1) pos++;
    comp[i] = pos;
  }
  return ((u64)(s.stm - 1) * C1 + lexRank(p1, p, N)) * C2 + lexRank(comp, p, N - p);
}

static const int DX[6] = {1, -1, 0, 0, 1, -1};
static const int DY[6] = {0, 0, 1, -1, -1, 1};

// Recursive hop search; marks reachable landing cells in dest[].
static void hopSearch(const u8* board, int x, int y, bool* visited, bool* dest) {
  for (int d = 0; d < 6; d++) {
    int ox = x + DX[d], oy = y + DY[d];
    int lx = x + 2 * DX[d], ly = y + 2 * DY[d];
    if (lx < 0 || lx >= m || ly < 0 || ly >= m) continue;
    if (ox < 0 || ox >= m || oy < 0 || oy >= m) continue;
    int over = gridId[ox][oy], land = gridId[lx][ly];
    if (board[over] == 0) continue;   // must jump an adjacent piece
    if (board[land] != 0) continue;   // must land on empty
    if (visited[land]) continue;
    visited[land] = true;
    dest[land] = true;
    hopSearch(board, lx, ly, visited, dest);
  }
}

// All moves for the side to move; returns list of (from,to).
static int genMoves(const State& s, int moves[][2]) {
  int n = 0;
  for (int c = 0; c < N; c++) {
    if (s.board[c] != s.stm) continue;
    bool dest[49] = {false}, visited[49] = {false};
    int x = cx[c], y = cy[c];
    // steps
    for (int d = 0; d < 6; d++) {
      int nx2 = x + DX[d], ny2 = y + DY[d];
      if (nx2 < 0 || nx2 >= m || ny2 < 0 || ny2 >= m) continue;
      if (s.board[gridId[nx2][ny2]] == 0) dest[gridId[nx2][ny2]] = true;
    }
    // hop chains: vacate the origin first (unless variant)
    u8 tmp[49];
    memcpy(tmp, s.board, N);
    if (!originOccupied) tmp[c] = 0;
    if (allowNullMove) {
      // Allow chains to land back on the (now empty) origin: a legal hop
      // sequence that ends where it started = a null move / pass.
      hopSearch(tmp, x, y, visited, dest);
    } else {
      visited[c] = true;  // may cross the origin but never end there
      hopSearch(tmp, x, y, visited, dest);
      dest[c] = false;
    }
    bool inOwnGoal =
        (s.stm == 1) ? (c >= N - p) : (c < p);  // P1 goal bottom, P2 goal top
    bool inOwnStart = (s.stm == 1) ? (c < p) : (c >= N - p);
    for (int t = 0; t < N; t++)
      if (dest[t]) {
        if (noGoalExit && inOwnGoal &&
            !((s.stm == 1) ? (t >= N - p) : (t < p)))
          continue;  // may not leave own goal area
        bool destOwnStart = (s.stm == 1) ? (t < p) : (t >= N - p);
        if (destOwnStart &&
            (ownStartEntry == 2 || (ownStartEntry == 1 && !inOwnStart)))
          continue;  // may not move (back) into own start area
        moves[n][0] = c;
        moves[n][1] = t;
        n++;
      }
  }
  return n;
}

// Win condition (Def. 1) for player `pl`: pl's goal area completely filled,
// at least one piece in it belongs to pl. P1's goal = last p ids (bottom),
// P2's goal = first p ids (top).
static bool winCond(const State& s, int pl) {
  int lo = (pl == 1) ? N - p : 0;
  int own = 0;
  for (int i = lo; i < lo + p; i++) {
    if (s.board[i] == 0) return false;
    if (s.board[i] == pl) own++;
  }
  return own > 0;
}

static bool isIllegalPart1(const State& s) { return winCond(s, s.stm); }

static bool isIllegalPart2(const State& s) {
  if (p == 6) {
    // part 2, for each player: goal tip empty and the four outer-edge cells
    // of the goal triangle all occupied by the opponent.
    // player 1's goal (bottom): tip (m-1,m-1), edges (m-2,m-1),(m-1,m-2),
    // (m-3,m-1),(m-1,m-3), blocked by player 2's pieces.
    {
      int tip = gridId[m - 1][m - 1];
      static int altPat = getenv("CC_ALT_PART2") ? atoi(getenv("CC_ALT_PART2")) : 0;
      int e[4];
      if (altPat == 1) {
        e[0] = gridId[m - 2][m - 1]; e[1] = gridId[m - 1][m - 2];
        e[2] = gridId[m - 3][m - 1]; e[3] = gridId[m - 2][m - 2];
      } else {
        e[0] = gridId[m - 2][m - 1]; e[1] = gridId[m - 1][m - 2];
        e[2] = gridId[m - 3][m - 1]; e[3] = gridId[m - 1][m - 3];
      }
      bool blocked = (s.board[tip] == 0);
      for (int i = 0; i < 4 && blocked; i++) blocked = (s.board[e[i]] == 2);
      if (blocked) return true;
    }
    {
      int tip = gridId[0][0];
      static int altPat2 = getenv("CC_ALT_PART2") ? atoi(getenv("CC_ALT_PART2")) : 0;
      int e[4];
      if (altPat2 == 1) {
        e[0] = gridId[1][0]; e[1] = gridId[0][1];
        e[2] = gridId[2][0]; e[3] = gridId[1][1];
      } else {
        e[0] = gridId[1][0]; e[1] = gridId[0][1];
        e[2] = gridId[2][0]; e[3] = gridId[0][2];
      }
      bool blocked = (s.board[tip] == 0);
      for (int i = 0; i < 4 && blocked; i++) blocked = (s.board[e[i]] == 1);
      if (blocked) return true;
    }
  }
  return false;
}

static bool isIllegal(const State& s) {
  return isIllegalPart1(s) || isIllegalPart2(s);
}

static std::vector<u8> val;

// Enumerate every state (both stm), calling f(state).
template <typename F>
static void forEachState(F f) {
  std::vector<int> a(p), b(p);
  // iterate P1 subsets
  for (int i = 0; i < p; i++) a[i] = i;
  while (true) {
    // iterate P2 subsets of remaining cells
    State s;
    memset(s.board, 0, sizeof(s.board));
    for (int i = 0; i < p; i++) s.board[a[i]] = 1;
    std::vector<int> freeCells;
    for (int c = 0; c < N; c++)
      if (s.board[c] == 0) freeCells.push_back(c);
    for (int i = 0; i < p; i++) b[i] = i;
    while (true) {
      for (int i = 0; i < p; i++) s.board[freeCells[b[i]]] = 2;
      s.stm = 1;
      f(s);
      s.stm = 2;
      f(s);
      for (int i = 0; i < p; i++) s.board[freeCells[b[i]]] = 0;
      // next P2 subset (lex)
      int i = p - 1;
      while (i >= 0 && b[i] == (int)freeCells.size() - p + i) i--;
      if (i < 0) break;
      b[i]++;
      for (int j = i + 1; j < p; j++) b[j] = b[j - 1] + 1;
    }
    // next P1 subset (lex)
    int i = p - 1;
    while (i >= 0 && a[i] == N - p + i) i--;
    if (i < 0) break;
    a[i]++;
    for (int j = i + 1; j < p; j++) a[j] = a[j - 1] + 1;
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <m> <p> [dumpfile]\n", argv[0]);
    return 2;
  }
  m = atoi(argv[1]);
  p = atoi(argv[2]);
  for (int i = 3; i < argc; i++)
    if (!strcmp(argv[i], "--null")) {
      allowNullMove = true;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--nogoalexit")) {
      noGoalExit = true;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--part2soft")) {
      part2Soft = true;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--nostartentry")) {
      ownStartEntry = 1;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--nostartentry2")) {
      ownStartEntry = 2;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--passalways")) {
      passAlways = true;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--originoccupied")) {
      originOccupied = true;
      argv[i] = nullptr;
    } else if (!strcmp(argv[i], "--layers")) {
      layered = true;
      argv[i] = nullptr;
    }
  // compact argv after flag removal
  {
    int w = 3;
    for (int i = 3; i < argc; i++)
      if (argv[i]) argv[w++] = argv[i];
    argc = w;
  }
  buildBoard();
  C1 = binom[N][p];
  C2 = binom[N - p][p];
  u64 total = 2 * C1 * C2;
  printf("bruteforce %dx%d p=%d, full state space = %llu\n", m, m, p,
         (unsigned long long)total);
  val.assign(total, V_UNK);

  // Phase 1: static classification.
  u64 nIll = 0, nTermLoss = 0;
  forEachState([&](const State& s) {
    u64 idx = stateIndex(s);
    if (isIllegal(s)) {
      val[idx] = V_ILL;
      nIll++;
    } else if (winCond(s, 3 - s.stm)) {
      val[idx] = V_LOSS;  // opponent has won: terminal loss for the mover
      nTermLoss++;
    }
  });
  printf("illegal=%llu terminal_losses=%llu\n", (unsigned long long)nIll,
         (unsigned long long)nTermLoss);

  // Phase 2: value iteration to fixpoint.
  u64 stuckLoss = 0;
  int pass = 0;
  u64 cumDecided = nIll + nTermLoss;
  std::vector<u8> valSnapshot;
  while (true) {
    pass++;
    if (layered) valSnapshot = val;  // strict layering: read previous layer
    const std::vector<u8>& rd = layered ? valSnapshot : val;
    u64 changed = 0;
    forEachState([&](const State& s) {
      u64 idx = stateIndex(s);
      if (val[idx] != V_UNK) return;
      int moves[512][2];
      int nm = genMoves(s, moves);
      bool anyLoss = false, allWin = true;
      int legal = 0;
      State t = s;
      t.stm = 3 - s.stm;
      for (int i = 0; i < nm; i++) {
        t.board[moves[i][0]] = 0;
        t.board[moves[i][1]] = s.stm;
        u64 tidx = stateIndex(t);
        u8 v = rd[tidx];
        if (v == V_ILL && part2Soft && !isIllegalPart1(t)) {
          // part-2 illegal successor: reachable but never provable; it
          // blocks a loss proof like an unknown successor.
          t.board[moves[i][1]] = 0;
          t.board[moves[i][0]] = s.stm;
          legal++;
          allWin = false;
          continue;
        }
        t.board[moves[i][1]] = 0;
        t.board[moves[i][0]] = s.stm;
        if (v == V_ILL) continue;  // moves into illegal states are forbidden
        legal++;
        if (v == V_LOSS) anyLoss = true;
        else if (v != V_WIN) allWin = false;
      }
      if (passAlways) {
        // Pass successor: same board, other side to move.
        State t2 = s;
        t2.stm = 3 - s.stm;
        u8 v = rd[stateIndex(t2)];
        if (v != V_ILL) {
          legal++;
          if (v == V_LOSS) anyLoss = true;
          else if (v != V_WIN) allWin = false;
        }
      }
      if (anyLoss) {
        val[idx] = V_WIN;
        changed++;
      } else if (allWin) {  // includes legal == 0 (stuck: mover loses)
        if (legal == 0) stuckLoss++;
        val[idx] = V_LOSS;
        changed++;
      }
    });
    cumDecided += changed;
    if (layered)
      printf("layer %d: changed=%llu cumulative_decided=%llu (with illegal: %llu)\n",
             pass, (unsigned long long)changed,
             (unsigned long long)(cumDecided - nIll),
             (unsigned long long)cumDecided);
    else
      printf("pass %d: changed=%llu\n", pass, (unsigned long long)changed);
    if (changed == 0) break;
  }

  u64 nW = 0, nL = 0, nD = 0, nI = 0;
  for (u64 i = 0; i < total; i++) {
    switch (val[i]) {
      case V_WIN: nW++; break;
      case V_LOSS: nL++; break;
      case V_UNK: nD++; break;
      case V_ILL: nI++; break;
    }
  }
  printf("RESULT m=%d p=%d positions=%llu wins=%llu losses=%llu draws=%llu "
         "illegal=%llu stuck_loss_states=%llu\n",
         m, p, (unsigned long long)total, (unsigned long long)nW,
         (unsigned long long)nL, (unsigned long long)nD,
         (unsigned long long)nI, (unsigned long long)stuckLoss);

  if (argc > 3) {
    FILE* f = fopen(argv[3], "wb");
    if (!f) { perror("fopen dump"); return 1; }
    u64 hdr[4] = {0xCCB0F0CEULL, (u64)m, (u64)p, total};
    fwrite(hdr, 8, 4, f);
    fwrite(val.data(), 1, total, f);
    fclose(f);
    printf("dumped values to %s\n", argv[3]);
  }
  return 0;
}
