// Reader for gM-only block-compressed layer files (.gmz); see gmz.cpp for the
// format.  GmzTable memory-maps the files of one table directory on demand;
// GmzReader is the per-thread decompression cache (one block per layer).
//
// gN is not stored: gN(b) = clamp_n( min over legal moves of
//   cap > 0 ? gM(child, layer n - cap) : gM(child, layer n) ), or the
// opponent's row sum when the mover has no legal move.  The child's clamp
// interval [n' - W, W] contains the parent's [n - W, W] (n' <= n), so taking
// the minimum of clamped child values and re-clamping gives exactly the code
// the .lh files store.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "oware.hpp"

namespace oware {

struct GzHeader {
  char magic[8];  // "OWAREGZ1"
  int32_t n, A, V, seeds, blockBoards, pack, level, w;
  uint64_t size, nblocks, offsetsPos;
};

inline std::string gmzPath(const std::string& dir, int n) {
  char buf[64]; snprintf(buf, sizeof buf, "/oware_n%02d.gmz", n);
  return dir + buf;
}

struct GmzLayer {
  GzHeader h{};
  const uint8_t* base = nullptr;
  size_t bytes = 0;
  const uint64_t* offs = nullptr;
  bool ok() const { return base != nullptr; }
};

struct GmzTable {
  std::string dir;
  GmzLayer L[MAX_SEEDS + 1];
  std::vector<uint8_t> full[MAX_SEEDS + 1];  // whole layer decoded (loadAll), one code per board
  bool open(const std::string& d) { dir = d; return true; }
  // Decode all of layer n into memory (for bulk verification / solving).
  bool loadAll(int n) {
    const GmzLayer* g = layer(n);
    if (!g) return false;
    if (!full[n].empty()) return true;
    full[n].resize(g->h.size);
    size_t rawBytes = g->h.pack ? (size_t(g->h.blockBoards) * g->h.w + 7) / 8 + 2 : size_t(g->h.blockBoards);
    std::vector<uint8_t> raw(rawBytes);
    for (uint64_t b = 0; b < g->h.nblocks; ++b) {
      size_t r = ZSTD_decompress(raw.data(), raw.size(), g->base + g->offs[b], g->offs[b + 1] - g->offs[b]);
      if (ZSTD_isError(r)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r)); return false; }
      uint64_t i0 = b * uint64_t(g->h.blockBoards), i1 = std::min<uint64_t>(i0 + g->h.blockBoards, g->h.size);
      for (uint64_t i = i0; i < i1; ++i) {
        uint64_t j = i - i0;
        if (!g->h.pack) full[n][i] = raw[j];
        else { uint64_t bit = j * uint64_t(g->h.w); uint16_t v; memcpy(&v, &raw[bit >> 3], 2); full[n][i] = uint8_t((v >> (bit & 7)) & ((1 << g->h.w) - 1)); }
      }
    }
    return true;
  }
  // Map layer n (returns nullptr if the file is missing or wrong).
  const GmzLayer* layer(int n) {
    GmzLayer& g = L[n];
    if (g.ok()) return &g;
    std::string path = gmzPath(dir, n);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cannot open %s\n", path.c_str()); return nullptr; }
    GzHeader h;
    if (read(fd, &h, sizeof h) != (ssize_t)sizeof h || memcmp(h.magic, "OWAREGZ1", 8) || h.n != n || h.seeds != SEEDS) {
      fprintf(stderr, "%s is not a layer %d file of the %d-seed game\n", path.c_str(), n, SEEDS); ::close(fd); return nullptr;
    }
    size_t bytes = h.offsetsPos + 8 * (h.nblocks + 1);
    void* p = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return nullptr; }
    g.h = h; g.base = static_cast<const uint8_t*>(p); g.bytes = bytes;
    g.offs = reinterpret_cast<const uint64_t*>(g.base + h.offsetsPos);
    return &g;
  }
};

struct GmzReader {
  GmzTable& T;
  struct Cache { int n = -1; uint64_t block = ~0ull; std::vector<uint8_t> raw; };
  Cache C[MAX_SEEDS + 1];
  ZSTD_DCtx* dctx;
  explicit GmzReader(GmzTable& t) : T(t), dctx(ZSTD_createDCtx()) {}
  ~GmzReader() { ZSTD_freeDCtx(dctx); }
  GmzReader(const GmzReader&) = delete;

  // Clamped gM code of board index i in layer n, in seeds (A + code).
  int gM(int n, uint64_t i) {
    const GmzLayer* g = T.layer(n);
    if (!g) exit(1);
    if (!T.full[n].empty()) return g->h.A + T.full[n][i];
    uint64_t b = i / uint64_t(g->h.blockBoards);
    Cache& c = C[n];
    if (c.block != b) {
      size_t rawBytes = g->h.pack ? (size_t(g->h.blockBoards) * g->h.w + 7) / 8 + 2 : size_t(g->h.blockBoards);
      c.raw.assign(rawBytes, 0);
      size_t r = ZSTD_decompressDCtx(dctx, c.raw.data(), c.raw.size(), g->base + g->offs[b], g->offs[b + 1] - g->offs[b]);
      if (ZSTD_isError(r)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r)); exit(1); }
      c.block = b;
    }
    uint64_t j = i - b * uint64_t(g->h.blockBoards);
    int code;
    if (!g->h.pack) code = c.raw[j];
    else {
      uint64_t bit = j * uint64_t(g->h.w);
      uint16_t v; memcpy(&v, &c.raw[bit >> 3], 2);
      code = (v >> (bit & 7)) & ((1 << g->h.w) - 1);
    }
    return g->h.A + code;
  }
  // Clamp a value in seeds to layer n's code range, returned in seeds.
  static int clampSeeds(int n, int g) {
    int A = n >= winSeeds() ? n - winSeeds() : 0;
    int V = n >= winSeeds() ? SEEDS + 3 - n : n + 1;
    return std::clamp(g, A, A + V - 1);
  }
  // gN of an indexed board b of layer n, derived from the children's gM.
  int gN(const Board& b, int n, const Index& IX) {
    int mask = legalMask(b);
    if (!mask) return clampSeeds(n, b.rowSum(ROW));
    int g = 1 << 30;
    for (int m = 0; m < ROW; ++m) if (mask >> m & 1) {
      Board c; int cap = play(b, m, c);
      g = std::min(g, gM(n - cap, IX.rank(c, n - cap)));
    }
    return clampSeeds(n, g);
  }
};

}  // namespace oware
