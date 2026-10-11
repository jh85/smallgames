CXX = g++
CXXFLAGS = -O3 -march=native -std=c++17 -fopenmp -Wall -Wextra

qwdl: quoridor_wdl.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

qwdl_sym: quoridor_wdl_sym.cpp quoridor_wdl.cpp
	$(CXX) $(CXXFLAGS) -o $@ quoridor_wdl_sym.cpp

test: qwdl
	./qwdl selftest 3 3 2

test-sym: qwdl_sym
	./qwdl_sym selftest 3 3 2

clean:
	rm -f qwdl qwdl_sym

.PHONY: test test-sym clean
