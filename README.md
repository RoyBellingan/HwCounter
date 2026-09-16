# hwCounter — CPU hardware counters for Boost regression testing

In-process hardware event counting via `perf_event_open(2)`, built for
regression-testing C++ libraries. Current target: **Boost.JSON**; intended to
extend to capy and Beast2.

The point: measure *work done*, not wall clock. On a shared CI runner wall time
moves 5–30% for reasons that have nothing to do with your code. Instruction
counts move ~0.01%. You can gate a build on the second; you cannot gate on the
first.

Measured here, two runs of identical code:

```
instructions delta   max 0.015%      <- gate on this
MB/s delta           ±2.5%           <- report only
```

## Quick start

```sh
make json-src                        # Boost.JSON at e93cf9c (UAT baseline)
make
make selfcheck                       # is this machine trustworthy?
make bench                           # run it + write <OUT>.meta.json
make short                           # quick compare
make maxy                            # every counter
make html                            # standalone HTML report
```

## Multi-machine collector

One Beast binary, two subcommands. Runners POST; the collector host also uses
`hwc push` against itself. There is no direct SQLite import.

```sh
# on the collector host
export HWC_TOKEN=change-me
make serve TOKEN="$HWC_TOKEN" BIND=0.0.0.0 PORT=8080
# UI: http://<host>:8080   views: Runs / Stability / Score

# seed the two pre-SHA 6pack CSVs (label=historical, json_sha=unknown)
make seed-historical TOKEN="$HWC_TOKEN" PUSH_URL=http://127.0.0.1:8080

# on every runner, including the collector host
make json-src
make selfcheck
for i in 1 2 3; do
  make bench-push OUT=results/uat-$i \
    JSON_SHA=e93cf9c254619142b9f3cfa9022b93fb1d4e5edb \
    PUSH_URL=http://<collector>:8080 TOKEN="$HWC_TOKEN"
done
```

`POST /api/runs` requires `Authorization: Bearer …` when `--token` is set, and
rejects a body that is missing `json_sha` or `cxxflags`. Reads are open on the
LAN. Payload is one JSON object `{meta, machine, rows}` — not multipart.

SQLite lives at `data/hwc.sqlite` by default (WAL). Volume is small.

### First UAT — pin this SHA

Elect **`e93cf9c254619142b9f3cfa9022b93fb1d4e5edb`** as the baseline. Same
`CXXFLAGS` (`-std=c++20 -O2 -g`) on every box. Three repeats per machine.

Pass criteria, same host + same flags:

- median `|ins/byte|` change across repeats ≈ 0
- worst pool/null row `< 0.3%`
- default-storage rows may show the known ~0.5–1.2% `pass_drift` — surface it
- `GHz` / `MB/s` may move; they are diagnostics

The Stability view colours CV: green `<0.1%`, amber `<1%`, red `≥1%`. Unknown
SHAs are hidden there by default. The Score view is geomean
`base_ins_per_byte / cand_ins_per_byte` (higher = fewer instructions).

## Portability — read this before running on a new machine

**No raw event encodings are hardcoded anywhere.** Every event is an abstract
kernel event (`PERF_TYPE_HARDWARE` / `HW_CACHE` / `SOFTWARE`) that the kernel
maps to whatever the local PMU calls it, and every event is *probed* at startup.
This matters more than it sounds:

- On the Zen2 laptop this was developed on, **9 of 30** catalog events do not
  exist — including every generic `LLC-*` event.
- `cache-misses` on that part resolves to `event=0x64,umask=0x09`, which is an
  **L2** miss, not last-level. On an Intel part the same abstract name is an LLC
  miss. The names are portable; the meanings are not.
- A 7900X3D (Zen4, family 0x19 model 0x61) will expose a different set again,
  and its 3D V-Cache makes L3 numbers incomparable with any non-X3D part.

Consequences, enforced by the tooling rather than left to discipline:

1. An unavailable event produces a **blank cell**, never a zero. A zero would
   silently look like "perfect cache behaviour".
2. Every result set ships with `<out>.machine.json` — CPU model, family/model/
   stepping, kernel, compiler, SMT state, L3 topology, detected PMU slot count,
   the kernel's own event→encoding aliases, and the list of missing events.
3. `report.py diff` compares the two runs' **comparability keys** (vendor,
   family, model, stepping, kernel). If they differ it suppresses cycle and
   cache deltas and reports only instructions, which is the one metric that
   survives a change of machine.
4. PMU slot count is **detected at runtime**, not assumed, by opening N events
   and asking the kernel whether it had to multiplex.

So: gate on instructions across your fleet; treat cycles/cache/TLB as
machine-local diagnostics.

## Multi-pass instead of multiplexing

This machine has 6 usable PMU slots and the catalog wants ~21 events. The
kernel's answer is time-multiplexing, which under-reports and then scales the
estimate back up. That is fine for profiling and not fine for a regression gate.

