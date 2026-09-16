// selfcheck - verify that this machine can produce trustworthy counter data.
//
// Run this FIRST on any new machine (CI runner, bare-metal server, VM) before
// trusting benchmark numbers from it. It reports the machine fingerprint, which
// events exist, how many PMU slots are available, and then empirically checks
// the two properties the harness depends on: that instruction counts are
// deterministic, and that they survive CPU migration.

#include <perf/session.hpp>

#include <sched.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

int failures = 0, warnings = 0;

void pass(std::string const& s) { std::cout << "  [ ok ] " << s << "\n"; }
void warn(std::string const& s) { std::cout << "  [warn] " << s << "\n"; ++warnings; }
void fail(std::string const& s) { std::cout << "  [FAIL] " << s << "\n"; ++failures; }

// Deterministic user-mode workload: fixed instruction count by construction.
// noinline + a distinct seed per call, so the compiler cannot common up two
// invocations and silently halve the work being measured.
__attribute__((noinline))
std::uint64_t workload(std::vector<std::uint32_t> const& v, std::uint32_t seed) {
    std::uint64_t s = seed;
    for (auto x : v) s += x * 3u + 1u;
    asm volatile("" : "+r"(s) : : "memory");
    return s;
}

double spread(std::vector<double> const& d) {
    auto [lo, hi] = std::minmax_element(d.begin(), d.end());
    return *lo > 0 ? (*hi - *lo) / *lo : 0.0;
}

std::uint32_t seed_counter = 0;

double measure_instructions(std::vector<std::uint32_t> const& v) {
    perf::counter_group g;
    g.add(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
    g.reset();
    g.enable();
    volatile auto s = workload(v, ++seed_counter);
    (void)s;
    g.disable();
    return double(g.read().values[0]);
}

}  // namespace

