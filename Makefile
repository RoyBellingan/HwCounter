# hwCounter - CPU hardware counter harness + Beast collector
#
#   make                 build benches + hwc
#   make selfcheck       verify this machine can produce trustworthy numbers
#   make json-src        clone/checkout Boost.JSON at JSON_SHA
#   make bench           run json_perf and write <OUT>.meta.json
#   make bench-push      bench, then POST to the collector
#   make serve           start the Beast collector (needs TOKEN or HWC_TOKEN)
#   make test            build and run the Boost.Test unit tests
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
LABEL     ?=
NOTE      ?=

JSON_SHA  ?= e93cf9c254619142b9f3cfa9022b93fb1d4e5edb
JSON_REF  ?= uat-baseline
JSON_URL  ?= https://github.com/boostorg/json.git

PUSH_URL  ?= http://127.0.0.1:8080
# The token goes to hwc through the environment, not argv, so `ps` does not
# show it. OPEN_WRITES=1 starts the collector without a token.
TOKEN     ?= $(HWC_TOKEN)
OPEN_WRITES ?=
WEB       ?= web
DB        ?= data/hwc.sqlite
PORT      ?= 8080
BIND      ?= 0.0.0.0

export JSON_ROOT JSON_SHA JSON_REF CXX CXXFLAGS PIN MINMS REPS LABEL NOTE OUT DATA

BINS := bench/selfcheck bench/json_perf examples/cache_demo hwc test/unit_tests

all: bench/selfcheck bench/json_perf examples/cache_demo hwc

bench/selfcheck: bench/selfcheck.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

bench/json_perf: bench/json_perf.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(JSON_INC) -o $@ $< $(JSON_LIB)

examples/cache_demo: examples/cache_demo.cpp $(wildcard include/perf/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $<

HWC_SRC := src/hwc/main.cpp src/hwc/serve.cpp src/hwc/push.cpp src/hwc/store.cpp src/hwc/json_src.cpp
hwc: $(HWC_SRC) $(wildcard include/hwc/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -pthread -o $@ $(HWC_SRC) -lsqlite3 $(JSON_LIB)

HWC_LIB_SRC := src/hwc/store.cpp src/hwc/json_src.cpp
test/unit_tests: test/unit_tests.cpp $(HWC_LIB_SRC) $(wildcard include/perf/*.hpp) $(wildcard include/hwc/*.hpp)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ test/unit_tests.cpp $(HWC_LIB_SRC) -lsqlite3 $(JSON_LIB)

test: test/unit_tests
	@./test/unit_tests --log_level=message

selfcheck: bench/selfcheck
	@./bench/selfcheck

json-src:
	@if [ ! -d "$(JSON_ROOT)/.git" ]; then \
	  git clone "$(JSON_URL)" "$(JSON_ROOT)"; \
	fi
	git -C "$(JSON_ROOT)" fetch --depth 1 origin "$(JSON_SHA)"
	git -C "$(JSON_ROOT)" checkout --detach FETCH_HEAD

bench: bench/json_perf
	@test -d "$(DATA)" || { echo "missing $(DATA) — run: make json-src"; exit 1; }
	./tools/run_bench.sh

bench-push: bench hwc
	@echo './hwc push --url "$(PUSH_URL)" "$(OUT)"'
	@HWC_TOKEN="$(TOKEN)" ./hwc push --url "$(PUSH_URL)" "$(OUT)"

serve: hwc
	@echo './hwc serve --db "$(DB)" --bind "$(BIND)" --port "$(PORT)" --web "$(WEB)"'
	@HWC_TOKEN="$(TOKEN)" ./hwc serve --db "$(DB)" --bind "$(BIND)" --port "$(PORT)" \
	  --web "$(WEB)" $(if $(OPEN_WRITES),--allow-open-writes)

seed-historical: hwc
	@HWC_TOKEN="$(TOKEN)" ./tools/seed_historical.sh "$(PUSH_URL)"

short: ; @./tools/report.py short $(OUT).csv
maxy:  ; @./tools/report.py maxy  $(OUT).csv
html:  ; @./tools/report.py html  $(OUT).csv -o $(OUT).html

THRESH ?= 1.0
MAXDRIFT ?= 0.5
diff:  ; @./tools/report.py diff baseline.csv $(OUT).csv -t $(THRESH) --max-drift $(MAXDRIFT)

clean:
	rm -f $(BINS) $(OUT).csv $(OUT).machine.json $(OUT).meta.json $(OUT).html

.PHONY: all test selfcheck json-src bench bench-push serve seed-historical short maxy html diff clean
