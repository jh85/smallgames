CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

dobutsu: dobutsu.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f dobutsu

.PHONY: clean
