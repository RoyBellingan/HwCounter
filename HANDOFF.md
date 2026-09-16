# hwCounter — state of play

Working notes for picking this up again. Last session: 2026-09-16.

Goal: hardware-counter based regression testing for Boost libraries. Boost.JSON
first, then capy, then Beast2. Replace/augment wall-clock benchmarking (and the
callgrind workflow) with counters that are stable enough to gate CI on.

Todo:

* test other allocator (mimalloc)

* to proove that this is actually working, further test 2/3 case after gprof (should reduce branching) or other form of profiling


---

## 0. Do this first tomorrow

**The Boost.JSON clone was in the session scratchpad and is gone.** Re-clone it,
and this time somewhere permanent:

```sh
cd /home/roy/Public
git clone --depth 1 -b develop https://github.com/boostorg/json.git json
cd hwCounter && make        # Makefile defaults to JSON_ROOT=../json
```

Also worth doing: **this is not a git repo yet.** `git init` before further
changes — there is real work here now and no history.

```sh
cd /home/roy/Public/hwCounter && git init && git add -A && git commit -m "hwCounter: initial"
```

---

## 1. Where everything is

```
include/perf/counters.hpp   RAII counter group; grouped atomic read; multiplexing-aware
include/perf/events.hpp     portable event catalog, runtime probe, PMU slot detection,
                            multi-pass planning, sysfs alias discovery
include/perf/machine.hpp    machine fingerprint + comparability key
include/perf/session.hpp    multi-pass recording, stitch validation, CSV emit
bench/selfcheck.cpp         "can I trust this machine" — run on every new box
bench/json_perf.cpp         Boost.JSON driver (parse pool/default/null, serialize)
examples/cache_demo.cpp     minimal standalone example (sequential vs pointer chase)
tools/report.py             short / compare / maxy / diff / html
Makefile                    all targets
README.md                   the primer + design rationale
results/baseline/           the two runs captured so far
results/counters.html       the published page (source of the artifact)
```

Published page (private, shareable from its share menu):
**https://claude.ai/artifact/DC41eRiZ5bvG6pD75qCKn5**

To update it from a future session, pass that URL as `url` to the Artifact tool —
publishing without it creates a *separate* artifact instead of updating this one.

---

## 2. How to run

```sh
make selfcheck                       # verify the machine first, always
make bench                           # full run, all 14 datasets (~50s at MINMS=60)
make short                           # quick table
make maxy                            # all 30 columns
make html                            # standalone page

# knobs
make bench OUT=results/foo MINMS=400 REPS=4 PIN=2 JSON_ROOT=../json
./tools/report.py diff results/baseline/runB-idle.csv results/foo.csv -t 1.0
```

`diff` exits non-zero when any row regresses past the threshold — that is the CI
gate. Default threshold 1.0 % on `ins/byte`.

---

## 3. Architecture and *why* it is built this way

### 3.1 No hardcoded event encodings, ever

Every event is an abstract kernel event (`PERF_TYPE_HARDWARE` / `HW_CACHE` /
`SOFTWARE`) that the kernel maps to the local PMU, and every one is **probed** at
startup. This is the single most important constraint, because the fleet is
heterogeneous (Zen2 laptop today, 7900X3D tomorrow, unknown servers live).

Concretely, on the Zen2 dev box **9 of 30 catalog events do not exist**:

```
bus-cycles  ref-cycles  stalled-cycles-be  L1d-write
LLC-read  LLC-read-miss  LLC-write  LLC-write-miss  dTLB-write-miss
```

Every generic `LLC-*` event is missing. And `cache-misses` here resolves to
`event=0x64,umask=0x09`, which on this part is an **L2** miss — on Intel the same
abstract name is an LLC miss. *The names are portable; the meanings are not.*

Enforced consequences:

1. A missing event writes a **blank cell, never a zero**. A zero would read as
   perfect cache behaviour and silently corrupt any comparison.
