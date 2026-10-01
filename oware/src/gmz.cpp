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
//   gmz verify <lhdir> <gmzdir> <seeds> <n> [threads] [sample=1e7]
//        gM(gmz) == gM(lh) for every board; gN derived from gmz == gN(lh) for
//        every board when the layers a probe can reach fit in 64 GB decoded,
//        otherwise for `sample` random boards (0 = force every board)
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
#include <random>
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
  GzHeader h{};
  memcpy(h.magic, "OWAREGZ1", 8);
  h.n = n; h.A = ly.A; h.V = ly.V; h.seeds = SEEDS; h.blockBoards = int32_t(blockBoards); h.pack = pack; h.level = level; h.w = w;
  h.size = ly.size; h.nblocks = nblocks;
  std::string path = gmzPath(outdir, n), tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) { perror(tmp.c_str()); return 1; }
  fwrite(&h, sizeof h, 1, f);
  std::vector<uint64_t> offs(nblocks + 1);
  uint64_t pos = sizeof h;
  // Blocks are compressed in windows of WIN blocks (in parallel) and written in
  // order, so a layer never needs more than the window in memory and the .lh
  // file is read sequentially.
  const uint64_t WIN = uint64_t(threads) * 16;
  std::vector<std::vector<uint8_t>> out(WIN);
  for (uint64_t w0 = 0; w0 < nblocks; w0 += WIN) {
    uint64_t w1 = std::min(nblocks, w0 + WIN);
    std::atomic<uint64_t> next{w0};
    auto worker = [&]() {
      std::vector<uint8_t> raw(rawBytes);
      std::vector<uint8_t> comp(ZSTD_compressBound(rawBytes));
      for (;;) {
        uint64_t b = next.fetch_add(1);
        if (b >= w1) return;
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
        out[b - w0].assign(comp.begin(), comp.begin() + cs);
      }
    };
    std::vector<std::thread> th;
    for (int i = 0; i < threads; ++i) th.emplace_back(worker);
    for (auto& t : th) t.join();
    for (uint64_t b = w0; b < w1; ++b) {
      offs[b] = pos;
      if (fwrite(out[b - w0].data(), 1, out[b - w0].size(), f) != out[b - w0].size()) { perror("write"); return 1; }
      pos += out[b - w0].size();
    }
    if (ly.size > (1ull << 33) && (w0 / WIN) % 200 == 0) {
      fprintf(stderr, "  layer %d: %5.1f%% (%.0fs)\r", n, 100.0 * double(w1) / double(nblocks), now() - t0);
    }
  }
  offs[nblocks] = pos;
  h.offsetsPos = pos;
  if (fwrite(offs.data(), 8, nblocks + 1, f) != nblocks + 1) { perror("write"); return 1; }
  if (fseek(f, 0, SEEK_SET) != 0 || fwrite(&h, sizeof h, 1, f) != 1) { perror("write header"); return 1; }
  if (fclose(f) != 0) { perror("close"); return 1; }
  if (rename(tmp.c_str(), path.c_str()) != 0) { perror("rename"); return 1; }
  uint64_t total = pos + 8 * (nblocks + 1);
  printf("layer %d: %" PRIu64 " boards -> %s: %" PRIu64 " bytes (%.4f B/board, table %.4f B/board, ratio %.2fx) level %d block %u pack %d in %.1fs\n",
         n, ly.size, path.c_str(), total, double(total) / double(ly.size), double(ly.bytes) / double(ly.size),
         double(ly.bytes) / double(total), level, blockBoards, pack, now() - t0);
  return 0;
}

static int cmdVerify(int n, const std::string& gmzdir, int threads, uint64_t sample) {
  const Layer& ly = lh(n);
  GmzTable T;
  if (!T.open(gmzdir)) return 1;
  const GmzLayer* g = T.layer(n);
  if (!g) return 1;
  double t0 = now();
  // gM: every board, block by block, sequentially through both files
  std::atomic<uint64_t> badM{0};
  {
    std::atomic<uint64_t> next{0};
    std::vector<std::thread> th;
    for (int ti = 0; ti < threads; ++ti) th.emplace_back([&]() {
      std::vector<uint8_t> raw(size_t(g->h.blockBoards) + 2);
      ZSTD_DCtx* d = ZSTD_createDCtx();
      for (;;) {
        uint64_t b = next.fetch_add(1);
        if (b >= g->h.nblocks) break;
        size_t r = ZSTD_decompressDCtx(d, raw.data(), raw.size(), g->base + g->offs[b], g->offs[b + 1] - g->offs[b]);
        if (ZSTD_isError(r)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(r)); exit(1); }
        uint64_t i0 = b * uint64_t(g->h.blockBoards), i1 = std::min<uint64_t>(i0 + g->h.blockBoards, ly.size);
        uint64_t bad = 0;
        for (uint64_t i = i0; i < i1; ++i) bad += raw[i - i0] != ly.decM[ly.get(i)];
        if (bad) badM.fetch_add(bad);
      }
      ZSTD_freeDCtx(d);
    });
    for (auto& t : th) t.join();
  }
  printf("layer %d: gM of all %" PRIu64 " boards checked in %.1fs: %" PRIu64 " mismatches\n", n, ly.size, now() - t0, badM.load());
  fflush(stdout);
  // gN: derived from the children.  All boards when the reachable layers fit
  // in memory (one byte per board), else a random sample through the block reader.
  uint64_t reach = 0;
  for (int m = std::max(0, n - 20); m <= n; ++m) if (m != SEEDS - 1) reach += IX.layerSize(m);
  bool all = sample == 0 || (reach < (64ull << 30) && !getenv("OW_GMZ_FORCE_SAMPLE"));
  double t1 = now();
  std::atomic<uint64_t> badN{0};
  uint64_t checked;
  if (all) {
    for (int m = std::max(0, n - 20); m <= n; ++m) if (m != SEEDS - 1) if (!T.loadAll(m)) return 1;
    std::vector<std::thread> th;
    for (int ti = 0; ti < threads; ++ti) th.emplace_back([&, ti]() {
      GmzReader R(T);
      uint64_t i0 = ly.size * ti / threads, i1 = ly.size * (ti + 1) / threads, bad = 0;
      for (uint64_t i = i0; i < i1; ++i) bad += R.gN(IX.unrank(i, n), n, IX) != ly.gN(ly.get(i));
      badN.fetch_add(bad);
    });
    for (auto& t : th) t.join();
    checked = ly.size;
  } else {
    std::vector<std::thread> th;
    for (int ti = 0; ti < threads; ++ti) th.emplace_back([&, ti]() {
      GmzReader R(T);
      std::mt19937_64 rng(0x9e3779b97f4a7c15ull * (ti + 1) + n);
      uint64_t bad = 0;
      for (uint64_t k = ti; k < sample; k += threads) {
        uint64_t i = rng() % ly.size;
        bad += R.gN(IX.unrank(i, n), n, IX) != ly.gN(ly.get(i));
      }
      badN.fetch_add(bad);
    });
    for (auto& t : th) t.join();
    checked = sample;
  }
  printf("layer %d: derived gN of %" PRIu64 " %s boards checked in %.1fs: %" PRIu64 " mismatches%s\n", n, checked,
         all ? "(all)" : "random", now() - t1, badN.load(), badM.load() + badN.load() ? "  FAILED" : "  OK");
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
    uint64_t sample = argc > 7 ? strtoull(argv[7], nullptr, 10) : 10000000ull;
    return cmdVerify(atoi(argv[5]), argv[3], threads, sample);
  }
  fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 1;
}
