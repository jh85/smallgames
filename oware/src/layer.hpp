// Per-layer value storage for the Oware solver.
//
// For every indexed board with n seeds we keep two sound guarantees, in seeds out of
// the n still on the board:
//   gM = what the side to move can force,   gN = what the other side can force,
// both achieved by play that ends (capture to 25+, or a no-move sweep) without ever
// relying on how a repetition would be adjudicated.  gM + gN <= n; equality means
// the board's score is exact.
//
// Only thresholds that decide some legal score split matter for win/draw/loss, so
// for n > 24 the guarantees are clamped to [A, A+V-1] with A = n-25, V = 51-n
// (code 0 = "at most A", code V-1 = "at least 25").  For n <= 24 they are stored
// raw (A = 0, V = n+1).  A pair of codes (eM, eN) always satisfies eM+eN <= V-1 and
// is stored as one triangular index p in [0, P), P = V(V+1)/2.
#pragma once
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace oware {

enum Mode : int { RADIX = 0, BITS = 1, U16 = 2 };

struct Layer {
  int n = -1, A = 0, V = 0, P = 0;
  int mode = RADIX, G = 1, w = 8;  // G boards per byte (RADIX), w bits per board (BITS)
  uint64_t size = 0, bytes = 0;
  uint8_t* data = nullptr;
  uint16_t enc[28][28];
  uint8_t decM[400], decN[400];
  uint8_t digit[3][256];  // RADIX: digit[j][byte]
  int pw[3];

  #ifdef NOCLIP
  static int lowA(int) { return 0; }
  static int numV(int n) { return n + 1; }
#else
  static int lowA(int n) { return n > 24 ? n - 25 : 0; }
  static int numV(int n) { return n > 24 ? 51 - n : n + 1; }
#endif

  void describe(int n_, uint64_t size_) {
    n = n_; size = size_; A = lowA(n); V = numV(n); P = V * (V + 1) / 2;
    int p = 0;
    memset(enc, 0xff, sizeof enc);
    for (int s = 0; s < V; ++s)          // order by eM+eN so that (0,0) -> 0
      for (int m = 0; m <= s; ++m) { enc[m][s - m] = uint16_t(p); decM[p] = uint8_t(m); decN[p] = uint8_t(s - m); ++p; }
    G = P * P * P <= 256 ? 3 : P * P <= 256 ? 2 : 1;
    pw[0] = 1; pw[1] = P; pw[2] = P * P;
    for (int v = 0; v < 256; ++v) for (int j = 0; j < 3; ++j) digit[j][v] = uint8_t(v / pw[j] % P);
    w = 1; while ((1 << w) < P) ++w;
  }
  static uint64_t bytesFor(int mode, int G, int w, uint64_t size) {
    if (mode == U16) return 2 * size;
    if (mode == RADIX) return (size + G - 1) / G;
    return (size * w + 7) / 8 + 2;
  }
  int workMode() const { return P > 256 ? U16 : RADIX; }
  int finalMode() const { return P > 256 ? U16 : (G == 1 && w < 8) ? BITS : RADIX; }

  void alloc(int m) {
    mode = m;
    bytes = bytesFor(mode, G, w, size);
    void* p = mmap(nullptr, bytes ? bytes : 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(p, bytes, MADV_HUGEPAGE);
    data = static_cast<uint8_t*>(p);
  }
  void release() { if (data) munmap(data, bytes ? bytes : 1); data = nullptr; }

  inline int get(uint64_t i) const {
    switch (mode) {
      case RADIX:
        if (G == 1) return data[i];
        if (G == 2) return digit[i & 1][data[i >> 1]];
        return digit[i % 3][data[i / 3]];
      case BITS: {
        uint64_t bit = i * uint64_t(w);
        uint16_t v; memcpy(&v, data + (bit >> 3), 2);
        return (v >> (bit & 7)) & ((1 << w) - 1);
      }
      default: { uint16_t v; memcpy(&v, data + 2 * i, 2); return v; }
    }
  }
  // Only the thread owning board i's storage unit may call set().
  inline void set(uint64_t i, int oldp, int newp) {
    switch (mode) {
      case RADIX:
        if (G == 1) data[i] = uint8_t(newp);
        else if (G == 2) data[i >> 1] = uint8_t(data[i >> 1] + (newp - oldp) * pw[i & 1]);
        else data[i / 3] = uint8_t(data[i / 3] + (newp - oldp) * pw[i % 3]);
        break;
      case BITS: {
        uint64_t bit = i * uint64_t(w);
        uint16_t v; memcpy(&v, data + (bit >> 3), 2);
        v = uint16_t((v & ~(((1u << w) - 1) << (bit & 7))) | (unsigned(newp) << (bit & 7)));
        memcpy(data + (bit >> 3), &v, 2);
        break;
      }
      default: { uint16_t v = uint16_t(newp); memcpy(data + 2 * i, &v, 2); }
    }
  }
  // Guarantees in seeds (lower bounds; code 0 for n > 24 only says "<= A").
  int gM(int p) const { return A + decM[p]; }
  int gN(int p) const { return A + decN[p]; }
  int clampCode(int g) const { int e = g - A; return e < 0 ? 0 : e >= V ? V - 1 : e; }
};

struct FileHeader {
  char magic[8];  // "OWARELH1"
  int32_t n, A, V, P, mode, G, w, pad;
  uint64_t size, bytes;
};

inline std::string layerPath(const std::string& dir, int n) {
  char buf[64]; snprintf(buf, sizeof buf, "/oware_n%02d.lh", n);
  return dir + buf;
}

}  // namespace oware
