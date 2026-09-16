# hwCounter code review

Date: 2026-09-16
Scope: whole tree, including uncommitted collector / web files.
Purpose: notes for the 2026-09-17 review. Not a patch list.

**Verdict:** the measurement core is sound. Do not throw it away. Do not gate
CI on `ins/byte` until identity matching (Score + comparability key) is fixed.
Do not treat Stability CV as evidence until spread math is fixed.

19 findings: 4 high, 8 medium, 7 low.

---

## What holds

- Abstract kernel events only. No hardcoded raw encodings.
- Probe failures become blank cells, never zeros.
- Calibration runs once, before the pass loop (the 117% fake-drift bug is gone).
- `instructions` and `cycles` ride every pass as a stitch check.
- Groups are task-bound (`pid=0, cpu=-1`), never CPU-bound.
- `exclude_kernel=1` matches `perf_event_paranoid <= 2`.
- Derived rates live in `report.py` / `derived.hpp`, not in the C++ counters.
- `selfcheck` is the right first step on a new box.
- Collector requires `json_sha` and `cxxflags` on insert.
- Historical seeds use `json_sha=unknown`; Stability hides them.
- `suggest()` SQL is column-whitelisted and LIKE-escaped.
- Documented traps match the code: CSE in selfcheck, `stream_parser` ≠ null,
  Zen3/Zen4 model split, inherit vs `PERF_FORMAT_GROUP`.

---

## High — can lie to the gate

### H1. Score baseline picker ignores `cxxflags`

Where: `src/hwc/store.cpp` — `Store::compare`, `pick_base`

Default baseline is “latest UAT SHA on the same host + compiler”. Flags are
not required to match. An `-O2` run compared with `-O3` (or a stray
`CXXFLAGS`) looks like a library regression on the gate metric.

Fix: require `cxxflags` (and ideally compiler version) to match unless the
user picked both run ids. Surface a warning in the JSON when they do not.

### H2. Compiler is missing from the comparability key

Where: `include/perf/machine.hpp` — `comparability_key()`; `tools/report.py diff`

The key is `vendor|family|model|stepping|kernel`. Instructions are the CI
gate and they move when the compiler moves. `report.py diff` will treat a
gcc bump as a Boost.JSON regression and will still emit cycle deltas as if
the machines were comparable.

Fix: two-tier key.

- Instructions comparable iff compiler + `cxxflags` match.
- Cycles / cache comparable iff the current machine key also matches.

Put compiler in the instruction tier now. Governor / boost can wait
(HANDOFF §8).

### H3. `ins_spread` / `cyc_spread` depend on sample order

Where: `include/perf/session.hpp` — `session::measure`, `track()`

Spread is `max |v − running_min| / running_min`, then the min is updated. A
noisy sample before the best under-reports. The same values in another order
produce a different number. Stability advice based on this column is not
reproducible.

Fix: store min and max (or all reps) per pass. Compute `(max−min)/min` after
the last rep.

### H4. Derived ratios mix independent minima

Where: `include/perf/session.hpp` `measure()`; `include/hwc/derived.hpp`;
`tools/report.py` `derived()`

Wall time, instructions, and cycles each keep their own minimum across reps.
`IPC = min(ins)/min(cyc)` and `GHz = min(cyc)/task-clock` can be a pair that
never occurred. Fine as a per-event floor; misleading as a ratio.

Fix: keep the best-time sample’s full counter vector as the published row.
Report per-event mins only as diagnostics, or compute IPC / GHz from a
single sample.

---

## Medium — UAT and CI

### M1. Static-file prefix check is not a path boundary

Where: `src/hwc/serve.cpp` — `serve_static()`

`full.string().rfind(root.string(), 0) == 0` treats `/tmp/website` as inside
`/tmp/web`. Combined with `weakly_canonical` this is a directory-prefix
bypass if `--web` is a short path next to a similarly named tree.

Fix: compare with a trailing separator, or walk `parent_path()` until it
equals root.

### M2. Run metadata is interpolated into `innerHTML`

Where: `web/app.js` — `fillRunsTable`, `showRun`, `loadStability`,
`compareSelected`. Same hole in `tools/report.py` `cmd_html` for machine
fields.

`hostname`, `cxxflags`, `dataset`, `impl`, `label` come from POSTed JSON
and are written as HTML. A runner that can POST (empty token, or a leaked
bearer) can store script in the UI.

Fix: `textContent` / `createElement` for untrusted strings. Escape machine
JSON in the HTML reporter.

### M3. PMU slot probe can over-count, then under-protect

