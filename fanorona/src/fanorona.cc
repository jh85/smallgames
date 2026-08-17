#include "fanorona.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace fan {

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void Geom::Init(int w, int h) {
  W = w;
  H = h;
  N = w * h;
  if (N > kMaxPoints)
    throw std::runtime_error("board too large (max 32 points)");

  auto pt = [&](int x, int y) { return y * w + x; };
  auto inside = [&](int x, int y) { return x >= 0 && x < w && y >= 0 && y < h; };

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int p = pt(x, y);
      for (int d = 0; d < 8; d++) {
        const bool diag = d >= 4;
        if (diag && ((x + y) & 1)) continue;  // no diagonal lines here
        int cx = x, cy = y;
        int len = 0;
        while (inside(cx + kDirDx[d], cy + kDirDy[d])) {
          cx += kDirDx[d];
          cy += kDirDy[d];
          ray_pts[p][d][len++] = static_cast<uint8_t>(pt(cx, cy));
        }
        ray_len[p][d] = static_cast<uint8_t>(len);
        if (len > 0) adj[p] |= 1ull << ray_pts[p][d][0];
      }
    }
  }

  square = (w == h);

  // D4 transforms on point coordinates.
  auto apply = [&](int g, int x, int y, int* ox, int* oy) {
    // g: 0=id 1=rot90 2=rot180 3=rot270 4=flip-x 5=flip-y 6=flip-diag
    //    7=flip-antidiag
    switch (g) {
      case 0: *ox = x;         *oy = y;         break;
      case 1: *ox = h - 1 - y; *oy = x;         break;
      case 2: *ox = w - 1 - x; *oy = h - 1 - y; break;
      case 3: *ox = y;         *oy = w - 1 - x; break;
      case 4: *ox = w - 1 - x; *oy = y;         break;
      case 5: *ox = x;         *oy = h - 1 - y; break;
      case 6: *ox = y;         *oy = x;         break;
      case 7: *ox = h - 1 - y; *oy = w - 1 - x; break;
    }
  };
  for (int g = 0; g < 8; g++)
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int ox, oy;
        apply(g, x, y, &ox, &oy);
        perm[g][pt(x, y)] = static_cast<uint8_t>(pt(ox, oy));
      }

  // Automorphism check: a transform is a symmetry of the Fanorona line
  // graph only if it preserves line-adjacency (this rejects transforms
  // that swap strong and weak points on even-sided boards).
  sym_ok[0] = true;
  for (int g = 1; g < 8; g++) {
    bool ok = true;
    for (int p = 0; p < N && ok; p++) {
      uint64_t img = 0;
      uint64_t bits = adj[p];
      while (bits) {
        const int q = __builtin_ctzll(bits);
        bits &= bits - 1;
        img |= 1ull << perm[g][q];
      }
      if (img != adj[perm[g][p]]) ok = false;
    }
    sym_ok[g] = ok;
  }

  // D2 fundamental domain (group {0,2,4,5}): orbit minima. Available when
  // the whole D2 group is valid, i.e. both side lengths are odd.
  if (sym_ok[2] && sym_ok[4] && sym_ok[5]) {
    const int d2[4] = {0, 2, 4, 5};
    for (int p = 0; p < N; p++) {
      int mn = p;
      for (int i = 1; i < 4; i++) mn = std::min<int>(mn, perm[d2[i]][p]);
      if (mn == p) fd2_mask |= 1ull << p;
    }
  }

  if (!square) return;

  // Fundamental domain: {0 <= y <= x, x <= (W-1)/2}.
  const int xh = (w - 1) / 2;
  for (int x = 0; x <= xh; x++)
    for (int y = 0; y <= x; y++) fd_mask |= 1ull << pt(x, y);

  // Verify: every D4 point orbit's smallest-index element lies in the FD,
  // and the FD contains exactly one element per orbit.
  std::vector<int> orbit_min(N, -1);
  for (int p = 0; p < N; p++) {
    int mn = p;
    for (int g = 1; g < 8; g++) mn = std::min<int>(mn, perm[g][p]);
    orbit_min[p] = mn;
  }
  for (int p = 0; p < N; p++) {
    if (orbit_min[p] == p && !(fd_mask >> p & 1))
      throw std::runtime_error("D4 init: orbit minimum outside FD");
  }
  // Each orbit has exactly one FD member.
  for (int p = 0; p < N; p++) {
    if (orbit_min[p] != p) continue;
    int cnt = 0;
    uint64_t seen = 0;
    for (int g = 0; g < 8; g++) {
      const int q = perm[g][p];
      if (seen >> q & 1) continue;
      seen |= 1ull << q;
      if (fd_mask >> q & 1) cnt++;
    }
    if (cnt != 1) throw std::runtime_error("D4 init: FD not a transversal");
  }
}