int main() {
    std::vector<std::uint32_t> v(1u << 20);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = std::uint32_t(i);

    std::cout << "=== machine ===\n";
    auto mi = perf::probe_machine();
    std::cout << "  cpu         : " << mi.cpu_model << "\n"
              << "  microarch   : " << mi.microarch
              << "  (family " << mi.cpu_family << " model " << mi.cpu_model_id
              << " stepping " << mi.stepping << ")\n"
              << "  kernel      : " << mi.kernel << "\n"
              << "  compiler    : " << mi.compiler << "\n"
              << "  cpus        : " << mi.nproc
              << "   SMT " << (mi.smt ? "ON" : "off") << "\n"
              << "  L3 sharing  : " << mi.l3_topology << "\n"
              << "  comparability key: " << mi.comparability_key() << "\n";

    std::cout << "\n=== access ===\n";
    if (mi.paranoid <= 2) pass("perf_event_paranoid=" + std::to_string(mi.paranoid) +
                               " - user-mode self-profiling permitted");
    else fail("perf_event_paranoid=" + std::to_string(mi.paranoid) +
              " - too restrictive; need <= 2 (sysctl kernel.perf_event_paranoid=2,"
              " or --cap-add=PERFMON in a container)");

    std::cout << "\n=== events ===\n";
    std::vector<std::string> rejected;
    auto sup = perf::probe_supported(perf::catalog(), &rejected);
    std::cout << "  supported   : " << sup.size() << "\n";
    std::cout << "  unavailable : " << rejected.size();
    for (auto const& r : rejected) std::cout << " " << r;
    std::cout << "\n";
    bool have_ins = false, have_cyc = false;
    for (auto const& e : sup) {
        if (e.name == "instructions") have_ins = true;
        if (e.name == "cycles")       have_cyc = true;
    }
    if (have_ins) pass("'instructions' available - regression gating possible");
    else fail("'instructions' NOT available - this machine cannot gate regressions");
    if (!have_cyc) warn("'cycles' unavailable - diagnostics will be limited");

    std::cout << "\n=== PMU capacity ===\n";
    std::cout << "  slots       : " << mi.pmu_slots << "\n";
    auto passes = perf::plan_passes(sup, mi.pmu_slots);
    std::cout << "  passes      : " << passes.size()
              << " (workload repeated once per pass to avoid multiplexing)\n";
    if (mi.pmu_slots <= 2)
        warn("very few PMU slots - likely a VM with a restricted vPMU; "
             "expect many passes and longer runs");

    std::cout << "\n=== determinism ===\n";
    (void)measure_instructions(v);  // warm
    std::vector<double> samples;
    for (int i = 0; i < 12; ++i) samples.push_back(measure_instructions(v));
    double s = spread(samples);
    std::cout << "  instructions: " << std::fixed << std::setprecision(0)
              << samples[0] << "   spread over 12 runs: "
              << std::setprecision(4) << (s * 100) << "%\n";
    if (s < 0.001) pass("instruction counts are deterministic (<0.1%)");
    else if (s < 0.01) warn("instruction spread " + std::to_string(s * 100) +
                            "% - usable, but raise gate thresholds");
    else fail("instruction spread " + std::to_string(s * 100) +
              "% - too noisy to gate on; check for a shared/throttled host");

    std::cout << "\n=== migration safety ===\n";
    cpu_set_t orig;
    CPU_ZERO(&orig);
    sched_getaffinity(0, sizeof orig, &orig);
    std::vector<int> cpus;
    for (int c = 0; c < mi.nproc; ++c) if (CPU_ISSET(c, &orig)) cpus.push_back(c);

    if (cpus.size() < 2) {
        warn("only one CPU available - migration check skipped");
    } else {
        auto pin = [](int c) {
            cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s);
            return sched_setaffinity(0, sizeof s, &s) == 0;
        };
        pin(cpus.front());
        double a = measure_instructions(v);
        // Count across a forced migration to the most distant CPU available.
        perf::counter_group g;
        g.add(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
        g.reset(); g.enable();
        volatile auto x1 = workload(v, ++seed_counter); (void)x1;
        pin(cpus.back());
        volatile auto x2 = workload(v, ++seed_counter); (void)x2;
        g.disable();
        double both = double(g.read().values[0]);
        sched_setaffinity(0, sizeof orig, &orig);

        double expect = 2 * a, err = std::abs(both - expect) / expect;
        std::cout << "  cpu" << cpus.front() << " single : " << std::setprecision(0) << a << "\n"
                  << "  cpu" << cpus.front() << "->cpu" << cpus.back()
                  << " both: " << both << "   (expected ~" << expect << ", error "
                  << std::setprecision(3) << (err * 100) << "%)\n";
        if (err < 0.01) pass("counters survive CPU migration - pinning not required "
                             "for instruction gating");
        else fail("counts lost across migration - pin the benchmark process");
    }

    std::cout << "\n=== stability advice ===\n";
    if (mi.smt) warn("SMT is on - a sibling thread shares the core's PMU-visible "
                     "resources; pin to one CPU per physical core, or disable SMT");
    else pass("SMT off");
    std::string gov = perf::detail::run(
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null");
    if (!gov.empty()) {
        if (gov == "performance") pass("cpufreq governor = performance");
        else warn("cpufreq governor = " + gov +
                  " - cycle counts will vary with clock; instructions are unaffected");
    }
    if (mi.l3_topology.find(';') != std::string::npos &&
        mi.l3_topology.find_first_of(';') != mi.l3_topology.size() - 1)
        warn("multiple L3 domains - pin within one domain or cache counters "
             "will jump between runs");

    std::cout << "\n=== verdict ===\n";
    if (failures) {
        std::cout << "  UNUSABLE for regression gating (" << failures
                  << " failure(s), " << warnings << " warning(s))\n";
        return 1;
    }
    std::cout << "  OK for regression gating on instructions"
              << (warnings ? " (with " + std::to_string(warnings) +
                             " warning(s) affecting cycle/cache stability)" : "")
              << "\n";
    return 0;
}