2. Every run emits `<out>.machine.json` with family/model/stepping, kernel,
   compiler, SMT state, governor, boost state, L3 topology, detected PMU slots,
   the kernel's own event→encoding aliases, and the missing-event list.
3. `report.py diff` compares **comparability keys**
   (`vendor|family|model|stepping|kernel`). When they differ it suppresses cycle
   and cache deltas and reports only instructions.
4. PMU slot count is **detected at runtime** (open N events, ask the kernel
   whether it multiplexed), not assumed. Found 6 on this box.

### 3.2 Multi-pass instead of multiplexing

21 supported events, 6 slots. The kernel's answer is time-multiplexing, which
under-reports and scales the estimate back up — acceptable for profiling, not for
a gate. Instead the workload runs **once per event set** and results are stitched
by `(dataset, impl, op)`.

Current plan on this machine (4 passes):

```
pass 1  instructions cycles branches branch-misses cache-references cache-misses
pass 2  instructions cycles stalled-cycles-fe L1d-read L1d-read-miss L1d-prefetch
pass 3  instructions cycles L1i-read-miss dTLB-read dTLB-read-miss iTLB-read-miss
pass 4  instructions cycles BPU-read BPU-read-miss
```

`instructions` and `cycles` ride in **every** pass. That is the integrity check:
the same workload must retire the same instruction count in each pass, so
disagreement means the passes did not execute the same work and the stitch is
invalid. Two separate columns report this:

- `pass_drift` — disagreement between per-pass *best* counts → stitch integrity.
- `ins_spread` — rep-to-rep noise within a pass → machine stability.
- `cyc_spread` — same, for cycles. Since instructions are fixed per row, this
  **is** IPC stability. Surfaced in reports as `IPC stab%`.

Iteration counts are calibrated **once per workload, before any pass runs**.
Calibrating per pass makes each pass do different work and shows up as fake drift
— that bug already happened and is fixed; don't reintroduce it.

### 3.3 Derived metrics live in report.py, not C++

Raw counter columns stay exactly as measured. `ins/byte`, `cyc/byte`, `IPC`,
`GHz`, `br-miss%`, `L1d-miss%`, `cache-miss%`, `fe-stall%` are computed in the
reporter so formulas can be revised without re-running the benchmark.

`GHz` is worth calling out: it is `cycles / task-clock-ns`, both of which we
already count. It reads the achieved clock straight off the counters, which makes
throttling a *column* rather than an inference. No extra measurement needed.

---

## 4. Findings so far

### 4.1 The headline: instructions vs wall clock

Two runs of the **identical binary** over the same 14 datasets, 56 rows each.
Run A was taken while the box was compiling and hot; run B on an idle box.
Nothing about the code changed.

| metric | median change | worst row |
|---|---|---|
| `ins/byte` | **0.000 %** | 0.278 % |
| `cyc/byte` | 1.24 % | 19.2 % |
| `GHz` | 139.8 % | 141.0 % |
| `MB/s` | 137.9 % | 161.9 % |

Run A ran at **1.745 GHz**, run B at **4.183 GHz** (5700U: 1.8 base, 4.3 boost).

The clock is the whole explanation. `GHz` and `MB/s` moved together to within two
points while `cyc/byte` barely moved — the machine executed *identical work* at a
2.4× different rate. The throughput number was measuring the clock, not the code.

**Therefore: gate on `ins/byte`. Everything cycle-denominated is a diagnostic.**

### 4.2 Default storage is not instruction-deterministic

Three rows consistently flag `pass_drift` > 0.5 %, always the same ones, always
`boost` **default storage**, never `boost (pool)`:

```
apache_builds.json   ~1.2 %
citm_catalog.json    ~0.5 %
github_events.json   ~1.2 %
```

`ins_spread` for those rows is ~0.06 %, so it is not rep-to-rep noise — it is a
systematic pass-to-pass difference. Hypothesis: general-purpose allocator state
(arena layout / free-list shape) leaks into the instruction count, and it evolves
monotonically as the process runs. Pool storage shows no such effect.

