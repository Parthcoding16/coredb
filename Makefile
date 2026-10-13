# Simple build without CMake:  make            -> ./build/coredb
#                              make test       -> builds and runs the test suite
#                              make bench      -> throughput numbers
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread -Isrc
SRCS     := $(shell find src -name '*.cpp' ! -name 'main.cpp')
OBJS     := $(SRCS:src/%.cpp=build/obj/%.o)

all: build/coredb

build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/coredb: $(OBJS) build/obj/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

build/coredb_tests: $(OBJS) tests/test_main.cpp
	$(CXX) $(CXXFLAGS) $(OBJS) tests/test_main.cpp -o $@

build/coredb_bench: $(OBJS) tests/bench.cpp
	$(CXX) $(CXXFLAGS) $(OBJS) tests/bench.cpp -o $@

bench: build/coredb_bench
	./build/coredb_bench

test: build/coredb_tests
	./build/coredb_tests

clean:
	rm -rf build

.PHONY: all test bench clean
