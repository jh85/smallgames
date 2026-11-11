// Standalone query for an Amazons WDL package written by `slabsolve reach`.
//
//   g++ -O2 -std=c++17 -mbmi2 -o wdl_query wdl_query.cpp -lzstd
//   ./wdl_query DIR BOARD [MOVE ...] [--moves]
//
// DIR is the package directory (config.txt, k*/c*.idx, k*/c*.tiles).  BOARD is the board
// to play on: the package's own WxH or its transpose HxW.  Moves are written as in Song and
// Mueller, from-to x arrow, columns a, b, ... from the left and rows 1, 2, ... from the first
// player's side: b1-b4xd4.  The program replays the moves from the standard start with the
// rules, rejects illegal ones, and prints the value for the side to move.  --moves also
// prints the value of every legal move.
//
// Values are exact for every position reachable from the standard start of either board.
// Layers that the package does not hold are answered by one ply of search.
//
// File format (all integers little-endian).  A position is (M, O, A): the queens of the side
// to move, the opponent's queens, the arrows, as cell masks with cell = row * W + column on
// the package's WxH board.  The bit stored for it is 1 if the side to move loses.
//   * Queen sets are reduced by the board symmetries (4, or 8 on a square board).  The
//     canonical set of an orbit is its smallest (canon min) or largest (canon max) mask.
//   * A layer k (k arrows) is split into D classes of canonical opponent sets O, by row index:
//     class(r) = r * D / rows.  Class c is the file k<k>/c<c>.tiles with index c<c>.idx.
//   * Within a class file, tile d holds, column by column (O in row order), the slabs of the
//     movers M whose canonical set is in class d, in increasing rank of M among the Q-subsets
//     of the cells outside O.
//   * A slab is the bit vector over the arrow sets of popcount k on the 2Q-free cells F,
//     indexed by colex rank of pext(A, F), padded to 64-bit words.
//   * A tile is cut into zstd frames of floor(16 MiB / slab bytes) slabs.
//   * The index: u64 magic "AMZMBLT1", W, H, Q, k, D, c, canon (1 = max), loss count, data
//     hash, data bytes, D + 1 tile offsets, then per tile the frame count and u32 frame
//     lengths, then a u64 hash.

#include <immintrin.h>
#include <zstd.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using u64 = uint64_t;
using u32 = uint32_t;

static int W, H, N, Q = 4, F, GN, D;
static bool canon_max = false;
static int perm[8][64];
static u64 binom[65][65];
static std::string dir;
static std::string answers;  // boards the package holds values for, if config.txt says
static std::vector<u64> rows;       // canonical sets, by row
static std::vector<u32> rowmap;     // rank of any Q-set -> row << 3 | g
static u64 SPR;

[[noreturn]] static void Fail(const std::string& m) {
  std::fprintf(stderr, "wdl_query: %s\n", m.c_str());
  std::exit(1);
}

static u64 Rank(u64 mask) {  // colex rank among sets of the same size
  u64 r = 0;
  int i = 1;
  for (; mask; mask &= mask - 1) r += binom[__builtin_ctzll(mask)][i++];
  return r;
}
static u64 Transform(int g, u64 b) {
  u64 r = 0;
  for (; b; b &= b - 1) r |= u64(1) << perm[g][__builtin_ctzll(b)];
  return r;
}
static u64 Board() { return (u64(1) << N) - 1; }

static void Init() {
  F = N - 2 * Q;
  for (int n = 0; n <= 64; ++n) {
    binom[n][0] = 1;
    for (int k = 1; k <= n; ++k) binom[n][k] = binom[n - 1][k - 1] + (k < n ? binom[n - 1][k] : 0);
  }
  GN = W == H ? 8 : 4;
  for (int g = 0; g < GN; ++g) {
    for (int c = 0; c < N; ++c) {
      int x = c % W, y = c / W;
      if (g & 4) std::swap(x, y);
      if (g & 1) x = W - 1 - x;
      if (g & 2) y = H - 1 - y;
      perm[g][c] = y * W + x;
    }
  }
  const u64 sets = binom[N][Q];
  SPR = binom[N - Q][Q];
  std::vector<u64> all(sets);
  u64 v = (u64(1) << Q) - 1;
  for (u64 i = 0; i < sets; ++i) {
    all[i] = v;
    const u64 t = v | (v - 1);
    if (i + 1 < sets) v = (t + 1) | (((~t & (t + 1)) - 1) >> (__builtin_ctzll(v) + 1));
  }
  auto better = [](u64 a, u64 b) { return canon_max ? a > b : a < b; };
  rowmap.assign(sets, 0);
  for (u64 i = 0; i < sets; ++i) {
    bool canonical = true;
    for (int g = 0; g < GN; ++g) canonical = canonical && !better(Transform(g, all[i]), all[i]);
    if (canonical) {
      rowmap[i] = u32(rows.size() << 3);
      rows.push_back(all[i]);
    }
  }
  for (u64 i = 0; i < sets; ++i) {
    u64 best = all[i];
    int bg = 0;
    for (int g = 0; g < GN; ++g) {
      const u64 t = Transform(g, all[i]);
      if (better(t, best)) best = t, bg = g;
    }
    if (best != all[i]) rowmap[i] = (rowmap[Rank(best)] & ~u32(7)) | u32(bg);
  }
}