// ---------------------------------------------------------------------------
// Captures
// ---------------------------------------------------------------------------

// Contiguous enemy-stone prefix of the ray from p in direction d.
static inline uint64_t CapturePrefix(const Geom& g, int p, int d,
                                     uint64_t enemy) {
  uint64_t caps = 0;
  const int len = g.ray_len[p][d];
  for (int i = 0; i < len; i++) {
    const int q = g.ray_pts[p][d][i];
    if (!(enemy >> q & 1)) break;
    caps |= 1ull << q;
  }
  return caps;
}

bool HasCapture(const Geom& g, const Pos& p) {
  const uint64_t own = p.Own(), opp = p.Opp(), occ = own | opp;
  uint64_t bits = own;
  while (bits) {
    const int u = __builtin_ctzll(bits);
    bits &= bits - 1;
    for (int d = 0; d < 8; d++) {
      if (!g.ray_len[u][d]) continue;
      const int v = g.ray_pts[u][d][0];
      if (occ >> v & 1) continue;
      // approach: enemy directly beyond v; withdrawal: enemy behind u
      if (g.ray_len[v][d] && (opp >> g.ray_pts[v][d][0] & 1)) return true;
      const int od = d ^ 1;
      if (g.ray_len[u][od] && (opp >> g.ray_pts[u][od][0] & 1)) return true;
    }
  }
  return false;
}

namespace {

// Recursive capture-sequence enumerator. Mutates a local copy of the board.
struct CapDfs {
  const Geom& g;
  std::vector<Pos>* out;
  bool mover_white;

  void Run(uint64_t white, uint64_t black, int cur, uint32_t visited,
           uint32_t dir_mask, int last_dir) {
    const uint64_t own = mover_white ? white : black;
    const uint64_t opp = mover_white ? black : white;
    const uint64_t occ = own | opp;
    for (int d = 0; d < 8; d++) {
      if (g.seq_rule == 0) {
        if (d == last_dir) continue;
      } else if (g.seq_rule == 1) {
        if (dir_mask >> d & 1) continue;
      } else {
        if (d == last_dir || d == (last_dir ^ 1)) continue;
      }
      if (!g.ray_len[cur][d]) continue;
      const int v = g.ray_pts[cur][d][0];
      if (occ >> v & 1) continue;
      if (visited >> v & 1) continue;
      const uint64_t appr = CapturePrefix(g, v, d, opp);
      const uint64_t wdr = CapturePrefix(g, cur, d ^ 1, opp);
      if (!appr && !wdr) continue;
      for (int choice = 0; choice < 2; choice++) {
        const uint64_t caps = choice == 0 ? appr : wdr;
        if (!caps) continue;
        // Move stone cur -> v, remove captured enemy stones.
        uint64_t nw = white, nb = black;
        if (mover_white) {
          nw = (nw & ~(1ull << cur)) | (1ull << v);
          nb &= ~caps;
        } else {
          nb = (nb & ~(1ull << cur)) | (1ull << v);
          nw &= ~caps;
        }
        // Stop here: emit the turn-boundary child (side flipped).
        out->push_back(Pos{nw, nb, !mover_white});
        // Continue the sequence.
        Run(nw, nb, v, visited | (1u << cur), dir_mask | (1u << d), d);
      }
    }
  }
};

}  // namespace

