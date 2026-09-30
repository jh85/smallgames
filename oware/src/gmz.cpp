// gmz: gM-only, block-compressed Oware layer files (.gmz) and their tools.
//
// The .lh layer files store the pair (gM, gN) per board.  gN is redundant for
// queries: gN(b) = min over the legal moves of gM(child) (the child of a
// capturing move lies in a lower layer), or the opponent's row sum when the
// mover has no legal move.  A .gmz file therefore stores only the clamped gM
// code, in blocks of `blockBoards` boards compressed independently with zstd,
// followed by the block offset table, so that a probe decompresses one block.
//
//   gmz pack   <lhdir> <seeds> <n> <outdir> [level=19] [blockBoards=65536] [pack=0|1] [threads]
//   gmz verify <lhdir> <gmzdir> <seeds> <n> [threads]
//        every board: gM(gmz) == gM(lh) and gN derived from gmz == gN(lh)
//   gmz stats  <lhdir> <seeds> <n>          entropy of the gM codes (bits/board)
//
// pack=0 stores one byte per board inside a block (zstd's entropy coder gets
// close to the code entropy anyway); pack=1 stores w-bit fields.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "gmz.hpp"
#include "layer.hpp"
#include "oware.hpp"

using namespace oware;

static Index IX;
static Layer LH[MAX_SEEDS + 1];
static std::string LHDIR;

static const Layer& lh(int n) {
  Layer& ly = LH[n];
  if (ly.data) return ly;
  ly.describe(n, IX.layerSize(n));
  std::string path = layerPath(LHDIR, n);
  int fd = open(path.c_str(), O_RDONLY);
  FileHeader h;
  if (fd < 0 || read(fd, &h, sizeof h) != sizeof h || memcmp(h.magic, "OWARELH1", 8) || h.n != n ||
      !headerSeedsMatch(h) || h.size != ly.size) {
    fprintf(stderr, "cannot use %s for a %d-seed game\n", path.c_str(), SEEDS); exit(1);
  }
  ly.mode = h.mode; ly.bytes = h.bytes;
  void* p = mmap(nullptr, sizeof h + h.bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) { perror("mmap"); exit(1); }
  close(fd);
  ly.data = static_cast<uint8_t*>(p) + sizeof h;
  return ly;
}

static double now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static int cmdStats(int n) {
  const Layer& ly = lh(n);
  std::vector<uint64_t> cnt(ly.V, 0), cntN(ly.V, 0);
  for (uint64_t i = 0; i < ly.size; ++i) { int p = ly.get(i); ++cnt[ly.decM[p]]; ++cntN[ly.decN[p]]; }
  double H = 0, HN = 0;
  for (int v = 0; v < ly.V; ++v) {
    if (cnt[v]) { double q = double(cnt[v]) / double(ly.size); H -= q * std::log2(q); }
    if (cntN[v]) { double q = double(cntN[v]) / double(ly.size); HN -= q * std::log2(q); }
  }
  printf("layer %d: %" PRIu64 " boards V=%d P=%d; table %.3f bytes/board; H(gM)=%.3f bits (%.3f B/board), H(gN)=%.3f bits\n",
         n, ly.size, ly.V, ly.P, double(ly.bytes) / double(ly.size), H, H / 8, HN);
  printf("  gM code histogram:"); for (int v = 0; v < ly.V; ++v) printf(" %d:%.4f", v, double(cnt[v]) / double(ly.size)); printf("\n");
  return 0;
}

