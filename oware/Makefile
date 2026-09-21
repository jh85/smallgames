CXX ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra -fopenmp
BIN := build
all: $(BIN)/solve $(BIN)/tests $(BIN)/probe $(BIN)/xcheck $(BIN)/explore $(BIN)/explore2
$(BIN)/%: src/%.cpp src/oware.hpp src/layer.hpp
	@mkdir -p $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ $<
test: $(BIN)/tests
	$(BIN)/tests
clean:
	rm -rf $(BIN)
.PHONY: all test clean
