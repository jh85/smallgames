#pragma once

#include "symbolic_dfa.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#define TWIXT_PROCESS_WORKERS 1
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace twixt_dfa {

inline void delta_u32(std::ostream& out, std::uint32_t value) {
  const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                         static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  out.write(bytes, 4);
}

inline std::uint32_t delta_u32(std::istream& in) {
  unsigned char bytes[4];
  if (!in.read(reinterpret_cast<char*>(bytes), 4)) throw std::runtime_error("truncated worker delta");
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

// Old IDs [0,base) are shared with the fork's parent. Only reachable NEW
// nodes cross the process boundary. The parent must not collect until every
// worker from this wave has been imported and reaped.
inline void write_delta(Manager& manager, const std::string& path, Id base,
                        const std::vector<Id>& roots) {
  if (base < 2 || base > manager.size()) throw std::logic_error("bad worker snapshot base");
  const std::size_t added = manager.size() - base;
  std::vector<std::uint8_t> live(added);
  std::vector<Id> stack;
  auto mark = [&](Id id) {
    if (id < base || live[id - base]) return;
    live[id - base] = 1;
    stack.push_back(id);
  };
  for (Id root : roots) mark(root);
  std::uint32_t count = 0;
  while (!stack.empty()) {
    const Id id = stack.back(); stack.pop_back();
    ++count;
    for (Id child : manager.node(id).next) mark(child);
    if ((count & 0x3fffU) == 0) manager.checkpoint();
  }
  std::vector<Id> mapped(added);
  Id next = base;
  for (std::size_t i = 0; i < added; ++i) if (live[i]) mapped[i] = next++;
  auto encode = [&](Id id) { return id < base ? id : mapped[id - base]; };
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot create worker delta: " + path);
  out.write("TWXDEL01", 8);
  delta_u32(out, manager.length());
  delta_u32(out, base);
  delta_u32(out, count);
  delta_u32(out, static_cast<std::uint32_t>(roots.size()));
  for (auto n : manager.alphabet()) out.put(static_cast<char>(n));
  for (Id root : roots) delta_u32(out, encode(root));
  for (std::size_t i = 0; i < added; ++i) {
    if (!live[i]) continue;
    const Node& node = manager.node(static_cast<Id>(base + i));
    delta_u32(out, node.level);
    for (Id child : node.next) delta_u32(out, encode(child));
    if ((i & 0x3fffU) == 0) manager.checkpoint();
  }
  out.close();
  if (!out) throw std::runtime_error("failed to write worker delta: " + path);
}

inline std::vector<Id> read_delta(Manager& manager, const std::string& path,
                                 Id expected_base, unsigned root_count) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("missing worker delta: " + path);
  char magic[8]{};
  in.read(magic, 8);
  if (std::string(magic, 8) != "TWXDEL01" || delta_u32(in) != manager.length()) {
    throw std::runtime_error("wrong worker delta schema");
  }
  const Id base = delta_u32(in), count = delta_u32(in);
  if (base != expected_base || base > manager.size() || delta_u32(in) != root_count ||
      count > std::numeric_limits<Id>::max() - base) {
    throw std::runtime_error("wrong worker snapshot or root count");
  }
  for (auto n : manager.alphabet()) {
    if (in.get() != n) throw std::runtime_error("wrong worker delta alphabet");
  }
  const auto offset = in.tellg();
  in.seekg(0, std::ios::end);
  const std::uint64_t expected = static_cast<std::uint64_t>(offset) +
                                4ULL * root_count + 16ULL * count;
  if (static_cast<std::uint64_t>(in.tellg()) != expected) throw std::runtime_error("wrong worker delta length");
  in.seekg(offset);
  std::vector<Id> roots(root_count), mapped(count);
  for (auto& root : roots) root = delta_u32(in);
  auto decode = [&](Id id, std::uint32_t available) -> Id {
    if (id < base) return id;
    if (id - base >= available) throw std::runtime_error("invalid worker delta reference");
    return mapped[id - base];
  };
  for (std::uint32_t i = 0; i < count; ++i) {
    const unsigned level = delta_u32(in);
    Children children{};
    for (Id& child : children) child = decode(delta_u32(in), i);
    mapped[i] = manager.intern(level, children);
  }
  for (auto& root : roots) {
    root = decode(root, count);
    if (root != 0 && (root == 1 || manager.node(root).level != 0)) {
      throw std::runtime_error("invalid worker language root");
    }
  }
  return roots;
}

struct ProcessOptions {
  unsigned workers = 1;
  std::uint64_t memory_bytes = 0;  // aggregate PSS on Linux; 0 disables
  std::string temporary_directory = "tables";
  bool threads = true;
};

struct ProcessStats {
  double seconds = 0;
  double import_seconds = 0;
  double merge_seconds = 0;
  std::uint64_t peak_memory_bytes = 0;
  std::uint64_t delta_bytes = 0;
  unsigned peak_workers = 0;
  unsigned peak_active_workers = 0;
};

#ifdef TWIXT_PROCESS_WORKERS

class WorkerDirectory {
 public:
  explicit WorkerDirectory(const std::string& parent) {
    std::filesystem::create_directories(parent);
    std::string pattern = (std::filesystem::path(parent) / "twixt-workers-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* result = ::mkdtemp(buffer.data());
    if (!result) throw std::runtime_error("cannot create worker temporary directory");
    path_ = result;
  }
  ~WorkerDirectory() {
    std::error_code error;
    // This is precisely the private directory created above, never the user's
    // chosen parent directory. It contains only this pool's transient files.
    std::filesystem::remove_all(path_, error);
  }
  std::string file(std::size_t job) const { return path_ + "/" + std::to_string(job); }
 private:
  std::string path_;
};

inline std::uint64_t process_memory(pid_t pid) {
#ifdef __linux__
  std::ifstream in("/proc/" + std::to_string(pid) + "/smaps_rollup");
  std::string line;
  while (std::getline(in, line)) {
    if (line.compare(0, 4, "Pss:") == 0) {
      std::istringstream value(line.substr(4));
      std::uint64_t kib = 0;
      if (value >> kib) return kib * 1024;
    }
  }
#else
  (void)pid;
#endif
  return 0;
}

struct LiveWorkers {
  std::vector<pid_t> pids;
  ~LiveWorkers() {
    for (pid_t pid : pids) if (pid > 0) ::kill(pid, SIGTERM);
    for (pid_t pid : pids) {
      if (pid <= 0) continue;
      int status = 0;
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }
  }
};

// Jobs in one wave inherit the same immutable base. Importing/unioning is
// safe while children compute, but GC is allowed only at the wave barrier.
// Callback `barrier` may collect and renumber every live parent root.
template <class Compute, class Consume, class Barrier>
ProcessStats process_jobs(Manager& manager, const ProcessOptions& options,
                  std::size_t job_count, unsigned root_count,
                  Compute compute, Consume consume, Barrier barrier) {
  if (options.workers == 0) throw std::invalid_argument("zero parallel workers");
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  ProcessStats stats;
  WorkerDirectory directory(options.temporary_directory);
  const pid_t parent = ::getpid();
  for (std::size_t first = 0; first < job_count; first += options.workers) {
    manager.checkpoint();
    const std::size_t count = std::min<std::size_t>(options.workers, job_count - first);
    stats.peak_workers = std::max(stats.peak_workers, static_cast<unsigned>(count));
    const Id base = static_cast<Id>(manager.size());
    LiveWorkers workers;
    workers.pids.assign(count, 0);
    std::cout.flush(); std::cerr.flush();
    for (std::size_t i = 0; i < count; ++i) {
      manager.checkpoint();
      const std::size_t job = first + i;
      const pid_t child = ::fork();
      if (child < 0) throw std::runtime_error("cannot fork DFA worker");
      if (child == 0) {
#ifdef __linux__
        // Even SIGKILL of the coordinator must not leave workers solving in
        // the background. Check the race where the parent died before prctl.
        if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent) ::_exit(2);
#endif
        int code = 0;
        try {
          const auto roots = compute(job);
          if (roots.size() != root_count) throw std::logic_error("wrong worker result arity");
          write_delta(manager, directory.file(job), base, roots);
        } catch (const std::exception& error) {
          std::ofstream message(directory.file(job) + ".error");
          message << error.what();
          code = dynamic_cast<const Limit*>(&error) ? 3 : 2;
        } catch (...) {
          code = 2;
        }
        ::_exit(code);
      }
      workers.pids[i] = child;
    }
    std::size_t remaining = count;
    auto memory_checked = std::chrono::steady_clock::time_point{};
    while (remaining != 0) {
      manager.checkpoint();
      bool ready = false;
      const auto now = std::chrono::steady_clock::now();
      if (options.memory_bytes != 0 && now - memory_checked >= std::chrono::seconds(1)) {
        std::uint64_t bytes = process_memory(parent);
        for (pid_t pid : workers.pids) if (pid > 0) bytes += process_memory(pid);
        stats.peak_memory_bytes = std::max(stats.peak_memory_bytes, bytes);
        if (bytes > options.memory_bytes) throw Limit("aggregate parallel worker memory cap reached");
        memory_checked = now;
      }
      for (std::size_t i = 0; i < count; ++i) {
        if (workers.pids[i] <= 0) continue;
        int status = 0;
        const pid_t done = ::waitpid(workers.pids[i], &status, WNOHANG);
        if (done == 0 || (done < 0 && errno == EINTR)) continue;
        if (done < 0) throw std::runtime_error("cannot wait for DFA worker");
        workers.pids[i] = 0;
        --remaining;
        ready = true;
        const std::size_t job = first + i;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
          std::ifstream message(directory.file(job) + ".error");
          const std::string details((std::istreambuf_iterator<char>(message)), {});
          const std::string error = "DFA worker " + std::to_string(job) + " failed: " +
              (details.empty() ? "worker terminated without a result" : details);
          if (WIFEXITED(status) && WEXITSTATUS(status) == 3) throw Limit(error);
          throw std::runtime_error(error);
        }
        const auto import_started = Clock::now();
        stats.delta_bytes += std::filesystem::file_size(directory.file(job));
        consume(job, read_delta(manager, directory.file(job), base, root_count));
        std::filesystem::remove(directory.file(job));
        stats.import_seconds += std::chrono::duration<double>(Clock::now() - import_started).count();
      }
      if (!ready && remaining != 0) ::usleep(10'000);
    }
    const auto merge_started = Clock::now();
    barrier();
    stats.merge_seconds += std::chrono::duration<double>(Clock::now() - merge_started).count();
  }
  stats.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return stats;
}

#else

template <class Compute, class Consume, class Barrier>
ProcessStats process_jobs(Manager&, const ProcessOptions&, std::size_t, unsigned,
                  Compute, Consume, Barrier) {
  throw std::runtime_error("parallel DFA workers require a POSIX platform");
}

#endif

// Shared-memory workers use the same intern registry, stable node blocks, and
// private per-thread operation caches. No result serialization/import is needed.
template <class Compute, class Consume, class Barrier>
ProcessStats thread_jobs(Manager& manager, const ProcessOptions& options,
                         std::size_t job_count, unsigned root_count,
                         Compute compute, Consume consume, Barrier barrier) {
  if (options.workers == 0) throw std::invalid_argument("zero parallel workers");
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();
  ProcessStats stats;
  for (std::size_t first = 0; first < job_count; first += options.workers) {
    manager.checkpoint();
    const unsigned count = static_cast<unsigned>(std::min<std::size_t>(options.workers, job_count - first));
    stats.peak_workers = std::max(stats.peak_workers, count);
    std::vector<std::vector<Id>> results(count);
    std::vector<std::thread> workers;
    workers.reserve(count);
    std::atomic<unsigned> next{0}, remaining{count};
    std::atomic<unsigned> active{0}, high{0};
    std::exception_ptr worker_error, coordinator_error;
    std::mutex error_mutex;
    manager.prepare_workers(count);
    try {
      for (unsigned slot = 1; slot <= count; ++slot) {
        workers.emplace_back([&, slot]() {
          Manager::WorkerScope binding(manager, slot);
          try {
            while (!manager.worker_stopping()) {
              const unsigned job = next.fetch_add(1);
              if (job >= count) break;
              const unsigned busy = active.fetch_add(1) + 1;
              unsigned peak = high.load();
              while (busy > peak && !high.compare_exchange_weak(peak, busy)) {}
              try { results[job] = compute(first + job); }
              catch (...) { active.fetch_sub(1); throw; }
              active.fetch_sub(1);
              if (results[job].size() != root_count) throw std::logic_error("wrong worker result arity");
            }
          } catch (...) {
            std::lock_guard<std::mutex> guard(error_mutex);
            if (!worker_error) worker_error = std::current_exception();
            manager.request_worker_stop();
          }
          remaining.fetch_sub(1, std::memory_order_release);
        });
      }
    } catch (...) {
      manager.request_worker_stop();
      for (auto& worker : workers) worker.join();
      manager.finish_workers();
      throw;
    }
    auto memory_checked = Clock::time_point{};
    while (remaining.load(std::memory_order_acquire) != 0) {
      if (!manager.worker_stopping()) {
        try {
          manager.checkpoint();
#ifdef TWIXT_PROCESS_WORKERS
          const auto now = Clock::now();
          if (options.memory_bytes != 0 && now - memory_checked >= std::chrono::seconds(1)) {
            const std::uint64_t bytes = process_memory(::getpid());
            stats.peak_memory_bytes = std::max(stats.peak_memory_bytes, bytes);
            if (bytes > options.memory_bytes) throw Limit("aggregate parallel worker memory cap reached");
            memory_checked = now;
          }
#endif
        } catch (...) {
          coordinator_error = std::current_exception();
          manager.request_worker_stop();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    for (auto& worker : workers) worker.join();
    stats.peak_active_workers = std::max(stats.peak_active_workers, high.load());
    manager.finish_workers();
    if (coordinator_error) std::rethrow_exception(coordinator_error);
    if (worker_error) std::rethrow_exception(worker_error);
    for (unsigned job = 0; job < count; ++job) consume(first + job, results[job]);
    const auto merge_started = Clock::now();
    barrier();
    stats.merge_seconds += std::chrono::duration<double>(Clock::now() - merge_started).count();
  }
  stats.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  return stats;
}

template <class Compute, class Consume, class Barrier>
ProcessStats parallel_jobs(Manager& manager, const ProcessOptions& options,
                           std::size_t job_count, unsigned root_count,
                           Compute compute, Consume consume, Barrier barrier) {
  return options.threads ? thread_jobs(manager, options, job_count, root_count, compute, consume, barrier) :
                           process_jobs(manager, options, job_count, root_count, compute, consume, barrier);
}

}  // namespace twixt_dfa
