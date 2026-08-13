// Locality-aware out-of-core retrograde engine for Pentago
//
// Single-machine replacement for girving's MPI engine (girving/pentago/mpi).
// Computes slice n from slice n+1, section by section, in a fixed order that
// makes disk access near-sequential (see DESIGN.md):
//
//   - Output sections are processed in the fixed (sorted) section order.
//   - While computing output section S, only its <= 4 child sections of slice
//     n+1 are touched, so the input reader's block cache stays hot; consecutive
//     output sections tend to share child sections.
//   - Within a section, block lines are processed dimension by dimension, so
//     each child section is swept in a near-sequential order.
//
// Compute kernels and the block-line geometry are reused from
// girving/pentago/end/compute.cc (line_details_t / schedule_compute_line).
//
// Current scope (v1): the uncompressed output section must fit in RAM
// (--section-memory).  This covers all sections down to roughly slice 27 with
// 750 GB RAM; the slab/chunked mode for the few monster sections near the peak
// (largest is 1.8 TB uncompressed) is the next milestone (see README roadmap).
#pragma once

#include "slice_file.h"
#include "pentago/end/sections.h"
#include <functional>

namespace pgs {

struct engine_options_t {
  uint64_t section_memory = (uint64_t)200<<30; // max uncompressed output section size
  uint64_t line_memory = (uint64_t)4<<30;      // max memory of in-flight block lines
  uint64_t cache_bytes = (uint64_t)4<<30;      // input block cache
  int zlib_level = 6;
  int cpu_threads = 0; // 0 = hardware concurrency
  std::function<void(const std::string&)> progress; // optional progress log
};

// All standard sections per slice, computed once (expensive: a few seconds).
// Result indexed by slice 0..35.
std::vector<std::shared_ptr<const pentago::end::sections_t>> all_sections();

// Compute slice n into dir/slice-n.pgs, reading slice n+1 from dir (not needed
// for n == 35).  Restarts automatically from existing partial files.
// Throws if slice n+1 is missing/incomplete, or a section exceeds
// options.section_memory.
void compute_slice(int n, const std::string& dir,
                   const std::vector<std::shared_ptr<const pentago::end::sections_t>>& by_slice,
                   const engine_options_t& options);

}  // namespace pgs
