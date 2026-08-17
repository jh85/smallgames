/*
  Fanorona — board representation, exact rules, move generation.

  A game state in the WDL table is a *turn-start* position (board + side to
  move). Capturing sequences are collapsed into single "composite" moves:
  because only the mover makes decisions during a sequence and stopping is
  allowed after every step, the composite-move graph is strategically
  identical to the real game. This keeps the state space at ~3^(W*H)*2 and
  avoids path-dependent mid-turn states (visited set, last direction) in the
  table.

  Rules implemented (Schadd et al., "Fanorona is a draw", ICGA Journal 2007):
    - moves along grid lines; diagonal lines exist only from points with
      (x+y) even;
    - approach capture: enemy stones beyond the target point, contiguous
      line captured fully; withdrawal capture: enemy stones behind the
      origin, likewise; if both exist for the same step they are two
      distinct move options;
    - capture is mandatory when available;
    - continuation captures with the same stone are optional; within a
      sequence the stone may not revisit a point it occupied this turn and
      may not repeat the immediately preceding direction;
    - win = opponent has no stones; a side to move with no legal move loses
      (stalemate rule, switchable to draw);
    - draw = infinite play (decided by the retrograde fixpoint).
*/

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fan {

constexpr int kMaxPoints = 32;  // combinadic indexing supports up to 32 points
constexpr int kMaxMoves = 4096; // per-position composite/paika move cap (checked)

// Compass directions: 0=E 1=W 2=S 3=N 4=SE 5=NW 6=SW 7=NE
// (d^1 is always the opposite direction)
constexpr int kDirDx[8] = {1, -1, 0, 0, 1, -1, -1, 1};
constexpr int kDirDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

struct Geom {
  int W = 0, H = 0, N = 0;

  uint64_t adj[kMaxPoints] = {};  // line-adjacent points

  // Ray points starting one step from p in direction d, in increasing
  // distance order. A ray follows a straight grid line; it is empty when
  // the first step is not a legal line step.
  static constexpr int kMaxRay = kMaxPoints - 1;
  uint8_t ray_pts[kMaxPoints][8][kMaxRay] = {};
  uint8_t ray_len[kMaxPoints][8] = {};

  // Board symmetries. perm[g][p] = image of point p under transform g
  // (0=id 1=rot90 2=rot180 3=rot270 4=flip-x 5=flip-y 6=flip-diag
  // 7=flip-antidiag), computed geometrically for every board. sym_ok[g] is
  // true only when transform g is a genuine automorphism of the Fanorona
  // line graph (verified at init via adjacency): transforms that swap
  // strong and weak points are rejected (e.g. flip-x when W is even,
  // rot90 when the side length is even).
  //
  // fd_mask (D4, square boards): fundamental-domain points; with the
  // natural numbering (p = y*W+x) the smallest-indexed element of every
  // point orbit lies in the fundamental domain (verified at init).
  // fd2_mask (D2, group {0,2,4,5}, valid iff both side lengths are odd):
  // the orbit-minimum points.
  bool square = false;
  uint8_t perm[8][kMaxPoints] = {};
  bool sym_ok[8] = {};
  uint64_t fd_mask = 0;
  uint64_t fd2_mask = 0;

  // Capture-sequence direction rule:
  //   0 = a step may not repeat the immediately preceding direction
  //       (literal reading of Schadd et al.);
  //   1 = no direction may be used twice in the whole sequence;
  //   2 = a step may neither repeat nor reverse the preceding direction.
  int seq_rule = 0;

  void Init(int w, int h);

  bool StepOk(int p, int d) const { return ray_len[p][d] > 0; }
  int Step(int p, int d) const { return ray_pts[p][d][0]; }
};

// Position. `white`/`black` are bitboards over points. `white_to_move` is
// the actual side to move (tables store the folded white-to-move form).
struct Pos {
  uint64_t white = 0, black = 0;
  bool white_to_move = true;

  uint64_t Own() const { return white_to_move ? white : black; }
  uint64_t Opp() const { return white_to_move ? black : white; }
  int OwnCount() const { return __builtin_popcountll(Own()); }
  int OppCount() const { return __builtin_popcountll(Opp()); }

  bool operator==(const Pos& o) const {
    return white == o.white && black == o.black &&
           white_to_move == o.white_to_move;
  }
};

// ---------------------------------------------------------------------------
// Move generation
// ---------------------------------------------------------------------------

// True if the side to move has at least one capturing step.
bool HasCapture(const Geom& g, const Pos& p);

// Generates all distinct child positions (side to move flipped).
//  - If a capture is available: enumerates all capturing sequences as
//    composite moves (each stop point = one child). Duplicate child boards
    // from different sequences are removed unless keep_dups (perft mode).
//  - Otherwise: all paika (non-capture) single steps.
// Returns the number of children written to out (capacity kMaxMoves).
size_t GenMoves(const Geom& g, const Pos& p, Pos* out, bool keep_dups = false);

// Same but without dedup and without the move cap: appends to a vector.
// (Used by perft with duplicate sequence counting.)
void GenMovesVec(const Geom& g, const Pos& p, std::vector<Pos>* out,
                 bool keep_dups);

// ---------------------------------------------------------------------------
// Stepwise (real game tree) generation for perft cross-checks
// ---------------------------------------------------------------------------
//
// A turn is either a paika move, or a capture step after which the same
// player either stops or continues. PerftTurns counts turn-boundary nodes;
// it must match composite perft with keep_dups=true exactly.

struct TurnState {
  Pos pos;
  // Mid-sequence state (valid iff mid == true).
  bool mid = false;
  int cur = 0;             // current point of the capturing stone
  uint32_t visited = 0;    // points occupied by the stone this turn
  int last_dir = -1;       // direction of the previous step, -1 = none
};

// Appends successor turn-boundary states: each is (board after a paika move
// or after a completed capture sequence, side flipped). Sequences are counted
// with multiplicity (one per distinct step path), matching composite perft
// with keep_dups=true.
void GenTurnSuccessors(const Geom& g, const TurnState& s,
                       std::vector<Pos>* out);

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

// Initial position: outer rows filled (black top, white bottom); for odd H
// the middle row alternates B W B W ... W B with the center point empty.
Pos InitialPos(const Geom& g);

// Parses "BBBBB/BBBBB/BW.WB/WWWWW/WWWWW w" (top row first). Side defaults
// to white.
Pos ParsePos(const Geom& g, const std::string& s);
std::string PosToString(const Geom& g, const Pos& p);
void PrintPos(const Geom& g, const Pos& p, FILE* f = stdout);

// Zobrist-style hash (for the verifier / brute-force tools).
uint64_t HashPos(const Pos& p);

// perft over turn boundaries. stepwise selects the generator.
uint64_t Perft(const Geom& g, const Pos& p, int depth, bool stepwise,
               bool keep_dups);

}  // namespace fan
