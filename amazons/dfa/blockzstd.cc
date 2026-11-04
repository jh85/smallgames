/*
  blockzstd — compressed size of a file when each fixed-size block is a separate
  zstd frame.  This is what a random-access or streaming table format would get,
  as opposed to the whole-file numbers of the zstd command-line tool.

  Usage: blockzstd FILE BLOCK_BYTES LEVEL [THREADS]
*/
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: blockzstd FILE BLOCK_BYTES LEVEL [THREADS]\n");
    return 2;
  }
  const char* path = argv[1];
  const uint64_t block = std::strtoull(argv[2], nullptr, 10);
  const int level = std::atoi(argv[3]);
  const int threads = argc > 4 ? std::atoi(argv[4]) : int(std::thread::hardware_concurrency());
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) {
    std::perror(path);
    return 1;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size == 0) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 1;
  }
  const uint64_t size = uint64_t(st.st_size);
  const auto* data = static_cast<const char*>(::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    std::perror("mmap");
    return 1;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const uint64_t blocks = (size + block - 1) / block;
  std::atomic<uint64_t> next{0}, total{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&] {
      std::vector<char> out(ZSTD_compressBound(block));
      ZSTD_CCtx* ctx = ZSTD_createCCtx();
      for (;;) {
        const uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= blocks) break;
        const uint64_t begin = i * block;
        const uint64_t len = begin + block > size ? size - begin : block;
        const size_t got = ZSTD_compressCCtx(ctx, out.data(), out.size(), data + begin, len, level);
        if (ZSTD_isError(got)) {
          std::fprintf(stderr, "zstd: %s\n", ZSTD_getErrorName(got));
          std::exit(1);
        }
        total.fetch_add(got, std::memory_order_relaxed);
      }
      ZSTD_freeCCtx(ctx);
    });
  }
  for (auto& th : pool) th.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%s block=%" PRIu64 " level=%d raw=%" PRIu64 " compressed=%" PRIu64 " ratio=%.2f secs=%.1f\n", path, block, level, size,
              total.load(), double(size) / double(total.load()), secs);
  return 0;
}
