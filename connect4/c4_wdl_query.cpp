// c4_wdl_query.cpp — standalone reader for the Connect Four WDL tables.
//
// Build:  g++ -O2 -std=c++17 -o c4_wdl_query c4_wdl_query.cpp -lzstd
// Usage:  ./c4_wdl_query DIR [MOVES]
//   DIR    a board directory: zdd.bin or zdd.bin.zst, and slab_<ply>.bin or slab_<ply>.bin.zst
//   MOVES  columns played from the empty board, 0-indexed from the left, e.g. 3323 ('-' or
//          nothing = the empty board). Prints the board, the value for the side to move and the
//          value of every move.
//
// Positions. A position after k moves is stored at ply k. Its index ("rank") is its position in
// the lexicographic order of all positions of that ply, computed from the ZDD in zdd.bin.
// Positions with a four-in-a-row and full boards are not stored; the program detects them itself.
//
// zdd.bin (little-endian): u64 header[8] = {0xC4C4C4C4C4C4C4C4, W, H, nnodes, W*H+1, 0, 0, 0};
// u32 roots[W*H+1]; u64 count[W*H+1]; u8 var[nnodes]; u32 lo[nnodes]; u32 hi[nnodes];
// u64 cntlo[nnodes]. Nodes 0 and 1 are the false and true leaves. A position is a list of W
// column states. The state of a column with h stones is (2^h - 1) + p, where bit r of p is 1 when
// the stone in row r (0 = bottom) belongs to the first player. Each state is SB bits, where
// SB = bits needed for 2^(H+1) - 2 (8 when H = 7). Node n tests bit (SB-1-j) of column c's state,
// where var[n] = c*SB + j. rank: start at roots[k] with r = 0. While n > 1: if the bit is set,
// r += cntlo[n] and n = hi[n]; otherwise n = lo[n]. The walk ends at node 1 for every stored
// position.
//
// slab_<k>.bin: one value per rank (0 = draw, 1 = win, 2 = loss, for the side to move). Base-3
// when the directory has a PACK_BASE3 file: byte i/5 = sum of v_j * 3^j for j = i mod 5. Otherwise
// 2-bit: value i is bits 2*(i mod 4) of byte i/4.
//
// slab_<k>.bin.zst: the same bytes in the zstd seekable format. Independent frames are followed by
// a skippable frame with a seek table: (compressed size, decompressed size) as u32 pairs, then the
// footer: u32 frame count, u8 descriptor, u32 magic 0x8F92EAB1. `zstd -d` restores slab_<k>.bin.
// This program decompresses only the one frame holding the wanted byte.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <zstd.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;

static void die(const char* m, const std::string& a = "") {
  fprintf(stderr, "error: %s %s\n", m, a.c_str());
  exit(1);
}
static bool exists(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (f) fclose(f);
  return f != nullptr;
}

// ---- reading a whole file, decompressing it if it is .zst ----
static std::vector<u8> readAll(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) die("cannot open", path);
  std::vector<u8> in;
  u8 buf[1 << 16];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) in.insert(in.end(), buf, buf + n);
  fclose(f);
  return in;
}
static std::vector<u8> readMaybeZst(const std::string& plain) {
  if (exists(plain)) return readAll(plain);
  std::vector<u8> z = readAll(plain + ".zst"), out;
  if (z.size() >= 9) {                 // size the output from the seek table, if there is one
    u32 nf, magic;
    memcpy(&nf, &z[z.size() - 9], 4);
    memcpy(&magic, &z[z.size() - 4], 4);
    size_t es = (z[z.size() - 5] & 0x80) ? 12 : 8;
    if (magic == 0x8F92EAB1u && (size_t)nf * es + 9 <= z.size()) {
      u64 total = 0;
      for (size_t i = 0, at = z.size() - 9 - (size_t)nf * es; i < nf; ++i, at += es) {
        u32 d;
        memcpy(&d, &z[at + 4], 4);
        total += d;
      }
      out.reserve(total);
    }
  }
  ZSTD_DStream* ds = ZSTD_createDStream();
  ZSTD_inBuffer in{z.data(), z.size(), 0};
  std::vector<u8> chunk(ZSTD_DStreamOutSize());
  while (in.pos < in.size) {
    ZSTD_outBuffer o{chunk.data(), chunk.size(), 0};
    size_t r = ZSTD_decompressStream(ds, &o, &in);   // skippable frames (the seek table) are skipped
    if (ZSTD_isError(r)) die("zstd error in", plain + ".zst");
    out.insert(out.end(), chunk.data(), chunk.data() + o.pos);
  }
  ZSTD_freeDStream(ds);
  return out;
}