void GenMovesVec(const Geom& g, const Pos& p, std::vector<Pos>* out,
                 bool keep_dups) {
  const size_t begin = out->size();
  if (HasCapture(g, p)) {
    uint64_t own = p.Own();
    while (own) {
      const int u = __builtin_ctzll(own);
      own &= own - 1;
      CapDfs dfs{g, out, p.white_to_move};
      dfs.Run(p.white, p.black, u, 1u << u, 0, -1);
    }
  } else {
    const uint64_t own = p.Own();
    const uint64_t occ = own | p.Opp();
    uint64_t bits = own;
    while (bits) {
      const int u = __builtin_ctzll(bits);
      bits &= bits - 1;
      uint64_t targets = g.adj[u] & ~occ;
      while (targets) {
        const int v = __builtin_ctzll(targets);
        targets &= targets - 1;
        Pos c = p;
        const uint64_t mv = (1ull << u) | (1ull << v);
        if (p.white_to_move)
          c.white ^= mv;
        else
          c.black ^= mv;
        c.white_to_move = !p.white_to_move;
        out->push_back(c);
      }
    }
  }
  if (!keep_dups) {
    // Remove duplicate child boards (different capture sequences can reach
    // the same board). The retrograde counters require simple edges.
    std::sort(out->begin() + begin, out->end(),
              [](const Pos& x, const Pos& y) {
                if (x.white != y.white) return x.white < y.white;
                return x.black < y.black;
              });
    out->erase(std::unique(out->begin() + begin, out->end()),
               out->end());
  }
}

size_t GenMoves(const Geom& g, const Pos& p, Pos* out, bool keep_dups) {
  std::vector<Pos> v;
  v.reserve(64);
  GenMovesVec(g, p, &v, keep_dups);
  if (v.size() > kMaxMoves) {
    fprintf(stderr, "fan: move count %zu exceeds cap %d\n", v.size(),
            kMaxMoves);
    abort();
  }
  std::copy(v.begin(), v.end(), out);
  return v.size();
}

// ---------------------------------------------------------------------------
// Stepwise turn successors (perft cross-check)
// ---------------------------------------------------------------------------

void GenTurnSuccessors(const Geom& g, const TurnState& s,
                       std::vector<Pos>* out) {
  if (!s.mid) {
    // Turn start. Paika if no capture, else first capture steps (each step
    // either stops = emitted child, or continues mid-sequence).
    if (!HasCapture(g, s.pos)) {
      GenMovesVec(g, s.pos, out, /*keep_dups=*/true);
      return;
    }
    // Recurse through mid-sequence states; emit one child per stop.
    struct Rec {
      const Geom& g;
      std::vector<Pos>* out;
      void Step(uint64_t white, uint64_t black, bool mover_white, int cur,
                uint32_t visited, uint32_t dir_mask, int last_dir) {
        const uint64_t own = mover_white ? white : black;
        const uint64_t opp = mover_white ? black : white;
        const uint64_t occ = own | opp;
        for (int d = 0; d < 8; d++) {
          if (g.seq_rule == 0) {
            if (d == last_dir) continue;
          } else if (g.seq_rule == 1) {
            if (dir_mask >> d & 1) continue;
          } else {
            if (d == last_dir || d == (last_dir ^ 1)) continue;
          }
          if (!g.ray_len[cur][d]) continue;
          const int v = g.ray_pts[cur][d][0];
          if (occ >> v & 1) continue;
          if (visited >> v & 1) continue;
          const uint64_t appr = CapturePrefix(g, v, d, opp);
          const uint64_t wdr = CapturePrefix(g, cur, d ^ 1, opp);
          if (!appr && !wdr) continue;
          for (int choice = 0; choice < 2; choice++) {
            const uint64_t caps = choice == 0 ? appr : wdr;
            if (!caps) continue;
            uint64_t nw = white, nb = black;
            if (mover_white) {
              nw = (nw & ~(1ull << cur)) | (1ull << v);
              nb &= ~caps;
            } else {
              nb = (nb & ~(1ull << cur)) | (1ull << v);
              nw &= ~caps;
            }
            out->push_back(Pos{nw, nb, !mover_white});  // stop
            Step(nw, nb, mover_white, v, visited | (1u << cur),
                 dir_mask | (1u << d), d);
          }
        }
      }
    };
    Rec rec{g, out};
    uint64_t own = s.pos.Own();
    while (own) {
      const int u = __builtin_ctzll(own);
      own &= own - 1;
      rec.Step(s.pos.white, s.pos.black, s.pos.white_to_move, u, 1u << u, 0, -1);
    }
    return;
  }
  // Mid-sequence: only reachable inside Rec above; never called externally.
  abort();
}

