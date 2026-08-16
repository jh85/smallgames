// Exhaustive internal-consistency check of a wdlretro table:
// for every position in layers 0..maxlayer, re-derive the verdict from the
// child tables (W iff some child is L, L iff all children are W, no-move = L)
// and compare against the stored 2-bit verdict.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "board.h"
using namespace amazons;

int g_w, g_h, g_nsq, g_parts;
std::string g_dir;

struct SymTables { uint64_t m[8][4][256]; } g_sym;

void BuildSymTables() {
  const bool square = (g_w == g_h);
  std::memset(&g_sym, 0, sizeof g_sym);
  for (int s = 0; s < 8; s++) {
    const bool swap_axes = square && (s & 4);
    const bool flip_x = s & 1, flip_y = s & 2;
    for (int j = 0; j < 4; j++)
      for (int v = 0; v < 256; v++) {
        uint64_t word = 0;
        for (int b = 0; b < 8; b++) {
          if (!((v >> b) & 1)) continue;
          const int sq = j * 8 + b;
          if (sq >= g_nsq) continue;
          int x = sq % g_w, y = sq / g_w;
          if (swap_axes) std::swap(x, y);
          if (flip_x) x = g_w - 1 - x;
          if (flip_y) y = g_h - 1 - y;
          word |= uint64_t{1} << (y * g_w + x);
        }
        g_sym.m[s][j][v] = word;
      }
  }
}
inline uint64_t MapBB(int s, uint64_t bb) {
  return g_sym.m[s][0][bb & 0xFF] | g_sym.m[s][1][(bb >> 8) & 0xFF] |
         g_sym.m[s][2][(bb >> 16) & 0xFF] | g_sym.m[s][3][(bb >> 24) & 0xFF];
}
inline uint64_t Spread(uint64_t v) {
  v = (v | (v << 16)) & 0x0000FFFF0000FFFFull;
  v = (v | (v << 8)) & 0x00FF00FF00FF00FFull;
  v = (v | (v << 4)) & 0x0F0F0F0F0F0F0F0Full;
  v = (v | (v << 2)) & 0x3333333333333333ull;
  v = (v | (v << 1)) & 0x5555555555555555ull;
  return v;
}
inline uint64_t CanonKey(uint64_t wq, uint64_t bq, uint64_t burn) {
  const int nsym = (g_w == g_h) ? 8 : 4;
  uint64_t bw = ~0ull, bb = 0, bx = 0;
  for (int s = 0; s < nsym; s++) {
    const uint64_t w2 = MapBB(s, wq);
    if (w2 > bw) continue;
    const uint64_t b2 = MapBB(s, bq);
    if (w2 == bw && b2 > bb) continue;
    const uint64_t x2 = MapBB(s, burn);
    if (w2 == bw && b2 == bb && x2 >= bx) continue;
    bw = w2; bb = b2; bx = x2;
  }
  return Spread(bw) + 2 * Spread(bb) + 3 * Spread(bx);
}
inline uint64_t ChildKey(const Position& p, Move m) {
  Position c = p;
  c.DoMove(m);
  return CanonKey(c.queens[kBlack], c.queens[kWhite], c.burned);
}
Position Decode(uint64_t k) {
  Position p;
  p.w = g_w; p.h = g_h; p.stm = kWhite;
  for (int sq = 0; sq < g_nsq; sq++, k /= 4) {
    const uint64_t b = uint64_t{1} << sq;
    switch (k % 4) {
      case 1: p.queens[kWhite] |= b; break;
      case 2: p.queens[kBlack] |= b; break;
      case 3: p.burned |= b; break;
      default: break;
    }
  }
  return p;
}
uint64_t KeyHash(uint64_t k) { k *= 0x9E3779B97F4A7C15ull; return k ^ (k >> 29); }
int Part(uint64_t key) { return int(KeyHash(key) & uint64_t(g_parts - 1)); }

std::string Path(const char* kind, int layer, int q) {
  char buf[256];
  std::snprintf(buf, sizeof buf, "%s/%s%02d.q%02d.%s", g_dir.c_str(), kind,
                layer, q, kind[0] == 'l' ? "keys" : "bits");
  return buf;
}
uint64_t FileSize(const std::string& p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0) return 0;
  return uint64_t(st.st_size);
}

// Flat open-addressed probe table: slot = key | verdict<<60.
struct Probe {
  std::vector<uint64_t> slots;
  size_t mask;
  void Init(uint64_t n) {
    size_t cap = 1 << 14;
    while (cap < n * 2) cap *= 2;
    slots.assign(cap, 0);
    mask = cap - 1;
  }
  void Insert(uint64_t kv) {
    size_t i = KeyHash(kv) & mask;
    while (slots[i]) i = (i + 1) & mask;
    slots[i] = kv;
  }
  uint8_t Get(uint64_t key) const {
    const uint64_t kmask = (uint64_t{1} << 60) - 1;
    size_t i = KeyHash(key) & mask;
    while (slots[i]) {
      if ((slots[i] & kmask) == key) return uint8_t(slots[i] >> 60);
      i = (i + 1) & mask;
    }
    return 0;
  }
};

