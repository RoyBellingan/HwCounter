// Minimal demo: count cycles / instructions / cache misses / branch misses
// around two workloads with deliberately different memory behaviour.

#include <perf/counters.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <vector>

namespace {

struct event_spec {
    char const* name;
    perf_hw_id  id;
};

constexpr event_spec kEvents[] = {
    {"cycles",         PERF_COUNT_HW_CPU_CYCLES},
    {"instructions",   PERF_COUNT_HW_INSTRUCTIONS},
    {"branches",       PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
    {"branch-misses",  PERF_COUNT_HW_BRANCH_MISSES},
    {"cache-misses",   PERF_COUNT_HW_CACHE_MISSES},
};

// Sum with a sequential access pattern: prefetcher-friendly.
std::uint64_t sequential_sum(std::vector<std::uint32_t> const& v) {
    std::uint64_t s = 0;
    for (auto x : v) s += x;
    return s;
}

// Same amount of work, chased through a random permutation: cache-hostile.
std::uint64_t pointer_chase(std::vector<std::uint32_t> const& next) {
    std::uint64_t s = 0;
    std::uint32_t i = 0;
    for (std::size_t n = 0; n < next.size(); ++n) { i = next[i]; s += i; }
    return s;
}

void measure(char const* label, auto&& fn) {
    perf::counter_group g;
    for (auto const& e : kEvents)
        g.add(PERF_TYPE_HARDWARE, static_cast<std::uint64_t>(e.id));

    g.reset();
    g.enable();
    auto t0 = std::chrono::steady_clock::now();
    auto sink = fn();
    auto t1 = std::chrono::steady_clock::now();
    g.disable();

    auto r = g.read();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::printf("\n== %s ==  (%.2f ms, checksum %llu%s)\n", label, ms,
                (unsigned long long)sink,
                r.multiplexed() ? ", MULTIPLEXED - counts scaled" : "");
    for (std::size_t i = 0; i < r.values.size(); ++i)
        std::printf("  %-16s %14.0f\n", kEvents[i].name, r.scaled(i));

    double cyc = r.scaled(0), ins = r.scaled(1);
    if (cyc > 0) std::printf("  %-16s %14.3f\n", "IPC", ins / cyc);
}

}  // namespace

int main() {
    constexpr std::size_t N = 8u << 20;  // 32 MiB of uint32 -> well past L3

    std::vector<std::uint32_t> data(N);
    std::iota(data.begin(), data.end(), 0u);

    std::vector<std::uint32_t> perm = data;
    std::shuffle(perm.begin(), perm.end(), std::mt19937{12345});

    // Warm up so we measure steady state, not first-touch page faults.
    volatile std::uint64_t warm = sequential_sum(data) + pointer_chase(perm);
    (void)warm;

    measure("sequential sum",  [&] { return sequential_sum(data); });
    measure("random pointer chase", [&] { return pointer_chase(perm); });
    return 0;
}
