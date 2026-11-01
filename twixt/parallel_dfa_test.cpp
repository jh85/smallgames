#include "parallel_dfa.hpp"

#include <atomic>
#include <iostream>

using namespace twixt_dfa;

void require(bool good, const char* message) {
  if (!good) throw std::runtime_error(message);
}

int main() {
  try {
#ifdef TWIXT_PROCESS_WORKERS
    WorkerDirectory test_directory(std::filesystem::temp_directory_path().string());
    for (bool threads : {false, true}) {
    ProcessOptions options{64, 512ULL << 20, test_directory.file(0), threads};
    Manager manager({3, 2, 3, 2}, 200'000);
    const Word zero{0, 0, 0, 0};
    Id seed = manager.singleton(zero);
    auto word_for = [](std::size_t job) {
      return Word{static_cast<std::uint8_t>(job % 3), static_cast<std::uint8_t>((job / 3) % 2),
                  static_cast<std::uint8_t>((job / 6) % 3), static_cast<std::uint8_t>((job / 18) % 2)};
    };
    std::vector<Id> kept;
    const auto stats = parallel_jobs(manager, options, 72, 2,
        [&](std::size_t job) {
          const Word word = word_for(job);
          Id singleton = manager.singleton(word);
          auto cube = manager.unrestricted();
          cube[0] = static_cast<std::uint8_t>(1U << word[0]);
          singleton = manager.filter(singleton, cube);
          return std::vector<Id>{singleton, manager.unite(seed, singleton)};
        },
        [&](std::size_t job, const std::vector<Id>& roots) {
          require(manager.contains(roots[0], word_for(job)), "delta changed singleton");
          require(manager.count(roots[0]) == 1, "delta singleton count");
          require(manager.contains(roots[1], zero) && manager.contains(roots[1], word_for(job)),
                  "delta lost base-node references");
          require(manager.count(roots[1]) == (word_for(job) == zero ? 1 : 2), "delta union count");
          kept.insert(kept.end(), roots.begin(), roots.end());
        },
        [&]() {
          if (threads) require(manager.filter_cache_bytes() == 0,
                               "finished workers retained dense filter buffers at the merge barrier");
          std::vector<Id*> live{&seed};
          for (auto& root : kept) live.push_back(&root);
          manager.collect(live);
        });
    require(stats.peak_workers == 64, "64-worker scheduler did not launch a full wave");
    require(std::filesystem::is_empty(options.temporary_directory), "worker scratch was not cleaned");
    if (threads) {
      // Waves larger than the thread count: 250 jobs, 7 threads, 100 jobs per
      // wave; every job runs once and each wave ends in one barrier.
      ProcessOptions wide = options;
      wide.workers = 7;
      wide.wave_jobs = 100;
      std::vector<std::atomic<int>> runs(250);
      std::vector<Id> wave_roots;
      int barriers = 0;
      std::size_t consumed = 0;
      const auto wide_stats = parallel_jobs(manager, wide, runs.size(), 1,
          [&](std::size_t job) {
            runs[job].fetch_add(1);
            return std::vector<Id>{manager.singleton(word_for(job % 36))};
          },
          [&](std::size_t job, const std::vector<Id>& roots) {
            require(job == consumed++, "wave results consumed out of order");
            require(manager.contains(roots[0], word_for(job % 36)), "wave job result");
            wave_roots.push_back(roots[0]);
          },
          [&]() {
            ++barriers;
            require(consumed == std::min<std::size_t>(250, 100 * static_cast<std::size_t>(barriers)),
                    "barrier before its wave finished");
            std::vector<Id*> live{&seed};
            for (auto& root : kept) live.push_back(&root);
            for (auto& root : wave_roots) live.push_back(&root);
            manager.collect(live);
          });
      for (const auto& r : runs) require(r.load() == 1, "wave job ran other than once");
      require(barriers == 3, "100-job waves over 250 jobs need three barriers");
      require(wide_stats.peak_workers == 7, "wide waves launched more threads than workers");
    }

    // A base-only language generates a zero-node delta. A wrong snapshot ID
    // must be rejected instead of silently binding references to another DAG.
    const std::string delta = test_directory.file(1);
    const Id base = static_cast<Id>(manager.size());
    write_delta(manager, delta, base, {seed});
    const auto loaded = read_delta(manager, delta, base, 1);
    require(loaded[0] == seed, "base-only delta changed the root");
    bool rejected = false;
    try { (void)read_delta(manager, delta, base - 1, 1); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "mismatched worker snapshot was accepted");
    std::filesystem::remove(delta);

    options.workers = 4;
    for (unsigned failure = 0; failure < 3; ++failure) {
      bool failed = false;
      options.memory_bytes = failure == 2 ? 1 : 512ULL << 20;
      try {
        parallel_jobs(manager, options, 4, 1,
            [&](std::size_t job) -> std::vector<Id> {
              if (job == 0 && failure == 0) throw std::runtime_error("injected worker failure");
              if (job == 0 && failure == 1) throw Limit("injected worker cap");
              ::usleep(100'000);
              return {seed};
            }, [](std::size_t, const std::vector<Id>&) {}, []() {});
      } catch (const Limit&) { failed = failure != 0; }
      catch (const std::runtime_error&) { failed = failure == 0; }
      require(failed, "worker failure/cap did not propagate");
      int status = 0;
      errno = 0;
      require(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD,
              "failed wave left unreaped workers");
      require(std::filesystem::is_empty(options.temporary_directory), "failed wave leaked scratch files");
    }
    }
    // Exercise allocation across several block boundaries, including races
    // creating the next block. IDs must be unique and published values intact.
    NodeStore store;
    std::vector<std::thread> writers;
    std::vector<Id> ids(64 * 2048);
    for (unsigned slot = 0; slot < 64; ++slot) {
      writers.emplace_back([&, slot]() {
        for (unsigned i = 0; i < 2048; ++i) {
          // Packed nodes keep 8-bit levels; children may use all 40 bits.
          ids[slot * 2048 + i] = store.append(Node{{slot, i, (Id{1} << 39) + slot * 2048 + i}, (slot + i) % 251});
        }
      });
    }
    for (auto& writer : writers) writer.join();
    require(store.size() == ids.size(), "concurrent node IDs were lost");
    for (unsigned slot = 0; slot < 64; ++slot) {
      for (unsigned i = 0; i < 2048; ++i) {
        const Node& node = store.at(ids[slot * 2048 + i]);
        require(node.next[0] == slot && node.next[1] == i && node.next[2] == (Id{1} << 39) + slot * 2048 + i &&
                node.level == (slot + i) % 251, "concurrent node publication was corrupted");
      }
    }
    std::sort(ids.begin(), ids.end());
    require(std::adjacent_find(ids.begin(), ids.end()) == ids.end(), "duplicate concurrent node IDs");
    std::cout << "parallel DFA: 64 threads/processes, node publication, transfers, barriers, failure cleanup passed\n";
#else
    std::cout << "parallel DFA tests skipped on non-POSIX platform\n";
#endif
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "parallel DFA test failed: " << error.what() << '\n';
    return 1;
  }
}
