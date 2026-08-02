CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -pthread -Wall -Wextra
SAN      := -std=c++17 -g -O1 -pthread -fsanitize=thread

BINS := server_phase5 origin_server benchmark concurrency_test test_lru_cache test_http
PHASES := server_base_version server_phase2 server_phase3 server_phase4

.PHONY: all phases test bench clean

all: $(BINS)

server_phase5: server_phase5.cpp http.hpp lru_cache.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

origin_server: origin_server.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

benchmark: benchmark.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

concurrency_test: concurrency_test.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

test_lru_cache: test_lru_cache.cpp lru_cache.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

test_http: test_http.cpp http.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

# The earlier phases are kept as the development history of the design.
phases: $(PHASES)
$(PHASES): %: %.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

# ThreadSanitizer build: the cache and the connection state are touched by the
# whole thread pool, so "it did not crash" is not evidence of thread safety.
tsan: test_lru_cache.cpp lru_cache.hpp
	$(CXX) $(SAN) test_lru_cache.cpp -o test_lru_cache_tsan

test: test_lru_cache test_http
	./test_http
	./test_lru_cache

bench: all
	./run_benchmarks.sh

clean:
	rm -f $(BINS) $(PHASES) test_lru_cache_tsan *.log RESULTS_raw.txt
