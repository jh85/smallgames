# Pentago two-tier strong solver — build
#
# Reuses girving's base/end/mid kernels (girving/ submodule, BSD licensed);
# adds a locality-aware single-machine out-of-core retrograde engine (src/).

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -march=native -Wall -Wno-deprecated -Wno-volatile
GIRVING  := girving
GEN      := build/gen
INCLUDES := -I$(GIRVING) -Ithird_party -I$(GEN) -Isrc
LDFLAGS  := -lz -lpthread

# Generated tables (from girving's precompute)
TABLES := $(GEN)/pentago/base/gen/tables.cc $(GEN)/pentago/base/gen/halfsuper_wins.h

UTIL_SRCS := $(filter-out %_test.cc %wasm_alloc.cc,$(wildcard $(GIRVING)/pentago/utility/*.cc))

LIB_SRCS := \
  $(GIRVING)/pentago/base/all_boards.cc $(GIRVING)/pentago/base/board.cc \
  $(GIRVING)/pentago/base/count.cc $(GIRVING)/pentago/base/hash.cc \
  $(GIRVING)/pentago/base/moves.cc $(GIRVING)/pentago/base/score.cc \
  $(GIRVING)/pentago/base/section.cc $(GIRVING)/pentago/base/superscore.cc \
  $(GIRVING)/pentago/base/symmetry.cc $(GEN)/pentago/base/gen/tables.cc \
  $(GIRVING)/pentago/end/sections.cc $(GIRVING)/pentago/end/compute.cc \
  $(GIRVING)/pentago/end/line.cc $(GIRVING)/pentago/end/history.cc \
  $(GIRVING)/pentago/end/options.cc \
  $(GIRVING)/pentago/mid/midengine.cc $(GIRVING)/pentago/mid/halfsuper.cc \
  $(GIRVING)/pentago/high/board.cc \
  $(UTIL_SRCS) \
  src/slice_file.cc src/engine.cc

LIB_OBJS := $(patsubst %.cc,build/obj/%.o,$(LIB_SRCS))
UTIL_OBJS := $(patsubst %.cc,build/obj/%.o,$(UTIL_SRCS))
REST_OBJS := $(filter-out $(UTIL_OBJS),$(LIB_OBJS))

BINS := build/retro build/wdl build/midsolve build/verify_forward build/verify_counts build/verify_sparse

all: $(BINS)

# --- generated tables ---
build/precompute: $(GIRVING)/pentago/base/precompute.cc $(UTIL_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(TABLES): build/precompute
	mkdir -p $(GEN)/pentago/base/gen
	build/precompute $(GEN)/pentago/base/gen/tables.h $(GEN)/pentago/base/gen/tables.cc \
	  $(GEN)/tables.js $(GEN)/pentago/base/gen/halfsuper_wins.h

# --- object rules (utility must not depend on generated tables) ---
$(UTIL_OBJS): build/obj/%.o: %.cc
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(REST_OBJS): build/obj/%.o: %.cc $(TABLES)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# --- binaries ---
build/%: src/%.cpp $(LIB_OBJS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< $(LIB_OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf build

.PHONY: all clean
