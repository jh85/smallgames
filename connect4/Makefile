CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

all: c4 c4count

c4: c4.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

c4count: c4count.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f c4 c4count
.PHONY: all clean
