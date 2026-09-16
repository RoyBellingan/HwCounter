# hwCounter - CPU hardware counter harness
#
#   make                 build everything
#   make selfcheck       verify this machine can produce trustworthy numbers
#   make bench           run the Boost.JSON counter benchmark
#   make short / maxy    render the last results
#
# Boost.JSON sources are expected at $(JSON_ROOT); override on the command line:
#   make bench JSON_ROOT=/path/to/boostorg/json

CXX       ?= g++
CXXFLAGS  ?= -std=c++20 -O2 -g -Wall -Wextra
INCLUDES  := -Iinclude
JSON_ROOT ?= ../json
JSON_INC  := -I$(JSON_ROOT)/include
JSON_LIB  ?= -lboost_charconv
DATA      ?= $(JSON_ROOT)/bench/data
OUT       ?= results
PIN       ?= 2
MINMS     ?= 200
REPS      ?= 3

BINS := bench/selfcheck bench/json_perf examples/cache_demo

all: $(BINS)

bench/selfcheck: bench/selfcheck.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

bench/json_perf: bench/json_perf.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(JSON_INC) -o $@ $< $(JSON_LIB)

examples/cache_demo: examples/cache_demo.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

selfcheck: bench/selfcheck
	@./bench/selfcheck

bench: bench/json_perf
	./bench/json_perf --out $(OUT) --min-ms $(MINMS) --reps $(REPS) --pin $(PIN) $(DATA)

short: ; @./tools/report.py short $(OUT).csv
maxy:  ; @./tools/report.py maxy  $(OUT).csv
html:  ; @./tools/report.py html  $(OUT).csv -o $(OUT).html

# Regression gate: compare against a stored baseline, fail over threshold.
THRESH ?= 1.0
diff:  ; @./tools/report.py diff baseline.csv $(OUT).csv -t $(THRESH)

clean:
	rm -f $(BINS) $(OUT).csv $(OUT).machine.json $(OUT).html

.PHONY: all selfcheck bench short maxy html diff clean
