CXX = g++
CXXFLAGS = -O3 -march=native -std=c++17 -fopenmp -Wall -Wextra

qwdl: quoridor_wdl.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: qwdl
	./qwdl selftest 3 3 2

clean:
	rm -f qwdl

.PHONY: test clean
