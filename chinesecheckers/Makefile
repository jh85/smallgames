CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra
LDFLAGS  ?= -pthread

BIN = src/solver src/bruteforce src/query src/crosscheck src/rulecount \
      src/zddbench tests/unit_tests

all: $(BIN)

src/solver: src/solver.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

src/bruteforce: src/bruteforce.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

src/query: src/query.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

src/crosscheck: src/crosscheck.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

src/rulecount: src/rulecount.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

src/zddbench: src/zddbench.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

tests/unit_tests: tests/unit_tests.cpp src/cc_core.hpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDFLAGS)

test: tests/unit_tests
	./tests/unit_tests

# Reproduce the fast Table-1 validations (a few minutes total).
validate-small: src/solver
	./src/solver --m 7 --p 1 --dir runs/m7p1 --ckpt-mins 0
	./src/solver --m 7 --p 2 --dir runs/m7p2 --ckpt-mins 0
	./src/solver --m 7 --p 3 --dir runs/m7p3 --ckpt-mins 0
	./src/solver --m 4 --p 6 --dir runs/m4p6 --ckpt-mins 0

# Larger validations (minutes to ~10 minutes each on 64 threads).
validate-mid: src/solver
	./src/solver --m 5 --p 6 --dir runs/m5p6 --ckpt-mins 0
	./src/solver --m 7 --p 4 --areas-sym --dir runs/m7p4sym --ckpt-mins 0

# The full 6x6 solve (~145 GB RAM, several hours).
solve-6x6: src/solver
	./src/solver --m 6 --p 6 --dir runs/m6p6 --ckpt-mins 60

clean:
	rm -f $(BIN)

.PHONY: all test validate-small validate-mid solve-6x6 clean
