CXX ?= g++
CXXFLAGS ?= -O2 -std=c++20 -Wall -pthread
BINS = dfacomp6 dfacomp7 wtraj6 wtraj7
all: $(BINS)
HDRS = ../src/oware.hpp ../src/layer.hpp mvdfa.hpp
dfacomp6: dfacomp.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=6 -o $@ dfacomp.cpp
dfacomp7: dfacomp.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=7 -o $@ dfacomp.cpp
wtraj6: wtraj.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=6 -o $@ wtraj.cpp
wtraj7: wtraj.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) -DOWARE_ROW=7 -o $@ wtraj.cpp
clean:
	rm -f $(BINS)
