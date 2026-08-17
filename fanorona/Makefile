CXX ?= g++
CXXFLAGS ?= -std=c++17 -O3 -march=native -Wall -Wextra -pthread
SRC := src/fanorona.cc src/index.cc src/retro.cc src/bns.cc src/main.cc
HDR := src/fanorona.h src/index.h src/retro.h src/bns.h

fanorona: $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -Isrc $(SRC) -o $@

clean:
	rm -f fanorona

.PHONY: clean
