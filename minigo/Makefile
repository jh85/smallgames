CXX = g++
CXXFLAGS = -O3 -march=native -std=c++17 -Wall -Wextra -pthread

minigo: minigo.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: minigo
	./minigo selftest

clean:
	rm -f minigo

.PHONY: test clean
