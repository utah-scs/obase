# Trace workload drivers for CrestKV (Twitter cache traces, Meta kvcache
# traces, and the Meta mixgraph synthetic benchmark). All drivers speak the
# line-framed CrestKV protocol over the UNIX socket or TCP.
#
# Binary names match what loadMetaTraces.sh / runMetaBench.sh invoke.

CXX=g++
CXXFLAGS=-std=c++17 -O2 -pthread -Wall

all: twitterZstLoader.x twitterZstReplay.x kvTraceGenerator kvTraceReplay multiThreadedKvTraceReplay.x meta_bench.x

twitterZstLoader.x: twitterZstLoader.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd

twitterZstReplay.x: twitterZstReplay.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd

kvTraceGenerator: kvTraceGenerator.cc
	$(CXX) $(CXXFLAGS) -o $@ $<

kvTraceReplay: kvTraceReplay.cc
	$(CXX) $(CXXFLAGS) -o $@ $<

multiThreadedKvTraceReplay.x: multiThreadedKvTraceReplay.cc
	$(CXX) $(CXXFLAGS) -o $@ $< -lzstd

# std::barrier needs C++20
meta_bench.x: meta_bench_t.cc
	$(CXX) -std=c++20 -O2 -pthread -Wall -o $@ $<

clean:
	rm -f twitterZstLoader.x twitterZstReplay.x kvTraceGenerator kvTraceReplay multiThreadedKvTraceReplay.x meta_bench.x

.PHONY: all clean