Where: `include/perf/events.hpp` — `detect_pmu_slots`, `plan_passes`

Capacity is measured by opening N copies of `branch-instructions`. Identical
events can share or duplicate counters on some PMUs, so mixed-event passes
later multiplex. If `slots < 2`, core events (instructions + cycles) still
all go in every pass — the situation the multi-pass design exists to avoid.

Fix: probe with distinct general-purpose events from the catalog. If
`slots <= core.size()`, fail `selfcheck` or split core events too, and stop
claiming “no multiplexing”.

### M4. Collector is wide-open on the LAN by default

Where: `Makefile` `BIND` / `TOKEN`; `src/hwc/serve.cpp` `write_ok`,
`cmd_serve`

`make serve` binds `0.0.0.0:8080`. GET APIs have no auth. An empty `--token`
accepts POSTs. Documented for UAT, but the binary does not refuse to start
without a token, has no body size limit, compares the bearer with `==`
(timing + `ps` argv leak).

Fix: require `--token` unless `--allow-open-writes`. Default bind to
`127.0.0.1`. Cap JSON body size. Constant-time token compare; read token
from env or file.

### M5. CI diff does not honour `pass_drift`

Where: `tools/report.py` `cmd_diff`; `bench/json_perf.cpp` exit code

`json_perf` warns on >0.5% stitch drift and still exits 0. `report.py diff`
gates on `ins/byte` only. Known default-storage rows drift ~0.5–1.2%, right
on the 1% threshold.

The instruction *minimum* is still a valid gate. Stitched cache columns in
`maxy` / `html` are not, and nothing in the gate path says so. Default-storage
drift can silently eat the 1% budget.

Fix: fail or skip rows with `pass_drift` above a flag. Print a non-zero hint
when any row is flagged. Do not let default-storage drift consume the gate
threshold. Consider gating pool / null tighter than default, or pool only,
until the fresh-process experiment in HANDOFF §4.2 lands.

### M6. `list_runs` is O(runs × samples) in one thread

Where: `src/hwc/store.cpp` `list_runs`; `src/hwc/serve.cpp` accept loop

Every list loads every sample to compute median `ins/byte` and
drift-vs-first. The HTTP server is a blocking accept / read / write loop
with no SQLite `busy_timeout`. A few hundred UAT runs will hitch pushes and
the UI together.

Fix: store `median_ins_byte` on insert, or SQL aggregate. Set `busy_timeout`.
If threads are added later, mutex around `Store`.

### M7. GHz mixes user-only cycles with full task-clock

Where: `include/perf/counters.hpp` `exclude_kernel=1`; derived
`GHz = cycles / task-clock-ns`

Hardware events exclude kernel; `PERF_COUNT_SW_TASK_CLOCK` does not.
Syscalls, page faults, and kernel time in the measurement loop pull GHz
down. That is not “achieved clock” as documented.

Fix: either include kernel in HW events (needs `paranoid < 2`), or use
`time_running` from the group, or document GHz as user-cycles per task-ns.

### M8. No automated tests for the project invariants

Where: repo root — no `test/` or `*_test.cpp`

Blank-vs-zero, pass planning, CSV round-trip, derived formulas (C++ vs
Python), `sha_match` prefixing, path sanitisation, and Score picking are
all hand-tested. They will regress quietly.

Fix: unit-test `plan_passes`, `derived_of` vs `report.py derived()`,
`split_csv_line`, `serve_static`, and `compare()` default picks with a
fixture DB.

---

## Low — hygiene

### L1. Fingerprint collection shells out instead of reading files

Where: `include/perf/machine.hpp` `detail::run`; `events.hpp` `sysfs_aliases`

hostname, nproc, SMT, L3, governor, boost, flags md5, and sysfs aliases all
go through `popen`. `ls | cat | md5sum | tr` is locale-fragile, needs those
binaries, and is the wrong tool for `/sys`.

Fix: `std::filesystem` directory iterator and `ifstream`. Hash in-process.

### L2. `to_json` string escape is incomplete

Where: `include/perf/machine.hpp` `to_json`

Only quotes, backslash, and newline are escaped. A `cpu_model` or hostname
with a tab or control byte yields invalid JSON and a failed push.

Fix: use Boost.JSON (already linked into `hwc`) or escape the RFC 8259 set.

### L3. CSV rows are not quoted; ioctl / read errors are ignored

Where: `include/perf/session.hpp` `write_csv`; `include/perf/counters.hpp`