static int ClassOf(u64 row) { return int(row * u64(D) / rows.size()); }
static int SetClass(u64 set) { return ClassOf(rowmap[Rank(set)] >> 3); }

// The jj-th Q-subset of 0..N-Q-1 in colex order.
static u64 Unrank(u64 jj) {
  u64 v = 0;
  for (int i = Q; i >= 1; --i) {
    int p = i - 1;
    while (binom[p + 1][i] <= jj) ++p;
    jj -= binom[p][i];
    v |= u64(1) << p;
  }
  return v;
}

// Slabs of column y in each tile.
static const std::vector<u64>& ColumnCounts(u64 y) {
  static std::map<u64, std::vector<u64>> memo;
  auto it = memo.find(y);
  if (it != memo.end()) return it->second;
  std::vector<u64> cnt(size_t(D), 0);
  const u64 rest = ~rows[y] & Board();
  for (u64 jj = 0; jj < SPR; ++jj) ++cnt[size_t(SetClass(_pdep_u64(Unrank(jj), rest)))];
  return memo.emplace(y, std::move(cnt)).first->second;
}

struct Index {
  std::vector<u64> tile_off;
  std::vector<std::vector<u32>> frames;
};

struct Layer {
  bool present = false;
  std::map<int, Index> idx;
  std::map<std::pair<int, u64>, std::vector<char>> cache;  // (class, frame offset) -> raw
};
static std::map<int, Layer> layers;

static bool LoadIndex(int k, int c, Index* out) {
  std::ifstream in(dir + "/k" + std::to_string(k) + "/c" + std::to_string(c) + ".idx", std::ios::binary);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string s = ss.str();
  size_t pos = 0;
  auto get = [&]() {
    if (pos + 8 > s.size()) Fail("truncated index");
    u64 v;
    std::memcpy(&v, s.data() + pos, 8);
    pos += 8;
    return v;
  };
  if (get() != 0x31544C424D5A4D41ULL) Fail("bad index magic");
  const u64 w = get(), h = get(), q = get(), kk = get(), dd = get(), cc = get();
  get();  // canon
  if (w != u64(W) || h != u64(H) || q != u64(Q) || kk != u64(k) || dd != u64(D) || cc != u64(c)) Fail("index does not match config.txt");
  get(), get(), get();  // loss count, data hash, data bytes
  out->tile_off.resize(size_t(D) + 1);
  for (auto& t : out->tile_off) t = get();
  out->frames.resize(size_t(D));
  for (auto& f : out->frames) {
    const u64 n = get();
    f.resize(n);
    std::memcpy(f.data(), s.data() + pos, n * 4);
    pos += n * 4;
  }
  return true;
}

static Layer* GetLayer(int k) {
  auto it = layers.find(k);
  if (it != layers.end()) return it->second.present ? &it->second : nullptr;
  Layer& l = layers[k];
  Index first;
  l.present = LoadIndex(k, 0, &first);
  if (l.present) l.idx[0] = first;
  return l.present ? &l : nullptr;
}