**Open question, worth chasing:** this is either a real property worth documenting
about `json::parse` with default storage, or an artifact of running the passes
back-to-back in one process. Test by running each pass in a **fresh process** and
seeing whether the drift disappears. If it persists, the gate threshold for
default-storage rows needs to be looser than for pool rows — or those rows should
be gated on pool storage only.

Related and unexplained: the allocator is roughly *half* the parse cost
(`apache_builds`: 7.8 → 16.8 → 32.1 ins/byte for null → pool → default), and it is
also the only thing producing drift. Probably the same story. Look at the `L1d-*`
and `cache-*` columns in the Full view.

### 4.3 Migration does not lose counts; pinning is for variance only

A task-bound event (`pid=0, cpu=-1`) lives in the task's `perf_event_context`. On
context switch the kernel reads the hardware counter, accumulates into a 64-bit
software total, and reprograms on whatever CPU the task resumes on. Verified
empirically — `selfcheck` forces a cross-CCX migration and measures **0.000 %**
error.

But migration does change microarchitectural state:

| | instructions | cycles spread | cache-misses |
|---|---|---|---|
| pinned | 100,663,476 | 2.1 % | 4,810 |
| roaming | 100,663,476 | 2.3 % | 1,775 |
| forced cross-CCX | 100,663,710 | **7.0 %** | **9,794** |

So: no pinning needed for instruction gating; pin within one L3 domain for stable
cycle/cache diagnostics. This box has two L3 domains (cpus 0-7, 8-15).

**Trap to remember:** all of the above is true for *task-bound* events only. An
event opened with `cpu >= 0` counts whatever runs on that CPU, including other
processes, and migration then genuinely corrupts the result. The harness never
does this — keep it that way.

---

## 5. Machine currently used

```
AMD Ryzen 7 5700U (Zen2, family 23 model 104 stepping 1)
kernel 7.1.3-1-default, gcc 16.1.1
16 logical CPUs, SMT on, two L3 domains (0-7, 8-15)
perf_event_paranoid = 2   → user-mode self-profiling, no root needed
cpufreq governor = powersave, boost = 1
PMU slots detected: 6
```

`selfcheck` verdict: OK for regression gating, 3 warnings (SMT on, powersave
governor, multiple L3 domains) — all affecting cycle/cache stability only.

---

## 6. Next steps, roughly in priority order

### 6.1 The pending experiment (user was mid-flight on this)

Compare IPC/clock stability under the `performance` governor, including
thermal throttle. Needs sudo, so run it manually:

```sh
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# three back-to-back runs so the part heats up between them
for i in 1 2 3; do
  ./bench/json_perf --out results/perf$i --min-ms 400 --reps 4 --pin 2 ../json/bench/data
done
./tools/report.py diff results/perf1.csv results/perf3.csv
```

Expect: `GHz` sags between run 1 and run 3, `ins/byte` holds. Then add
`results/perf3.csv` as a third series on the strip plot in `counters.html` so the
page shows hot / idle / throttled together.

Caveat already established: this box is *already* hitting 4.17 GHz under
`powersave` because `amd-pstate` boosts regardless of governor. The real lever is
sustained load and thermals, so the three-run loop matters more than the governor
switch itself.

### 6.2 Known limitation blocking throttle study

The session keeps the **best** (lowest) sample per row, which deliberately
suppresses in-run throttling. Correct for gating, wrong for studying throttle. To
watch the clock decay *within* a run, add a per-rep time series that records every
sample instead of only the minimum. Not built yet.

### 6.3 Wire up the other implementations

Upstream `bench.cpp` also benchmarks `rapidjson` (pool and CRT) and `nlohmann`.
Not wired up here — `bench/clone.sh` in the Boost.JSON tree fetches them:

```sh
cd ../json/bench && ./clone.sh     # clones rapidjson + nlohmann into lib/
```