Instead the workload is run **once per event set** (4 passes here) and results
are stitched by key. `instructions` and `cycles` are re-measured in *every* pass,
which gives a free integrity check: if a row's instruction count differs between
passes, the passes did not execute the same work and the stitch is untrustworthy.
That row is flagged with `!` rather than quietly averaged.

Two columns report this separately:

- `pass_drift` — disagreement between per-pass *best* counts. Stitch integrity.
- `ins_spread` — run-to-run noise within a pass. Machine stability.

This is not theoretical: it caught a genuine ~1% drift on `apache_builds.json`
with default storage (but not with pool storage), because default-storage parse
instruction counts depend on allocator state.

## Do I need to pin?

**Not for correctness.** A task-bound event (`pid=0, cpu=-1`) lives in the task's
`perf_event_context`; on context switch the kernel reads the hardware counter,
accumulates into a 64-bit software total, and reprograms on whatever CPU the task
resumes on. `make selfcheck` verifies this empirically — 0.000% error across a
forced cross-CCX migration.

**Yes for stable cycle/cache numbers.** Measured on this box:

| | instructions | cycles spread | cache-misses |
|---|---|---|---|
| pinned | 100,663,476 | 2.1% | 4,810 |
| roaming | 100,663,476 | 2.3% | 1,775 |
| forced cross-CCX | 100,663,710 | **7.0%** | **9,794** |

Instructions are immovable; cycles triple their spread. Pin within one L3 domain
(`--pin N`) for diagnostics.

One real trap: all of this holds for *task-bound* events. An event opened with
`cpu >= 0` counts whatever runs on that CPU, including other processes, and then
migration genuinely corrupts the result. This harness never does that.

## Layout

```
include/perf/counters.hpp   RAII counter group, grouped read, multiplexing-aware
include/perf/events.hpp     portable catalog, runtime probing, slot detection,
                            pass planning, sysfs alias discovery
include/perf/machine.hpp    machine fingerprint + comparability key
include/perf/session.hpp    multi-pass recording, stitch validation, CSV
include/hwc/                collector: derived metrics, CSV parse, SQLite, HTTP helpers
src/hwc/                    Beast serve + push (one binary: ./hwc)
web/                        Runs / Stability / Score UI (AJAX)
bench/selfcheck.cpp         "can I trust this machine" — run on every new box
bench/json_perf.cpp         Boost.JSON driver (parse pool/default/null, serialize)
examples/cache_demo.cpp     minimal standalone example
tools/report.py             short / maxy / diff / html (local CLI only)
tools/run_bench.sh          json_perf + <prefix>.meta.json
tools/write_meta.py         identity sidecar (json_sha, cxxflags, pin, times)
```

## Output modes

`short` — the four numbers you look at first:

```
                   dataset            impl          op        MB/s    ins/byte       IPC    cyc/byte
        apache_builds.json           boost       parse         147      32.003     2.680      11.939
        apache_builds.json    boost (null)       parse         714       7.841     3.193       2.456
        apache_builds.json    boost (pool)       parse         315      16.792     3.014       5.571
```

`maxy` — 30 columns: every supported raw counter plus derived rates
(`ins/byte`, `cyc/byte`, `IPC`, `br-miss%`, `L1d-miss%`, `cache-miss%`,
`fe-stall%`). Derived metrics are computed in `report.py`, not in C++, so the
raw columns stay exactly as measured and formulas can be revised without
re-running the benchmark.

`diff` — regression gate. Exits non-zero over threshold:

```sh
./tools/report.py diff baseline.csv results.csv -t 1.0
```

`html` — standalone self-contained page, sticky header, no external assets.

Every `make bench` also writes `<prefix>.meta.json` (`json_sha`, `cxxflags`,
pin, timestamps). The collector refuses a push without those two identity
fields.

## CI notes

- Containers need `--cap-add=PERFMON` (older kernels: `SYS_ADMIN`), else
  `perf_event_open` returns `EACCES`. `selfcheck` reports this explicitly.
- Many cloud VMs expose no vPMU at all, or 1–2 slots. `selfcheck` warns and the
  pass count rises accordingly.
- Re-baseline in a dedicated commit so the diff shows who moved the number.

## Session notes

See **HANDOFF.md** for current state, findings, open questions and next steps.

## Where this goes next (capy / Beast2)

The threading model is the open question. A counter opened on thread A does not
follow work that runs on thread B. For capy's executor/coroutine model, either
force a single-threaded executor for measurement, or open a `counter_group` per
worker and sum. `inherit=1` covers spawned children but is incompatible with
`PERF_FORMAT_GROUP`, so grouped reads and inheritance are mutually exclusive —
per-thread groups is the better path.

For I/O-shaped work like Beast2, add the software events (`context-switches`,
`page-faults`); they cost no PMU slot and are always available.