// 1 if the side to move loses (m, o, a), from layer popcount(a), which must be present.
static bool StoredLoss(Layer* l, int k, u64 m, u64 o, u64 a) {
  const u32 e = rowmap[Rank(o)];
  const int g = int(e & 7);
  const u64 col = e >> 3;
  const u64 gm = Transform(g, m), go = Transform(g, o), ga = Transform(g, a);
  const u64 free_cells = ~(gm | go) & Board();
  const u64 r = Rank(_pext_u64(ga, free_cells));
  const u64 j = Rank(_pext_u64(gm, ~go & Board()));
  const int c = ClassOf(col), d = SetClass(gm);
  // Place of the slab in tile d: slabs of earlier columns of class c, then earlier movers.
  const u64 rb = (u64(c) * rows.size() + u64(D) - 1) / u64(D);
  u64 slab = 0;
  for (u64 y = rb; y < col; ++y) slab += ColumnCounts(y)[size_t(d)];
  const u64 rest = ~rows[col] & Board();
  for (u64 jj = 0; jj < j; ++jj) {
    if (SetClass(_pdep_u64(Unrank(jj), rest)) == d) ++slab;
  }
  if (!l->idx.count(c)) {
    Index ix;
    if (!LoadIndex(k, c, &ix)) Fail("missing class file " + std::to_string(c) + " of layer " + std::to_string(k));
    l->idx[c] = ix;
  }
  const Index& ix = l->idx[c];
  const u64 stride = (binom[F][k] + 63) / 64, slab_bytes = stride * 8;
  const u64 chunk = std::max<u64>(1, (u64(16) << 20) / slab_bytes);
  const u64 frame = slab / chunk;
  u64 off = ix.tile_off[size_t(d)];
  for (u64 f = 0; f < frame; ++f) off += ix.frames[size_t(d)].at(f);
  auto key = std::make_pair(c, off);
  auto it = l->cache.find(key);
  if (it == l->cache.end()) {
    const u64 len = ix.frames[size_t(d)].at(frame);
    std::vector<char> comp(len);
    std::ifstream in(dir + "/k" + std::to_string(k) + "/c" + std::to_string(c) + ".tiles", std::ios::binary);
    in.seekg(std::streamoff(off));
    if (!in.read(comp.data(), std::streamsize(len))) Fail("short read");
    const unsigned long long raw = ZSTD_getFrameContentSize(comp.data(), len);
    if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN) Fail("bad zstd frame");
    std::vector<char> out(raw);
    const size_t got = ZSTD_decompress(out.data(), raw, comp.data(), len);
    if (ZSTD_isError(got) || got != raw) Fail("corrupt zstd frame");
    it = l->cache.emplace(key, std::move(out)).first;
  }
  const u64 w = (slab % chunk) * stride + (r >> 6);
  u64 word;
  std::memcpy(&word, it->second.data() + w * 8, 8);
  return (word >> (r & 63)) & 1;
}

struct Move {
  int f, t, s;
};
static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1}, DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};

static std::vector<Move> Moves(u64 m, u64 o, u64 a) {
  std::vector<Move> out;
  const u64 occ = m | o | a;
  for (u64 mm = m; mm; mm &= mm - 1) {
    const int f = __builtin_ctzll(mm);
    for (int d = 0; d < 8; ++d) {
      for (int x = f % W + DX[d], y = f / W + DY[d]; x >= 0 && x < W && y >= 0 && y < H; x += DX[d], y += DY[d]) {
        const int t = y * W + x;
        if ((occ >> t) & 1) break;
        const u64 occ2 = (occ & ~(u64(1) << f)) | (u64(1) << t);
        for (int d2 = 0; d2 < 8; ++d2) {
          for (int sx = x + DX[d2], sy = y + DY[d2]; sx >= 0 && sx < W && sy >= 0 && sy < H; sx += DX[d2], sy += DY[d2]) {
            const int s = sy * W + sx;
            if ((occ2 >> s) & 1) break;
            out.push_back({f, t, s});
          }
        }
      }
    }
  }
  return out;
}

