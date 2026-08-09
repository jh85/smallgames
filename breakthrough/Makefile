CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

bt: bt.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f bt

.PHONY: clean
