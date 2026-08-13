CXX = g++
CXXFLAGS = -O3 -march=native -fopenmp -std=c++17 -Wall -Wextra

minishobu: solver.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f minishobu
.PHONY: clean