// ---- the ZDD (read in place from one buffer; the node arrays are not aligned, so use memcpy) ----
static int W, H, SB, CELLS;
static std::vector<u8> zbuf;
static std::vector<u32> roots;
static const u8 *var_, *lo_, *hi_, *cntlo_;
static inline u32 rd32(const u8* p, u64 i) { u32 v; memcpy(&v, p + 4 * i, 4); return v; }
static inline u64 rd64(const u8* p, u64 i) { u64 v; memcpy(&v, p + 8 * i, 8); return v; }

static void loadZdd(const std::string& dir) {
  zbuf = readMaybeZst(dir + "/zdd.bin");
  u64 hdr[8];
  if (zbuf.size() < sizeof hdr) die("zdd.bin is truncated");
  memcpy(hdr, zbuf.data(), sizeof hdr);
  if (hdr[0] != 0xC4C4C4C4C4C4C4C4ull) die("not a zdd.bin");
  W = (int)hdr[1]; H = (int)hdr[2]; CELLS = W * H;
  u64 nn = hdr[3];
  int NS = (1 << (H + 1)) - 1;
  SB = 0;
  while ((1 << SB) <= NS) ++SB;
  const u8* p = zbuf.data() + sizeof hdr;
  roots.resize(CELLS + 1);
  memcpy(roots.data(), p, 4 * (CELLS + 1));
  p += 4 * (CELLS + 1) + 8 * (CELLS + 1);          // roots, then per-ply counts (unused here)
  var_ = p; p += nn;
  lo_ = p; p += 4 * nn;
  hi_ = p; p += 4 * nn;
  cntlo_ = p; p += 8 * nn;
  if ((u64)(p - zbuf.data()) > zbuf.size()) die("zdd.bin is truncated");
}
static u64 rankOf(int ply, const std::vector<int>& state) {
  u32 n = roots[ply];
  u64 r = 0;
  while (n > 1) {
    int c = var_[n] / SB, j = var_[n] % SB;
    if ((state[c] >> (SB - 1 - j)) & 1) { r += rd64(cntlo_, n); n = rd32(hi_, n); }
    else n = rd32(lo_, n);
  }
  if (n != 1) die("position is not in the table");
  return r;
}

// ---- slabs ----
static std::string DIR;
static bool BASE3;
struct Slab {
  FILE* f = nullptr;
  bool zst = false;
  std::vector<u64> zoff, doff;     // per frame: compressed offset, decompressed offset (+ end)
  long cached = -1;
  std::vector<u8> frame;
};
static std::map<int, Slab> slabs;

static Slab& openSlab(int ply) {
  Slab& s = slabs[ply];
  if (s.f) return s;
  std::string plain = DIR + "/slab_" + std::to_string(ply) + ".bin";
  if ((s.f = fopen(plain.c_str(), "rb"))) return s;
  if (!(s.f = fopen((plain + ".zst").c_str(), "rb"))) die("missing", plain + "[.zst]");
  s.zst = true;
  u8 foot[9];
  fseeko(s.f, -9, SEEK_END);
  if (fread(foot, 1, 9, s.f) != 9) die("bad seek table in", plain + ".zst");
  u32 nf, magic;
  memcpy(&nf, foot, 4);
  memcpy(&magic, foot + 5, 4);
  if (magic != 0x8F92EAB1u) die("no seek table in", plain + ".zst");
  int es = (foot[4] & 0x80) ? 12 : 8;
  std::vector<u8> tab((size_t)nf * es);
  fseeko(s.f, -9 - (off_t)tab.size(), SEEK_END);
  if (fread(tab.data(), 1, tab.size(), s.f) != tab.size()) die("bad seek table in", plain + ".zst");
  s.zoff.assign(nf + 1, 0);
  s.doff.assign(nf + 1, 0);
  for (u32 i = 0; i < nf; ++i) {
    u32 c, d;
    memcpy(&c, &tab[(size_t)i * es], 4);
    memcpy(&d, &tab[(size_t)i * es + 4], 4);
    s.zoff[i + 1] = s.zoff[i] + c;
    s.doff[i + 1] = s.doff[i] + d;
  }
  return s;
}
static u8 slabByte(int ply, u64 at) {
  Slab& s = openSlab(ply);
  if (!s.zst) {
    fseeko(s.f, (off_t)at, SEEK_SET);
    int c = fgetc(s.f);
    if (c == EOF) die("slab too short");
    return (u8)c;
  }
  long fr = (long)(std::upper_bound(s.doff.begin(), s.doff.end(), at) - s.doff.begin()) - 1;
  if (fr < 0 || fr >= (long)s.doff.size() - 1) die("rank beyond the slab");
  if (fr != s.cached) {
    std::vector<u8> z(s.zoff[fr + 1] - s.zoff[fr]);
    fseeko(s.f, (off_t)s.zoff[fr], SEEK_SET);
    if (fread(z.data(), 1, z.size(), s.f) != z.size()) die("short read");
    s.frame.resize(s.doff[fr + 1] - s.doff[fr]);
    size_t r = ZSTD_decompress(s.frame.data(), s.frame.size(), z.data(), z.size());
    if (ZSTD_isError(r) || r != s.frame.size()) die("corrupt frame");
    s.cached = fr;
  }
  return s.frame[at - s.doff[fr]];
}
static int valueAt(int ply, u64 rank) {
  if (BASE3) {
    int b = slabByte(ply, rank / 5);
    for (u64 j = rank % 5; j; --j) b /= 3;
    return b % 3;
  }
  return (slabByte(ply, rank / 4) >> (2 * (rank % 4))) & 3;
}