static int cmdPack(int n, const std::string& outdir, int level, uint32_t blockBoards, int pack, int threads) {
  const Layer& ly = lh(n);
  double t0 = now();
  uint64_t nblocks = (ly.size + blockBoards - 1) / blockBoards;
  int w = 1; while ((1 << w) < ly.V) ++w;
  size_t rawBytes = pack ? (size_t(blockBoards) * w + 7) / 8 + 2 : blockBoards;
  std::vector<std::vector<uint8_t>> out(nblocks);
  std::atomic<uint64_t> next{0};
  auto worker = [&]() {
    std::vector<uint8_t> raw(rawBytes);
    std::vector<uint8_t> comp(ZSTD_compressBound(rawBytes));
    for (;;) {
      uint64_t b = next.fetch_add(1);
      if (b >= nblocks) return;
      uint64_t i0 = b * blockBoards, i1 = std::min<uint64_t>(i0 + blockBoards, ly.size);
      std::fill(raw.begin(), raw.end(), 0);
      for (uint64_t i = i0; i < i1; ++i) {
        int c = ly.decM[ly.get(i)];
        if (!pack) raw[i - i0] = uint8_t(c);
        else {
          uint64_t bit = (i - i0) * uint64_t(w);
          uint16_t v; memcpy(&v, &raw[bit >> 3], 2);
          v = uint16_t(v | (unsigned(c) << (bit & 7)));
          memcpy(&raw[bit >> 3], &v, 2);
        }
      }
      size_t used = pack ? ((i1 - i0) * w + 7) / 8 : (i1 - i0);
      size_t cs = ZSTD_compress(comp.data(), comp.size(), raw.data(), used, level);
      if (ZSTD_isError(cs)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(cs)); exit(1); }
      out[b].assign(comp.begin(), comp.begin() + cs);
    }
  };
  std::vector<std::thread> th;
  for (int i = 0; i < threads; ++i) th.emplace_back(worker);
  for (auto& t : th) t.join();

  GzHeader h{};
  memcpy(h.magic, "OWAREGZ1", 8);
  h.n = n; h.A = ly.A; h.V = ly.V; h.seeds = SEEDS; h.blockBoards = int32_t(blockBoards); h.pack = pack; h.level = level; h.w = w;
  h.size = ly.size; h.nblocks = nblocks;
  std::vector<uint64_t> offs(nblocks + 1);
  uint64_t pos = sizeof h;
  for (uint64_t b = 0; b < nblocks; ++b) { offs[b] = pos; pos += out[b].size(); }
  offs[nblocks] = pos;
  h.offsetsPos = pos;
  std::string path = gmzPath(outdir, n);
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) { perror(path.c_str()); return 1; }
  fwrite(&h, sizeof h, 1, f);
  for (uint64_t b = 0; b < nblocks; ++b) fwrite(out[b].data(), 1, out[b].size(), f);
  fwrite(offs.data(), 8, nblocks + 1, f);
  fclose(f);
  uint64_t total = pos + 8 * (nblocks + 1);
  printf("layer %d: %" PRIu64 " boards -> %s: %" PRIu64 " bytes (%.4f B/board, table %.4f B/board, ratio %.2fx) level %d block %u pack %d in %.1fs\n",
         n, ly.size, path.c_str(), total, double(total) / double(ly.size), double(ly.bytes) / double(ly.size),
         double(ly.bytes) / double(total), level, blockBoards, pack, now() - t0);
  return 0;
}

static int cmdVerify(int n, const std::string& gmzdir, int threads) {
  const Layer& ly = lh(n);
  for (int m = 0; m < n; ++m) if (m != SEEDS - 1) lh(m);
  GmzTable T;
  if (!T.open(gmzdir)) return 1;
  double t0 = now();
  for (int m = 0; m <= n; ++m) if (m != SEEDS - 1) if (!T.loadAll(m)) return 1;
  printf("layer %d: decoded layers 0..%d in %.1fs\n", n, n, now() - t0);
  std::atomic<uint64_t> badM{0}, badN{0}, done{0};
  std::vector<std::thread> th;
  for (int ti = 0; ti < threads; ++ti) th.emplace_back([&, ti]() {
    GmzReader R(T);
    uint64_t i0 = ly.size * ti / threads, i1 = ly.size * (ti + 1) / threads;
    for (uint64_t i = i0; i < i1; ++i) {
      int p = ly.get(i);
      Board b = IX.unrank(i, n);
      int gm = R.gM(n, i);
      if (gm != ly.gM(p)) { badM.fetch_add(1); }
      int gn = R.gN(b, n, IX);
      if (gn != ly.gN(p)) { badN.fetch_add(1); }
    }
    done.fetch_add(i1 - i0);
  });
  for (auto& t : th) t.join();
  printf("layer %d: %" PRIu64 " boards verified in %.1fs: gM mismatches %" PRIu64 ", derived-gN mismatches %" PRIu64 "%s\n",
         n, ly.size, now() - t0, badM.load(), badN.load(), badM.load() + badN.load() ? "  FAILED" : "  OK");
  return badM.load() + badN.load() ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: gmz pack <lhdir> <seeds> <n> <outdir> [level] [blockBoards] [pack] [threads]\n"
                    "       gmz verify <lhdir> <gmzdir> <seeds> <n> [threads]\n"
                    "       gmz stats <lhdir> <seeds> <n>\n");
    return 1;
  }
  std::string cmd = argv[1];
  LHDIR = argv[2];
  if (cmd == "stats") { SEEDS = atoi(argv[3]); return cmdStats(atoi(argv[4])); }
  if (cmd == "pack") {
    if (argc < 6) { fprintf(stderr, "pack needs <outdir>\n"); return 1; }
    SEEDS = atoi(argv[3]);
    int n = atoi(argv[4]);
    int level = argc > 6 ? atoi(argv[6]) : 19;
    uint32_t block = argc > 7 ? uint32_t(atoi(argv[7])) : 65536;
    int pack = argc > 8 ? atoi(argv[8]) : 0;
    int threads = argc > 9 ? atoi(argv[9]) : 16;
    return cmdPack(n, argv[5], level, block, pack, threads);
  }
  if (cmd == "verify") {
    if (argc < 6) { fprintf(stderr, "verify needs <gmzdir> <seeds> <n>\n"); return 1; }
    SEEDS = atoi(argv[4]);
    int threads = argc > 6 ? atoi(argv[6]) : 16;
    return cmdVerify(atoi(argv[5]), argv[3], threads);
  }
  fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 1;
}
