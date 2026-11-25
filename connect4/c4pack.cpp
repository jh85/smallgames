// c4pack.cpp — builds a compressed WDL package from a board directory written by `c4 solve`.
//
// Every input file (zdd.bin, slab_<ply>.bin) becomes <name>.zst in the zstd *seekable format*:
// independent zstd frames of --frame-mib MiB of input each (content checksums on), followed by
// a skippable frame holding the seek table. Stock `zstd -d` ignores the skippable frame and
// restores the original file byte for byte. A reader can also use the seek table to decompress
// only the frame that holds a given position (see c4_wdl_query.cpp). Small files (PACK_BASE3)
// are copied as they are.
//
// Every compressed frame is decompressed again in memory and compared with its input before it
// is written. For each file, the SHA-256 of the original and of the .zst are recorded in
// MANIFEST.tsv in the output directory. A file is written as <name>.zst.tmp, fsynced and renamed,
// then appended to MANIFEST.tsv. A restarted run skips the files already listed there, so the
// command can simply be re-run after an interruption.
//
// Input is read with O_DIRECT and written output is dropped from the page cache, so the run does
// not fill RAM with cache. Reading, compressing and writing overlap: a reader thread keeps up to
// 4 batches ahead, and the workers compress frames continuously.
//
// usage: c4pack SRC_DIR DST_DIR [--level L] [--frame-mib M] [--threads N] [--batch-gib G]
//   defaults: level 19, 16 MiB frames, all hardware threads, 2 GiB batches
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zstd.h>
#include <openssl/evp.h>

typedef uint64_t u64;
typedef uint32_t u32;

[[noreturn]] static void die(const char* fmt, const char* a = "", const char* b = "") {
  fprintf(stderr, "error: ");
  fprintf(stderr, fmt, a, b);
  fprintf(stderr, " (%s)\n", strerror(errno));
  exit(1);
}
static double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct Sha {
  EVP_MD_CTX* c;
  Sha() { c = EVP_MD_CTX_new(); EVP_DigestInit_ex(c, EVP_sha256(), nullptr); }
  ~Sha() { EVP_MD_CTX_free(c); }
  void add(const void* p, size_t n) { EVP_DigestUpdate(c, p, n); }
  std::string hex() {
    unsigned char d[32]; unsigned n = 0;
    EVP_DigestFinal_ex(c, d, &n);
    char s[65];
    for (int i = 0; i < 32; ++i) snprintf(s + 2 * i, 3, "%02x", d[i]);
    return s;
  }
};

static void writeAll(int fd, const void* p, size_t n) {
  const char* c = (const char*)p;
  while (n) {
    ssize_t w = write(fd, c, n);
    if (w <= 0) die("write failed");
    c += w; n -= (size_t)w;
  }
}

static int LEVEL = 19, THREADS = 0;
static u64 FRAME = 16ull << 20, BATCH = 2ull << 30;

// Reads up to `want` bytes at `off`. O_DIRECT needs aligned sizes, so the tail is read buffered.
static u64 readAt(int fdDirect, int fdBuf, char* buf, u64 off, u64 want) {
  u64 got = 0;
  u64 aligned = want & ~4095ull;
  while (got < aligned) {
    ssize_t r = pread(fdDirect, buf + got, aligned - got, (off_t)(off + got));
    if (r < 0) die("read failed");
    if (r == 0) return got;
    got += (u64)r;
    if (got & 4095) break;        // short read at end of file
  }
  while (got < want) {
    ssize_t r = pread(fdBuf, buf + got, want - got, (off_t)(off + got));
    if (r < 0) die("read failed");
    if (r == 0) break;
    got += (u64)r;
  }
  return got;
}

