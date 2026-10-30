CXX ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++17 -Wall -Wextra -Wpedantic

twixt: twixt.cpp symbolic_dfa.hpp twixt_symbolic.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

symbolic_dfa_test: symbolic_dfa_test.cpp symbolic_dfa.hpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test: twixt symbolic_dfa_test
	./symbolic_dfa_test
	./twixt --self-test
	./twixt --size 3 --expect draw --quiet
	./twixt --size 3 --strong --no-draw-pruning --expect draw \
		--census-max-states 100000 --census-seconds 0
	./twixt --size 3 --strong --no-draw-pruning --expect draw \
		--census-max-states 100000 --census-seconds 0 \
		--db-out .test.twixtdb
	./twixt --probe-db .test.twixtdb --moves B2 --quiet
	rm -f .test.twixtdb
	./twixt --board 5x6 --census-ply 4 \
		--census-max-states 100000 --census-seconds 0
	python3 test_symbolic.py

clean:
	rm -f twixt symbolic_dfa_test .test.twixtdb

.PHONY: test clean
