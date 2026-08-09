CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

nocca: nocca.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f nocca

.PHONY: clean