static void packFile(const std::string& src, const std::string& dstDir, const std::string& name, FILE* manifest) {
  std::string in = src + "/" + name, outPath = dstDir + "/" + name + ".zst", tmp = outPath + ".tmp";
  int fdD = open(in.c_str(), O_RDONLY | O_DIRECT), fdB = open(in.c_str(), O_RDONLY);
  if (fdD < 0 || fdB < 0) die("cannot open %s", in.c_str());
  struct stat st;
  fstat(fdB, &st);
  u64 size = (u64)st.st_size;
  int fo = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fo < 0) die("cannot create %s", tmp.c_str());

  double t0 = now_s();
  Sha shaRaw, shaZst;
  std::vector<std::pair<u32, u32>> seek;     // (compressed, decompressed) per frame
  u64 zbytes = 0;

  // Pipeline: a reader fills NB batch slots ahead; workers compress frames in order as their
  // batch arrives; this thread writes frames in order and frees each slot when its batch is done.
  const int NB = 4;
  const u64 FPB = BATCH / FRAME;                               // frames per batch
  const u64 nfTotal = size ? (size + FRAME - 1) / FRAME : 1;
  const u64 nbTotal = (nfTotal + FPB - 1) / FPB;
  struct Slot { char* raw = nullptr; u64 n = 0; long batch = -1; bool ready = false; };
  Slot slot[NB];
  for (auto& sl : slot)
    if (posix_memalign((void**)&sl.raw, 4096, BATCH)) die("out of memory");
  std::vector<std::vector<char>> out(nfTotal);
  std::vector<char> done(nfTotal, 0);
  std::mutex m;
  std::condition_variable cv;

  std::thread reader([&] {
    for (u64 bi = 0; bi < nbTotal; ++bi) {
      Slot& sl = slot[bi % NB];
      {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return sl.batch < 0; });
      }
      u64 off = bi * BATCH;
      u64 n = off < size ? readAt(fdD, fdB, sl.raw, off, std::min(BATCH, size - off)) : 0;
      if (n != std::min(BATCH, size - std::min(off, size))) die("short read on %s", in.c_str());
      std::lock_guard<std::mutex> lk(m);
      sl.n = n; sl.batch = (long)bi; sl.ready = true;
      cv.notify_all();
    }
  });
  std::atomic<u64> next{0};
  std::vector<std::thread> th;
  for (int t = 0; t < THREADS; ++t)
    th.emplace_back([&] {
      ZSTD_CCtx* cc = ZSTD_createCCtx();
      ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, LEVEL);
      ZSTD_CCtx_setParameter(cc, ZSTD_c_checksumFlag, 1);
      ZSTD_CCtx_setParameter(cc, ZSTD_c_contentSizeFlag, 1);
      ZSTD_DCtx* dc = ZSTD_createDCtx();
      std::vector<char> back(FRAME);
      for (u64 g; (g = next.fetch_add(1)) < nfTotal;) {
        u64 bi = g / FPB;
        Slot& sl = slot[bi % NB];
        {
          std::unique_lock<std::mutex> lk(m);
          cv.wait(lk, [&] { return sl.ready && sl.batch == (long)bi; });
        }
        u64 lo = (g % FPB) * FRAME, len = std::min(FRAME, sl.n - lo);
        std::vector<char> o(ZSTD_compressBound(len));
        size_t z = ZSTD_compress2(cc, o.data(), o.size(), sl.raw + lo, len);
        if (ZSTD_isError(z)) { fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(z)); exit(1); }
        o.resize(z);
        size_t d = ZSTD_decompressDCtx(dc, back.data(), back.size(), o.data(), o.size());
        if (ZSTD_isError(d) || d != len || memcmp(back.data(), sl.raw + lo, len)) {
          fprintf(stderr, "error: frame %llu of %s does not round-trip\n", (unsigned long long)g, name.c_str());
          exit(1);
        }
        std::lock_guard<std::mutex> lk(m);
        out[g].swap(o);
        done[g] = 1;
        cv.notify_all();
      }
      ZSTD_freeCCtx(cc);
      ZSTD_freeDCtx(dc);
    });
  u64 off = 0;
  for (u64 g = 0; g < nfTotal; ++g) {
    {
      std::unique_lock<std::mutex> lk(m);
      cv.wait(lk, [&] { return done[g] != 0; });
    }
    u64 bi = g / FPB;
    Slot& sl = slot[bi % NB];
    u64 len = std::min(FRAME, sl.n - (g % FPB) * FRAME);
    writeAll(fo, out[g].data(), out[g].size());
    shaZst.add(out[g].data(), out[g].size());
    seek.push_back({(u32)out[g].size(), (u32)len});
    zbytes += out[g].size();
    std::vector<char>().swap(out[g]);
    if (g % FPB == FPB - 1 || g == nfTotal - 1) {   // batch complete: hash its input, free the slot
      shaRaw.add(sl.raw, sl.n);
      off += sl.n;
      {
        std::lock_guard<std::mutex> lk(m);
        sl.ready = false; sl.batch = -1;
        cv.notify_all();
      }
      double el = now_s() - t0;
      fprintf(stderr, "\r  %s: %.1f / %.1f GB, %.2fx, %.0f MB/s   ", name.c_str(), off / 1e9, size / 1e9,
              zbytes ? (double)off / zbytes : 0.0, off / 1e6 / std::max(el, 1e-9));
      fdatasync(fo);
      posix_fadvise(fo, 0, 0, POSIX_FADV_DONTNEED);
    }
  }
  reader.join();
  for (auto& x : th) x.join();
  for (auto& sl : slot) free(sl.raw);
  if (off != size) die("short read on %s", in.c_str());
  // seek table: skippable frame (magic 0x184D2A5E), entries, footer (frames, descriptor, magic 0x8F92EAB1)
  std::vector<char> tab;
  auto le32 = [&](u32 v) { for (int i = 0; i < 4; ++i) tab.push_back((char)(v >> (8 * i))); };
  le32(0x184D2A5E);
  le32((u32)(seek.size() * 8 + 9));
  for (auto& e : seek) { le32(e.first); le32(e.second); }
  le32((u32)seek.size());
  tab.push_back(0);                // descriptor: no per-frame checksums in the table (frames carry their own)
  le32(0x8F92EAB1);
  writeAll(fo, tab.data(), tab.size());
  shaZst.add(tab.data(), tab.size());
  zbytes += tab.size();
  if (fsync(fo)) die("fsync failed");
  posix_fadvise(fo, 0, 0, POSIX_FADV_DONTNEED);
  close(fo); close(fdD); close(fdB);
  if (rename(tmp.c_str(), outPath.c_str())) die("rename failed");
  std::string hr = shaRaw.hex(), hz = shaZst.hex();
  fprintf(manifest, "%s\t%llu\t%s\t%s.zst\t%llu\t%s\n", name.c_str(), (unsigned long long)size, hr.c_str(),
          name.c_str(), (unsigned long long)zbytes, hz.c_str());
  fflush(manifest);
  fsync(fileno(manifest));
  fprintf(stderr, "\n");
  printf("[pack] %-14s %18llu -> %16llu B (%.2fx), %zu frames, %.0f s\n", name.c_str(), (unsigned long long)size,
         (unsigned long long)zbytes, zbytes ? (double)size / zbytes : 0.0, seek.size(), now_s() - t0);
  fflush(stdout);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s SRC_DIR DST_DIR [--level L] [--frame-mib M] [--threads N] [--batch-gib G]\n", argv[0]);
    return 1;
  }
  std::string src = argv[1], dst = argv[2];
  THREADS = (int)std::thread::hardware_concurrency();
  for (int i = 3; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--level") LEVEL = atoi(argv[i + 1]);
    else if (a == "--frame-mib") FRAME = (u64)(atof(argv[i + 1]) * (1 << 20));
    else if (a == "--threads") THREADS = atoi(argv[i + 1]);
    else if (a == "--batch-gib") BATCH = (u64)(atof(argv[i + 1]) * (1ull << 30));
    else die("unknown option %s", a.c_str());
  }
  if (FRAME == 0 || FRAME > (1ull << 31) || BATCH < FRAME) die("bad frame/batch size");
  BATCH = (BATCH / FRAME) * FRAME;
  BATCH = (BATCH + 4095) & ~4095ull;
  if (BATCH % FRAME) die("batch must be a multiple of the frame size and of 4096");
  mkdir(dst.c_str(), 0755);

  // inputs: zdd.bin, slab_0.bin, slab_1.bin, ... in ply order; PACK_BASE3 is copied
  std::vector<std::string> names{"zdd.bin"};
  for (int k = 0; k < 200; ++k) {
    std::string n = "slab_" + std::to_string(k) + ".bin";
    if (access((src + "/" + n).c_str(), R_OK) == 0) names.push_back(n);
  }
  if (access((src + "/zdd.bin").c_str(), R_OK)) die("no zdd.bin in %s", src.c_str());
  if (access((src + "/PACK_BASE3").c_str(), R_OK) == 0) {
    std::string cmd = "cp -p '" + src + "/PACK_BASE3' '" + dst + "/PACK_BASE3'";
    if (system(cmd.c_str())) die("copying PACK_BASE3 failed");
  }
  std::set<std::string> done;
  std::string mpath = dst + "/MANIFEST.tsv";
  if (FILE* m = fopen(mpath.c_str(), "r")) {
    char line[1024];
    while (fgets(line, sizeof line, m)) done.insert(std::string(line, strcspn(line, "\t")));
    fclose(m);
  }
  FILE* manifest = fopen(mpath.c_str(), "a");
  if (!manifest) die("cannot open %s", mpath.c_str());
  printf("[pack] %s -> %s: %zu files (%zu already done), zstd level %d, %.0f MiB frames, %d threads\n",
         src.c_str(), dst.c_str(), names.size(), done.size(), LEVEL, FRAME / 1048576.0, THREADS);
  fflush(stdout);
  double t0 = now_s();
  for (auto& n : names)
    if (!done.count(n)) packFile(src, dst, n, manifest);
  fclose(manifest);
  printf("[pack] complete in %.0f s\n", now_s() - t0);
  return 0;
}
