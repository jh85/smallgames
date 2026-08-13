// retro: compute Pentago WDL slices, from --hi down to --lo.
//
// Usage:
//   retro --dir DIR --hi 35 --lo 18 [--cache GB] [--line-memory GB]
//         [--section-memory GB] [--level 6] [--threads N]
//
// Files are DIR/slice-N.pgs; slice hi+1 must already exist and be complete
// (unless hi == 35).  Existing partial files are resumed automatically.

#include "engine.h"
#include "pentago/utility/thread.h"
#include "pentago/utility/log.h"
#include <cstring>
#include <cstdlib>
#include <iostream>

using namespace pgs;
using namespace pentago;

static uint64_t parse_gb(const char* s) {
  return (uint64_t)(atof(s)*(1ull<<30));
}

int main(int argc, char** argv) {
  std::string dir = ".";
  int hi = 35, lo = 35;
  engine_options_t opt;
  int threads = 0;
  for (int i=1;i<argc;i++) {
    const char* a = argv[i];
    auto next = [&]() { return argv[++i]; };
    if (!strcmp(a,"--dir")) dir = next();
    else if (!strcmp(a,"--hi")) hi = atoi(next());
    else if (!strcmp(a,"--lo")) lo = atoi(next());
    else if (!strcmp(a,"--cache")) opt.cache_bytes = parse_gb(next());
    else if (!strcmp(a,"--line-memory")) opt.line_memory = parse_gb(next());
    else if (!strcmp(a,"--section-memory")) opt.section_memory = parse_gb(next());
    else if (!strcmp(a,"--level")) opt.zlib_level = atoi(next());
    else if (!strcmp(a,"--threads")) threads = atoi(next());
    else {
      std::cerr << "unknown option: " << a << "\n"
        "usage: retro --dir DIR --hi N --lo M [--cache GB] [--line-memory GB]\n"
        "             [--section-memory GB] [--level L] [--threads N]\n";
      return 1;
    }
  }
  if (hi < lo || lo < 0 || hi > 35) {
    std::cerr << "need 0 <= lo <= hi <= 35\n";
    return 1;
  }

  try {
    init_threads(threads ? threads : default_threads(), 1);
    opt.progress = [](const std::string& s) { std::cerr << s << std::endl; };
    const auto by_slice = all_sections();
    for (int n = hi; n >= lo; n--)
      compute_slice(n, dir, by_slice, opt);
    shutdown_threads();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
}