// ---------------------------------------------------------------------------
// Setup / parsing / printing
// ---------------------------------------------------------------------------

Pos InitialPos(const Geom& g) {
  Pos p;
  for (int y = 0; y < g.H; y++) {
    for (int x = 0; x < g.W; x++) {
      const int i = y * g.W + x;
      if (y < g.H / 2) {
        p.black |= 1ull << i;
      } else if (y > g.H / 2) {
        p.white |= 1ull << i;
      } else {  // middle row, H odd
        if (x == g.W / 2) continue;  // center empty
        if (x % 2 == 0)
          p.black |= 1ull << i;
        else
          p.white |= 1ull << i;
      }
    }
  }
  p.white_to_move = true;
  return p;
}

Pos ParsePos(const Geom& g, const std::string& s) {
  Pos p;
  p.white_to_move = true;
  int x = 0, y = 0;
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    if (c == '/') {
      y++;
      x = 0;
      continue;
    }
    if (c == ' ') {
      p.white_to_move = s.size() > i + 1 && s[i + 1] != 'b';
      break;
    }
    if (y >= g.H || x >= g.W)
      throw std::runtime_error("bad position string (too many points)");
    const int pt = y * g.W + x;
    if (c == 'W' || c == 'w')
      p.white |= 1ull << pt;
    else if (c == 'B' || c == 'b')
      p.black |= 1ull << pt;
    else if (c != '.')
      throw std::runtime_error("bad position string (char)");
    x++;
  }
  return p;
}

std::string PosToString(const Geom& g, const Pos& p) {
  std::string s;
  for (int y = 0; y < g.H; y++) {
    for (int x = 0; x < g.W; x++) {
      const int i = y * g.W + x;
      s += (p.white >> i & 1) ? 'W' : (p.black >> i & 1) ? 'B' : '.';
    }
    if (y + 1 < g.H) s += '/';
  }
  s += p.white_to_move ? " w" : " b";
  return s;
}

void PrintPos(const Geom& g, const Pos& p, FILE* f) {
  for (int y = 0; y < g.H; y++) {
    for (int x = 0; x < g.W; x++) {
      const int i = y * g.W + x;
      fputc((p.white >> i & 1) ? 'W' : (p.black >> i & 1) ? 'B' : '.', f);
    }
    fputc('\n', f);
  }
  fprintf(f, "%s to move\n", p.white_to_move ? "White" : "Black");
}

uint64_t HashPos(const Pos& p) {
  uint64_t h = p.white * 0x9E3779B97F4A7C15ull;
  h ^= (p.black + 0x517cc1b727220a95ull) * 0xC2B2AE3D27D4EB4Full;
  h = h * 0x165667B19E3779F9ull + (p.white_to_move ? 0x9E3779B97F4A7C15ull : 0);
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;
  return h;
}

// ---------------------------------------------------------------------------
// perft
// ---------------------------------------------------------------------------

uint64_t Perft(const Geom& g, const Pos& p, int depth, bool stepwise,
               bool keep_dups) {
  if (depth == 0) return 1;
  std::vector<Pos> children;
  children.reserve(64);
  if (stepwise) {
    TurnState s{p, false, 0, 0, -1};
    GenTurnSuccessors(g, s, &children);
  } else {
    GenMovesVec(g, p, &children, keep_dups);
  }
  if (depth == 1) return children.size();
  uint64_t total = 0;
  for (const Pos& c : children) total += Perft(g, c, depth - 1, stepwise, keep_dups);
  return total;
}

}  // namespace fan
