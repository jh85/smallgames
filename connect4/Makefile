CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

c4: c4.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f c4
.PHONY: clean
