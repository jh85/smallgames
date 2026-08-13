// See engine.h.

#include "engine.h"
#include "pentago/end/compute.h"
#include "pentago/end/blocks.h"
#include "pentago/utility/thread.h"
#include "pentago/utility/index.h"
#include "pentago/utility/const_cast.h"
#include "pentago/utility/wall_time.h"
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace pgs {

using namespace pentago;
using namespace pentago::end;
using std::string;
using std::vector;

std::vector<std::shared_ptr<const sections_t>> all_sections() {
  // Empty root -> all sections of all slices
  section_t root;
  for (auto& c : root.counts) c = vec((uint8_t)0,(uint8_t)0);
  return descendent_sections(root, 35);
}

namespace {

struct slab_t {
  vector<Vector<super_t,2>> data; // one full output section, zero initialized
  Vector<int,4> shape, shape_strides;

  explicit slab_t(const section_t& s)
    : data(s.size()), shape(s.shape()), shape_strides(strides(shape)) {
    memset(data.data(), 0, 64*data.size());
  }

  // OR one computed output block into the slab.  Nodes of a block line are
  // disjoint within a dimension pass, so no locking is needed.
  void absorb(const section_t& section, const line_t& line, int k,
              RawArray<const Vector<super_t,2>> block) {
    const auto b4u = line.block(k);
    const auto b4 = Vector<int,4>(b4u);
    const auto bshape = block_shape(shape, b4u);
    GEODE_ASSERT(bshape.product() <= block.size());
    const uint64_t base = dot(shape_strides, block_size*b4);
    const auto bs = strides(bshape);
    for (int x0=0;x0<bshape[0];x0++)
      for (int x1=0;x1<bshape[1];x1++)
        for (int x2=0;x2<bshape[2];x2++)
          for (int x3=0;x3<bshape[3];x3++) {
            const uint64_t i = base + dot(shape_strides, vec(x0,x1,x2,x3));
            const int j = dot(bs, vec(x0,x1,x2,x3));
            data[i][0] |= block[j][0];
            data[i][1] |= block[j][1];
          }
  }
};

struct job_t {
  const line_t line_copy; // line_data_t holds a reference; we must own the line
  line_data_t pre;
  line_details_t details;
  job_t(const line_t& line, const line_details_t::wakeup_t& wakeup)
    : line_copy(line), pre(line_copy), details(pre, wakeup) {}
};

}  // namespace

void compute_slice(int n, const string& dir,
                   const vector<std::shared_ptr<const sections_t>>& by_slice,
                   const engine_options_t& opt) {
  GEODE_ASSERT(0<=n && n<=35);
  const auto& secs = *by_slice[n];
  const string path = dir + "/slice-" + std::to_string(n) + ".pgs";

  // Input reader for slice n+1
  std::unique_ptr<slice_reader_t> input;
  if (n < 35) {
    input.reset(new slice_reader_t(dir + "/slice-" + std::to_string(n+1) + ".pgs",
                                   opt.cache_bytes));
    if ((int)input->section_id.size() != (int)by_slice[n+1]->sections.size())
      throw std::runtime_error("slice " + std::to_string(n+1) + " is incomplete");
  }

  slice_writer_t writer(path, n, asarray(secs.sections), opt.zlib_level);
  if (opt.progress)
    opt.progress("slice " + std::to_string(n) + ": " + std::to_string(secs.sections.size())
                 + " sections, " + std::to_string(writer.sections_done()) + " done (restart)");

  std::mutex mu;
  std::condition_variable cv;
  uint64_t in_flight = 0; // bytes of line buffers in flight

  for (int si = writer.sections_done(); si < secs.sections.size(); si++) {
    const section_t& S = secs.sections[si];
    if (S.size() > opt.section_memory)
      throw std::runtime_error("section too large for --section-memory: needs slab mode (not yet implemented)");
    slab_t slab(S);
    const auto blocks = section_blocks(S);

    for (int d=0;d<4;d++) {
      if (S.counts[d].sum() >= 9) continue; // quadrant full: no moves
      // Child section in standard form
      const auto std_child = S.child(d).standardize<8>();
      const int child_id = n<35 ? input->find_section(get<0>(std_child)) : -1;
      if (n < 35 && child_id < 0)
        throw std::runtime_error("child section missing from slice above");

      // Iterate block lines along dimension d
      const auto cross = blocks.remove_index(d);
      for (int c0=0;c0<cross[0];c0++) for (int c1=0;c1<cross[1];c1++) for (int c2=0;c2<cross[2];c2++) {
        line_t line;
        line.section = S;
        line.dimension = d;
        line.length = blocks[d];
        line.block_base = vec((uint8_t)c0,(uint8_t)c1,(uint8_t)c2);
        if (getenv("PGS_DEBUG"))
          fprintf(stderr, "line: section=%llx d=%d base=(%d,%d,%d) len=%d shape=(%d,%d,%d,%d)\n",
                  (unsigned long long)S.sig(), d, c0, c1, c2, (int)blocks[d],
                  S.shape()[0], S.shape()[1], S.shape()[2], S.shape()[3]);

        auto job = std::make_unique<job_t>(line, line_details_t::wakeup_t());
        auto* jp = job.get();
        auto* slabp = &slab;

        // Fill input blocks from the child section
        if (n < 35) {
          const auto child_shape = get<0>(std_child).shape();
          for (int k=0;k<jp->details.input_blocks;k++) {
            const auto cb = jp->details.input_block(k);
            const int nodes = block_shape(child_shape, cb).product();
            input->read_block(child_id, Vector<int,4>(cb),
                              jp->details.input_block_data(k).slice(0, nodes));
          }
        }

        // Throttle in-flight memory (always allow at least one line)
        {
          std::unique_lock<std::mutex> lock(mu);
          const uint64_t mem = jp->pre.memory_usage;
          cv.wait(lock, [&] { return in_flight==0 || in_flight + mem <= opt.line_memory; });
          in_flight += mem;
        }

        // Wakeup: absorb into slab, free, unblock scheduler
        const_cast_(jp->details.wakeup) = [jp, slabp, &mu, &cv, &in_flight](line_details_t&, unit_t) {
          const auto& line = jp->pre.line;
          for (int k=0;k<line.length;k++)
            slabp->absorb(line.section, line, k, jp->details.output_block_data(k));
          {
            std::lock_guard<std::mutex> lock(mu);
            in_flight -= jp->pre.memory_usage;
          }
          cv.notify_all();
          delete jp;
        };

        schedule_compute_line(jp->details);
        job.release();
      }
      // Wait for this dimension pass to finish before reusing slab for next dim
      threads_wait_all_help();
    }

    writer.write_section(si, S, asarray(slab.data));
    if (opt.progress)
      opt.progress("slice " + std::to_string(n) + " section " + std::to_string(si+1)
                   + "/" + std::to_string(secs.sections.size()));
  }
  threads_wait_all();
}

}  // namespace pgs