static bool Loss(u64 m, u64 o, u64 a) {
  const int k = __builtin_popcountll(a);
  if (Layer* l = GetLayer(k)) return StoredLoss(l, k, m, o, a);
  if (k >= F) return true;
  if (!GetLayer(k + 1)) Fail("neither layer " + std::to_string(k) + " nor " + std::to_string(k + 1) + " is in the package");
  for (const Move& v : Moves(m, o, a)) {
    const u64 moved = (m & ~(u64(1) << v.f)) | (u64(1) << v.t);
    if (Loss(o, moved, a | (u64(1) << v.s))) return false;
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: wdl_query DIR BOARD [MOVE ...] [--moves]   e.g. wdl_query . 5x6 b1-b4xd4\n");
    return 2;
  }
  dir = argv[1];
  {
    std::ifstream in(dir + "/config.txt");
    if (!in) Fail("no config.txt in " + dir);
    std::string key, val;
    while (in >> key >> val) {
      if (key == "answers") answers = "," + val + ",";
      if (key == "board" && std::sscanf(val.c_str(), "%dx%d", &W, &H) != 2) Fail("bad board in config.txt");
      if (key == "classes") D = std::atoi(val.c_str());
      if (key == "canon") canon_max = val == "max";
      if (key == "queens") Q = std::atoi(val.c_str());
      std::getline(in, val);  // rest of the line
    }
  }
  N = W * H;
  if (Q != 4 || D <= 0) Fail("unsupported config.txt");
  int bw, bh;
  if (std::sscanf(argv[2], "%dx%d", &bw, &bh) != 2) Fail("bad board " + std::string(argv[2]));
  bool transposed;
  if (bw == W && bh == H) transposed = false;
  else if (bw == H && bh == W) transposed = true;
  else Fail("this package answers " + std::to_string(W) + "x" + std::to_string(H) + " and " + std::to_string(H) + "x" + std::to_string(W));
  if (!answers.empty() && answers.find("," + std::string(argv[2]) + ",") == std::string::npos) {
    Fail("this package answers " + answers.substr(1, answers.size() - 2) + " only");
  }
  Init();
  // A cell of the board being played (bw columns, bh rows), on the package's board.
  auto cell = [&](int x, int y) { return transposed ? x * W + y : y * W + x; };
  auto name = [&](int c) {
    const int x = transposed ? c / W : c % W, y = transposed ? c % W : c / W;
    return std::string(1, char('a' + x)) + std::to_string(y + 1);
  };
  auto parse_cell = [&](const std::string& s, size_t* pos) {
    if (*pos >= s.size() || !std::isalpha(static_cast<unsigned char>(s[*pos]))) Fail("bad move " + s);
    const int x = std::tolower(static_cast<unsigned char>(s[(*pos)++])) - 'a';
    int y = 0;
    if (*pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[*pos]))) Fail("bad move " + s);
    while (*pos < s.size() && std::isdigit(static_cast<unsigned char>(s[*pos]))) y = y * 10 + (s[(*pos)++] - '0');
    if (x < 0 || x >= bw || y < 1 || y > bh) Fail("cell off the board in " + s);
    return cell(x, y - 1);
  };
  // The standard start: (1,0), (W-2,0), (0,1), (W-1,1) and their mirror images.
  u64 m = 0, o = 0, a = 0;
  for (const auto& [x, y] : std::vector<std::pair<int, int>>{{1, 0}, {bw - 2, 0}, {0, 1}, {bw - 1, 1}}) {
    m |= u64(1) << cell(x, y);
    o |= u64(1) << cell(x, bh - 1 - y);
  }
  bool list = false, masks = false;
  int ply = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string s = argv[i];
    if (s == "--moves") {
      list = true;
      continue;
    }
    if (s == "--masks") {  // also print the position as cell lists on the package's board
      masks = true;
      continue;
    }
    size_t pos = 0;
    const int f = parse_cell(s, &pos);
    if (pos >= s.size() || s[pos++] != '-') Fail("bad move " + s);
    const int t = parse_cell(s, &pos);
    if (pos >= s.size() || (s[pos] != 'x' && s[pos] != 'X' && s[pos] != '/')) Fail("bad move " + s);
    ++pos;
    const int sh = parse_cell(s, &pos);
    bool legal = false;
    for (const Move& v : Moves(m, o, a)) legal = legal || (v.f == f && v.t == t && v.s == sh);
    if (!legal) Fail("illegal move " + s + " at ply " + std::to_string(ply + 1));
    const u64 moved = (m & ~(u64(1) << f)) | (u64(1) << t);
    a |= u64(1) << sh;
    m = o;
    o = moved;
    ++ply;
  }
  const bool loss = Loss(m, o, a);
  std::printf("%dx%d after %d moves: %s to move %s\n", bw, bh, ply, ply % 2 ? "second player" : "first player", loss ? "loses" : "wins");
  if (masks) {
    auto cells = [](u64 b) {
      std::string r;
      for (; b; b &= b - 1) r += (r.empty() ? "" : ",") + std::to_string(__builtin_ctzll(b));
      return r;
    };
    std::printf("masks %s %s %s\n", cells(m).c_str(), cells(o).c_str(), cells(a).c_str());
  }
  if (list) {
    for (const Move& v : Moves(m, o, a)) {
      const u64 moved = (m & ~(u64(1) << v.f)) | (u64(1) << v.t);
      std::printf("%s-%sx%s: %s\n", name(v.f).c_str(), name(v.t).c_str(), name(v.s).c_str(), Loss(o, moved, a | (u64(1) << v.s)) ? "wins" : "loses");
    }
  }
  return 0;
}
