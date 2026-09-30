CXX ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra -fopenmp
BIN := build
all: $(BIN)/solve $(BIN)/tests $(BIN)/probe $(BIN)/xcheck $(BIN)/explore $(BIN)/explore2 \
     $(BIN)/solve7 $(BIN)/tests7 $(BIN)/probe7 $(BIN)/xcheck7 \
     $(BIN)/gmz $(BIN)/gmz7 $(BIN)/probez $(BIN)/probez7 $(BIN)/layerfix $(BIN)/layerfix7
# gM-only block-compressed tables (.gmz): pack / verify / stats; needs libzstd-dev.
$(BIN)/gmz: src/gmz.cpp src/gmz.hpp src/oware.hpp src/layer.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd
$(BIN)/gmz7: src/gmz.cpp src/gmz.hpp src/oware.hpp src/layer.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=7 -o $@ $< -lzstd
$(BIN)/probez: src/probez.cpp src/gmz.hpp src/oware.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd
$(BIN)/probez7: src/probez.cpp src/gmz.hpp src/oware.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=7 -o $@ $< -lzstd
$(BIN)/%: src/%.cpp src/oware.hpp src/layer.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<
# 7 pits per player (7x2 board): same sources, OWARE_ROW=7.
$(BIN)/%7: src/%.cpp src/oware.hpp src/layer.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=7 -o $@ $<
test: $(BIN)/tests $(BIN)/tests7
	$(BIN)/tests
	$(BIN)/tests7
clean:
	rm -rf $(BIN)
.PHONY: all test clean
