/*
  Fanorona — retrograde WDL table builder.

  Strong-solves a W x H board by processing stone-count layers {a, b}
  (a <= b) in increasing a+b order. Within a layer, states are stored
  white-to-move (see index.h); the value is always for the side to move.

  Per layer, one byte per state in RAM:
    0..250       unresolved: number of children not yet known to be a win
                 for the child (same-layer children + lower-layer draws)
    251          unresolved, true counter in the overflow side map
    252, 253     proved WIN / LOSS
  At the fixpoint, unresolved representatives are draws.

  Cross-layer edges (captures strictly reduce the stone count) are resolved
  at the parent's init from the mmap'd 2-bit value files of completed
  layers. Within-layer edges (paika moves, which are reversible) are
  propagated by generating reverse predecessors when a state resolves;
  the mandatory-capture rule is honored by rejecting predecessors that
  themselves have a capture available.
*/

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "index.h"

namespace fan {

// 2-bit values in .wdl files.
enum WdlValue : uint8_t {
  kUnknown = 0,
  kWin = 1,
  kLoss = 2,
  kDraw = 3,
};

// Counter byte encodings (per-state RAM array during a layer build).
enum CounterEnc : uint8_t {
  kOverflow = 251,  // true count in overflow map
  kCWin = 252,
  kCLoss = 253,
};

// Read-only view over completed layers' value files.
class ValueStore {
 public:
  // Registers the file for a layer; expected_size = ceil(total/4) bytes.
  bool Register(int layer_id, const std::string& path, uint64_t total);
  // 0..3 (WdlValue). Requires the layer to be registered.
  int Get(int layer_id, uint64_t idx) const;
  bool Has(int layer_id) const;

 private:
  struct Region {
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;
  };
  std::unordered_map<int, Region> regions_;
};

struct LayerStats {
  int a = 0, b = 0;
  uint64_t total = 0;      // index-space size (superset when D4)
  uint64_t reps = 0;       // actual representatives processed
  uint64_t wins = 0, losses = 0, draws = 0;
  double seconds = 0.0;
};

class RetroBuilder {
 public:
  RetroBuilder(const Geom& g, const Indexer& ix, const std::string& out_dir,
               int threads);

  // Builds all layers with a+b <= max_stones (default: everything).
  // Completed layers are skipped (resume). Returns false on error.
  bool BuildAll(int max_stones);

  ValueStore& values() { return values_; }

  // Value of a stored-form position from completed layers; kUnknown if the
  // layer was not built.
  int Probe(const Pos& stored_white_to_move);

 private:
  bool BuildLayer(const Layer& l, int layer_id, LayerStats* stats);

  const Geom& g_;
  const Indexer& ix_;
  std::string out_dir_;
  int threads_;
  ValueStore values_;

  std::string LayerPath(const Layer& l) const;
};

// Standalone probe helper: opens all .wdl files found under out_dir for the
// given indexer and answers queries for arbitrary actual positions.
int ProbePosition(const Geom& g, const Indexer& ix, ValueStore& vs,
                  const Pos& actual);

}  // namespace fan
