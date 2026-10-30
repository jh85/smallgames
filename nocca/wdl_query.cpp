// Minimal NOCCA x NOCCA WDL-table query example (C++17, no dependencies).
// Build: c++ -O2 -std=c++17 -o wdl_query wdl_query.cpp
// Usage: ./wdl_query wdl_5x6.bin b b b b b - ... - w w w w w

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t;
static constexpr size_t HEADER_BYTES = 8192;
static const int BASE[4] = {0, 1, 3, 7};

[[noreturn]] static void fail(const char* s) { std::fprintf(stderr, "%s\n", s); std::exit(1); }

struct Header {
  char magic[8];
  u32 version, width, length, pieces;
  u64 positions, words;
  u32 passes_done, done;
  u64 wins_by_pass[256], losses_by_pass[256];
};

struct States {
  int white[15]{}, black[15]{}, top[15]{}, swap[15]{};
  void init() {
    for (int h = 0; h <= 3; ++h) for (int p = 0; p < (1 << h); ++p) {
      int s = BASE[h] + p;
      black[s] = __builtin_popcount((unsigned)p); white[s] = h - black[s];
      top[s] = h ? ((p >> (h - 1)) & 1) : -1;
      swap[s] = BASE[h] + ((~p) & ((1 << h) - 1));
    }
  }
} states;

// The same reduced ZDD used by the solver. Path counts make it a minimal perfect hash.
struct Zdd {
  int squares, pieces, depth;
  std::vector<u8> square, state;
  std::vector<u32> low, high;
  std::vector<u64> count, low_count;
  std::unordered_map<u32, u32> memo;
  std::unordered_map<u64, u32> unique;
  u32 root = 0;

  static u32 signature(int used, int nw, int nb, int white_top, int black_top) {
    return (u32)used | (u32)nw << 1 | (u32)nb << 4 |
           (u32)white_top << 7 | (u32)black_top << 8;
  }
  u32 make(int d, u32 sig) {
    if (d == depth) {
      int nw = (sig >> 1) & 7, nb = (sig >> 4) & 7;
      return (nw == pieces && nb == pieces && (sig & 0x180) == 0x180) ? 1 : 0;
    }
    u32 key = (u32)d << 9 | sig;
    if (auto it = memo.find(key); it != memo.end()) return it->second;
    int used = sig & 1, nw = (sig >> 1) & 7, nb = (sig >> 4) & 7;
    int wt = (sig >> 7) & 1, bt = (sig >> 8) & 1, s = d % 14;
    int used2 = used, nb2 = nb, bt2 = bt; bool ok = true;
    if (s == 13) { if (!used2 && (nb2 += 3) <= pieces) bt2 = 1; else if (!used2) ok = false; used2 = 0; }
    u32 lo = ok ? make(d + 1, signature(used2, nw, nb2, wt, bt2)) : 0;
    u32 hi = 0;
    if (!used && nw + states.white[s] <= pieces && nb + states.black[s] <= pieces) {
      hi = make(d + 1, signature(s == 13 ? 0 : 1, nw + states.white[s],
          nb + states.black[s], wt | (states.top[s] == 0), bt | (states.top[s] == 1)));
    }
    u32 result;
    if (!hi) result = lo;
    else {
      u64 ukey = (u64)d << 48 | (u64)lo << 24 | hi;
      if (auto it = unique.find(ukey); it != unique.end()) result = it->second;
      else { result = square.size(); square.push_back(d / 14); state.push_back(s);
             low.push_back(lo); high.push_back(hi); unique[ukey] = result; }
    }
    return memo[key] = result;
  }
  void build(int n_squares, int n_pieces) {
    squares = n_squares; pieces = n_pieces; depth = squares * 14;
    square.assign(2, squares); state.assign(2, 0); low.assign(2, 0); high.assign(2, 0);
    root = make(0, 0); count.assign(square.size(), 0); count[1] = 1;
    for (u32 i = 2; i < square.size(); ++i) count[i] = count[low[i]] + count[high[i]];
    low_count.resize(square.size());
    for (u32 i = 2; i < square.size(); ++i) low_count[i] = count[low[i]];
  }
  u64 rank(const u8* position) const {
    u32 node = root; u64 rank = 0;
    while (node > 1) {
      if (position[square[node]] == state[node]) { rank += low_count[node]; node = high[node]; }
      else node = low[node];
    }
    return node == 1 ? rank : UINT64_MAX;
  }
};

static int parse(const std::string& text) {
  if (text == "-") return 0;
  if (text.empty() || text.size() > 3) return -1;
  int pattern = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == 'b' || text[i] == 'B') pattern |= 1 << i;
    else if (text[i] != 'w' && text[i] != 'W') return -1;
  }
  return BASE[text.size()] + pattern; // token is bottom-to-top, e.g. "wb"
}

int main(int argc, char** argv) {
  if (argc < 4) fail("usage: wdl_query TABLE <b|w> CELL... (row-major, '-' or bottom-to-top stack)");
  int fd = open(argv[1], O_RDONLY); if (fd < 0) fail("cannot open table");
  Header h{}; if (read(fd, &h, sizeof h) != (ssize_t)sizeof h) fail("cannot read header");
  if (std::memcmp(h.magic, "NOCCAWDL", 8) || argc != 3 + (int)(h.width * h.length)) fail("bad table or cell count");
  states.init(); Zdd z; z.build(h.width * h.length, h.pieces);
  if (z.count[z.root] != h.positions) fail("table/ZDD position-count mismatch");
  std::vector<u8> p(h.width * h.length), canonical(p.size()); int nw = 0, nb = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    int s = parse(argv[i + 3]); if (s < 0) fail("bad cell");
    p[i] = s; nw += states.white[s]; nb += states.black[s];
  }
  if (nw != (int)h.pieces || nb != (int)h.pieces) fail("position must contain exactly five pieces per player");
  bool white_to_move = argv[2][0] == 'w' || argv[2][0] == 'W';
  if (white_to_move) for (u32 i = 0; i < p.size(); ++i)
    canonical[(h.length - 1 - i / h.width) * h.width + i % h.width] = states.swap[p[i]];
  else canonical = p;
  u64 rank = z.rank(canonical.data()); if (rank == UINT64_MAX) fail("position is not pseudo-reachable");
  u64 word; if (pread(fd, &word, 8, HEADER_BYTES + (rank >> 5) * 8) != 8) fail("table read failed");
  unsigned value = (word >> (2 * (rank & 31))) & 3;
  const char* names[] = {h.done ? "DRAW" : "UNKNOWN", "WIN", "LOSS", "INVALID"};
  std::printf("%s (rank %llu)\n", names[value], (unsigned long long)rank);
}