A dataset named `foo,bar.json` splits columns. `ioctl_group` ignores
failures so a failed ENABLE looks like zeros. `read()` does not check
`fds_.empty()` or `buf[0]==nr`.

Fix: RFC 4180 quoting. Check ioctl and read return values; treat failure as
missing, not zero.

### L4. UI depends on jsDelivr with no SRI and no offline fallback

Where: `web/index.html`

jQuery, Select2, and Tabulator load from a CDN. The collector UI is
otherwise a local Beast server; a CDN outage or tag move blanks Score.

Fix: vendor the three files next to `app.js`, or add integrity hashes and a
documented fallback.

### L5. `metrics()` and `suggest()` have sharp edges

Where: `src/hwc/store.cpp` `metrics`, `sha_match`

Event discovery only inspects 200 samples, so late-probed PMU names vanish.
`sha_match` is a prefix compare: `json_sha=e` matches every SHA. No
`UNIQUE(run_id, dataset, impl, op)`; duplicates last-win in `compare()`.

Fix: scan distinct keys properly, require a min prefix length, add a unique
sample index.

### L6. Small correctness nits in the drivers

Where: `bench/json_perf.cpp`; `bench/selfcheck.cpp`; `include/perf/session.hpp`

- `serializer_fixture::out` is reserved and never written (work still runs).
- `selfcheck` ignores `pin()` failure.
- `session.hpp` uses `std::map` / `std::tie` without including them.
- Stale comment still says each pass calibrates its own iteration count.

Fix: include what you use. Fail the migration test if affinity is not set.
Delete the dead string or actually serialize into it.

### L7. Push URL parser cannot do IPv6 or HTTPS

Where: `include/hwc/http_util.hpp` `parse_http_url`

`rfind(':')` splits on the last colon, so `[::1]:8080` breaks. HTTPS throws.
Fine for the documented UAT; easy to trip on an IPv6 literal.

Fix: parse brackets, or require host and port as separate flags.

---

## Suggested fix order

1. Match `cxxflags` (and compiler) in Score default pick; add compiler to
   the instruction comparability tier. Stops the gate from firing on
   identity mistakes.
2. Compute spread from min/max; publish ratios from one sample. Stops
   Stability and IPC from lying.
3. Skip or fail high `pass_drift` rows in `report.py diff`. Keeps the 1%
   budget for real code change.
4. Token required, bind localhost, body limit, path boundary, no
   `innerHTML`. Makes the collector safe to leave running on a LAN.
5. Slot probe with distinct events; tests for `plan_passes` + derived +
   Score pick. Locks the invariants before capy / Beast2.

Do not block UAT on the low list (popen fingerprints, CDN SRI, IPv6 URLs,
header hygiene). Those can wait.

---

## Open questions (experiments, not review defects)

These are still HANDOFF items. The code does not settle them.

- Default-storage `pass_drift`: allocator state vs same-process artifact.
  Fresh process per pass (HANDOFF §4.2) is the right next measurement.
- 1.0% as a CI threshold is a placeholder sitting next to known ~1.2%
  default-storage drift. Gate pool/null tighter than default, or gate pool
  only, until that experiment lands.
- Threaded libraries (capy, Beast2, corosio) cannot use this
  `counter_group` as-is. `inherit=1` cannot combine with
  `PERF_FORMAT_GROUP`. Per-worker groups plus software events is still the
  documented path; nothing in the tree implements it.
- Best-of-reps hides in-run throttle, which is correct for a gate and wrong
  for the thermal study in HANDOFF §6.2. That needs a time-series output,
  not a change to the published minimum.

---

## Architecture (for orientation)

| Layer | Files | Job |
|---|---|---|
| Counters | `include/perf/counters.hpp` | RAII group, task-bound, user-mode, grouped read |
| Catalog | `include/perf/events.hpp` | Abstract events, probe, slot detect, pass plan |
| Fingerprint | `include/perf/machine.hpp` | Comparability key + sidecar JSON |
| Session | `include/perf/session.hpp` | Multi-pass measure, stitch, CSV |
| Driver | `bench/json_perf.cpp` | Pool / default / null parse + serialize |
| Trust | `bench/selfcheck.cpp` | Paranoid, slots, determinism, migration |
| Collector | `src/hwc/*`, `include/hwc/*` | POST `/api/runs`, Stability, Score |
| UI | `web/app.js` | Runs pick pair → Score geomean `ins/byte` |

One measurement library, one JSON driver, one Beast binary (`serve` / `push`),
one SQLite file, one AJAX UI. Runners never talk SQLite; they POST JSON
`{meta, machine, rows}`.