// ---- the game ----
struct Pos {
  std::vector<std::vector<int>> g;     // g[c][r]: 0 empty, 1 first player (X), 2 second (O)
  std::vector<int> h;
  int ply = 0;
  Pos() : g(W, std::vector<int>(H, 0)), h(W, 0) {}
  bool fourAt(int c, int r) const {    // does the stone at (c, r) complete a four?
    int v = g[c][r];
    static const int D[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (auto& d : D) {
      int n = 1;
      for (int sgn = -1; sgn <= 1; sgn += 2)
        for (int k = 1;; ++k) {
          int cc = c + sgn * k * d[0], rr = r + sgn * k * d[1];
          if (cc < 0 || cc >= W || rr < 0 || rr >= H || g[cc][rr] != v) break;
          ++n;
        }
      if (n >= 4) return true;
    }
    return false;
  }
  bool play(int c) {                   // returns true if the move makes four
    int r = h[c]++;
    g[c][r] = ply % 2 == 0 ? 1 : 2;
    ++ply;
    return fourAt(c, r);
  }
  std::vector<int> states() const {
    std::vector<int> s(W);
    for (int c = 0; c < W; ++c) {
      int p = 0;
      for (int r = 0; r < h[c]; ++r) p |= (g[c][r] == 1) << r;
      s[c] = (1 << h[c]) - 1 + p;
    }
    return s;
  }
  int value() const { return valueAt(ply, rankOf(ply, states())); }   // for the side to move
};

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s DIR [MOVES]   e.g. %s c4_wdl_7x7 3323\n", argv[0], argv[0]);
    return 1;
  }
  DIR = argv[1];
  BASE3 = exists(DIR + "/PACK_BASE3");
  loadZdd(DIR);
  std::string moves = argc > 2 ? argv[2] : "-";
  Pos p;
  bool over = false;
  if (moves != "-")
    for (char ch : moves) {
      int c = ch - '0';
      if (over) die("the game is already over before move", std::string(1, ch));
      if (c < 0 || c >= W) die("bad column", std::string(1, ch));
      if (p.h[c] >= H) die("column is full:", std::string(1, ch));
      over = p.play(c);
    }
  const char* side = p.ply % 2 == 0 ? "first player (x)" : "second player (o)";
  printf("Connect Four %dx%d, after %d moves, %s to move\n", W, H, p.ply, side);
  for (int r = H - 1; r >= 0; --r) {
    printf("  ");
    for (int c = 0; c < W; ++c) printf("%c ", ".xo"[p.g[c][r]]);
    printf("\n");
  }
  printf("  ");
  for (int c = 0; c < W; ++c) printf("%d ", c);
  printf("\n");
  if (over) { printf("Game over: the last move made four in a row.\n"); return 0; }
  if (p.ply == CELLS) { printf("Game over: the board is full, a draw.\n"); return 0; }
  static const char* NAME[3] = {"draw", "win", "loss"};
  printf("Value for the side to move: %s\n", NAME[p.value()]);
  printf("Moves:\n");
  for (int c = 0; c < W; ++c) {
    if (p.h[c] >= H) { printf("  %d: column full\n", c); continue; }
    Pos q = p;
    if (q.play(c)) { printf("  %d: win (makes four)\n", c); continue; }
    if (q.ply == CELLS) { printf("  %d: draw (fills the board)\n", c); continue; }
    int v = q.value();                 // for the opponent
    printf("  %d: %s\n", c, NAME[v == 0 ? 0 : v == 1 ? 2 : 1]);
  }
  return 0;
}
