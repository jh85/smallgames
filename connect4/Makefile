CXX = g++
CXXFLAGS = -O3 -march=native -funroll-loops -std=c++20 -Wall -Wextra -pthread

all: c4 c4count c4pack c4_wdl_query

c4: c4.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

c4count: c4count.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

c4pack: c4pack.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd -lcrypto

c4_wdl_query: c4_wdl_query.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd

clean:
	rm -f c4 c4count c4pack c4_wdl_query
.PHONY: all clean