Then add impls to `bench/json_perf.cpp` mirroring the existing four. Straight-
forward; the `impl_def` table and the task-list loop are the only places to touch.

### 6.4 Integrate with the existing CI benchmark page

Current pipeline produces
`https://benchmark.cppalliance.org/jsonbenchmarks-pullrequests/<PR>/pullrequest.html`.
Two options, not yet decided:

- **(a)** Patch upstream `bench.cpp` to emit counters directly — hook point is the
  `f()` call inside `bench()` around `vi[j].get()->bench(verb, vf[i], repeat)`,
  wrapping the whole `run_for` so counts cover `result.calls` invocations.
- **(b)** Keep `json_perf` as a separate driver and publish a second page.

(b) is lower risk and is what exists now. (a) gets counters for *all* the
implementations for free, including rapidjson/nlohmann, and reuses upstream's
trial/discard statistics.

### 6.5 capy and Beast2 — the threading problem

This is the real unsolved piece. A counter opened on thread A does not follow work
that runs on thread B. Options:

- Force a single-threaded executor for measurement runs. Simplest, least
  representative.
- Open a `counter_group` per worker thread and sum. More work, correct.
- `inherit=1` covers spawned children **but is incompatible with
  `PERF_FORMAT_GROUP`** — grouped reads and inheritance are mutually exclusive.
  Since grouping is what makes ratios honest, per-thread groups is the better path.

For I/O-shaped work like Beast2, add the software events
(`context-switches`, `page-faults`, `cpu-migrations`) — they cost no PMU slot and
are always available.

### 6.6 Smaller items

- CI containers need `--cap-add=PERFMON` (older kernels `SYS_ADMIN`) or
  `perf_event_open` returns `EACCES`. `selfcheck` already reports this.
- Many cloud VMs expose no vPMU or only 1–2 slots → many passes, long runs.
  `selfcheck` warns; decide a policy for those runners.
- Baseline storage: store per-benchmark `ins/byte` baselines as JSON in-repo,
  re-baseline in dedicated commits so the diff shows who moved the number.
- `report.py` has a `compare` column set used by the HTML page but no CLI mode
  for it — add `report.py compare a.csv b.csv` if useful.

---

## 7. Traps already hit (do not re-introduce)

1. **Per-pass rep calibration** → fake instruction drift of 117 %. Calibrate once,
   before the pass loop.
2. **Storing raw totals instead of per-call** → same symptom. Normalise at record
   time in `session::measure`.
3. **Compiler CSE of two identical workload calls** in `selfcheck` made the
   migration test read 45 % error and falsely fail. Fixed with `noinline` + a
   distinct seed per call + an `asm volatile` barrier. Any new microbenchmark
   helper needs the same treatment.
4. **`json::stream_parser` is not a null parser** — it still builds a DOM, so the
   "null" impl initially showed zero saving vs. full parse. Must use
   `basic_parser<null_handler>` (mirrors upstream's `null_parser`). If porting
   other impls across, verify each actually does what its label claims; the
   counters will measure the wrong thing very precisely.
5. **`guess_microarch` had dead code** that would have mislabelled the 7900X3D.
   Zen3 is family 0x19 models ≤0x0f and 0x20–0x5f; Zen4 is 0x10–0x1f and ≥0x60
   (Raphael 0x61 = 7900X3D).

---

## 8. Open questions

- Is the default-storage drift real allocator behaviour or a same-process
  artifact? (§4.2 — test with fresh process per pass.)
- Should `cpufreq_governor` / `boost` join the comparability key? They are
  recorded but not part of it. Arguably yes for cycle comparisons, no for
  instruction comparisons — maybe a two-tier key.
- What threshold for the CI gate? 1.0 % is a placeholder. Needs to survive
  compiler upgrades without masking real regressions. Suggest measuring the
  distribution across a few gcc/clang versions before fixing it.
- Do we gate per-row, or on an aggregate? Per-row catches localised regressions
  but is noisier in aggregate CI signal.