void LoadProbe(int layer, Probe* pr) {
  uint64_t total = 0;
  for (int q = 0; q < g_parts; q++) total += FileSize(Path("layer", layer, q)) / 8;
  pr->Init(total + 16);
  for (int q = 0; q < g_parts; q++) {
    const std::string kp = Path("layer", layer, q), bp = Path("wdl", layer, q);
    const uint64_t n = FileSize(kp) / 8;
    int kfd = ::open(kp.c_str(), O_RDONLY), bfd = ::open(bp.c_str(), O_RDONLY);
    if (kfd < 0 || bfd < 0) { fprintf(stderr, "missing files layer %d\n", layer); exit(1); }
    std::vector<uint64_t> kb(1 << 20);
    std::vector<uint8_t> bb((1 << 20) / 4 + 1);
    for (uint64_t off = 0; off < n;) {
      const uint64_t cnt = std::min<uint64_t>(1 << 20, n - off);
      if (::pread(kfd, kb.data(), cnt * 8, off * 8) != ssize_t(cnt * 8)) { perror("pread"); exit(1); }
      const uint64_t nbytes = (cnt + 3) / 4;
      if (::pread(bfd, bb.data(), nbytes, off / 4) != ssize_t(nbytes)) { perror("pread"); exit(1); }
      for (uint64_t i = 0; i < cnt; i++) {
        const uint8_t v = uint8_t((bb[i / 4] >> ((i % 4) * 2)) & 3);
        pr->Insert(kb[i] | (uint64_t{v} << 60));
      }
      off += cnt;
    }
    ::close(kfd); ::close(bfd);
  }
  fprintf(stderr, "probe layer %d: %" PRIu64 " positions loaded\n", layer, total);
}

int main(int argc, char** argv) {
  if (argc < 6) { fprintf(stderr, "usage: wdlcheck W H dir firstlayer lastlayer\n"); return 1; }
  g_w = atoi(argv[1]); g_h = atoi(argv[2]); g_nsq = g_w * g_h;
  g_dir = argv[3];
  const int minlayer = atoi(argv[4]); const int maxlayer = atoi(argv[5]);
  g_parts = 2;
  BuildSymTables();

  for (int k = minlayer; k <= maxlayer; k++) {
    // child probe (unless layer k+1 does not exist: deepest layer)
    Probe pr;
    const bool has_children = FileSize(Path("layer", k + 1, 0)) + FileSize(Path("layer", k + 1, 1)) > 0
                              && k + 1 <= 12 && FileSize(Path("wdl", k + 1, 0)) > 0;
    if (has_children) LoadProbe(k + 1, &pr);

    uint64_t bad = 0, checked = 0, nw = 0, nl = 0;
    for (int q = 0; q < g_parts; q++) {
      const std::string kp = Path("layer", k, q), bp = Path("wdl", k, q);
      const uint64_t n = FileSize(kp) / 8;
      if (!n) continue;
      int kfd = ::open(kp.c_str(), O_RDONLY), bfd = ::open(bp.c_str(), O_RDONLY);
      std::vector<uint64_t> kb(1 << 20);
      std::vector<uint8_t> bb((1 << 20) / 4 + 1);
      std::vector<Move> moves;
      for (uint64_t off = 0; off < n;) {
        const uint64_t cnt = std::min<uint64_t>(1 << 20, n - off);
        if (::pread(kfd, kb.data(), cnt * 8, off * 8) != ssize_t(cnt * 8)) { perror("pread"); exit(1); }
        const uint64_t nbytes = (cnt + 3) / 4;
        if (::pread(bfd, bb.data(), nbytes, off / 4) != ssize_t(nbytes)) { perror("pread"); exit(1); }
        for (uint64_t i = 0; i < cnt; i++) {
          const uint8_t stored = uint8_t((bb[i / 4] >> ((i % 4) * 2)) & 3);
          Position p = Decode(kb[i]);
          p.GenerateMoves(&moves);
          uint8_t derived;
          if (!has_children) {
            derived = moves.empty() ? 2 : 0;  // deepest layer must be terminal
          } else if (moves.empty()) {
            derived = 2;
          } else {
            bool seen_l = false, seen_not_w = false, missing = false;
            for (Move m : moves) {
              const uint8_t v = pr.Get(ChildKey(p, m));
              if (v == 0) { missing = true; break; }
              if (v == 2) seen_l = true; else seen_not_w = true;
            }
            derived = missing ? 3 : (seen_l ? 1 : (seen_not_w ? 0 : 2));
            // 0 here means "all children W" => L
            if (derived == 0) derived = 2;
          }
          checked++;
          (stored == 1 ? nw : nl)++;
          if (derived != stored && bad++ < 3) {
            fprintf(stderr, "MISMATCH layer %d part %d idx %" PRIu64
                    " stored=%d derived=%d (outdeg=%zu)\n%s",
                    k, q, off + i, stored, derived, moves.size(),
                    p.ToString().c_str());
          } else if (derived != stored) {
            bad++;
          }
        }
        off += cnt;
      }
      ::close(kfd); ::close(bfd);
    }
    printf("layer %d: checked=%" PRIu64 " W=%" PRIu64 " L=%" PRIu64 " mismatches=%" PRIu64 "\n",
           k, checked, nw, nl, bad);
    fflush(stdout);
  }
  return 0;
}
