// perf/events.hpp - portable event catalog: runtime discovery, probing,
// PMU-slot detection and multi-pass planning.
//
// Design rule: NO hardcoded raw event encodings. Every event here is either a
// kernel-abstracted event (PERF_TYPE_HARDWARE / HW_CACHE / SOFTWARE), which the
// kernel maps to whatever the local PMU calls it, or an alias read at runtime
// from sysfs. Anything the local CPU cannot provide is detected by probing and
// reported as unavailable - never silently as zero.

#pragma once

#include "counters.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace perf {

struct event_def {
    std::string   name;
    std::uint32_t type   = 0;
    std::uint64_t config = 0;
    bool          pmu    = true;   // true = consumes a physical PMU slot
    bool          core   = false;  // true = measured in every pass (reference)
};

inline std::uint64_t cache_config(std::uint64_t id, std::uint64_t op,
                                  std::uint64_t result) {
    return id | (op << 8) | (result << 16);
}

// The portable catalog. Everything is an abstract event, so this same list is
// valid on Intel, AMD and ARM - the kernel resolves it, and whatever the local
// part cannot express is dropped by probe_supported().
inline std::vector<event_def> catalog() {
    auto HW = [](char const* n, std::uint64_t c, bool core = false) {
        return event_def{n, PERF_TYPE_HARDWARE, c, true, core};
    };
    auto SW = [](char const* n, std::uint64_t c) {
        return event_def{n, PERF_TYPE_SOFTWARE, c, false, false};
    };
    auto C = [](char const* n, std::uint64_t id, std::uint64_t op,
                std::uint64_t r) {
        return event_def{n, PERF_TYPE_HW_CACHE, cache_config(id, op, r),
                         true, false};
    };

    return {
        // --- core reference events: measured in EVERY pass ---
        HW("instructions", PERF_COUNT_HW_INSTRUCTIONS, true),
        HW("cycles",       PERF_COUNT_HW_CPU_CYCLES,   true),

        // --- additional hardware events, rotated across passes ---
        HW("branches",              PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
        HW("branch-misses",         PERF_COUNT_HW_BRANCH_MISSES),
        HW("cache-references",      PERF_COUNT_HW_CACHE_REFERENCES),
        HW("cache-misses",          PERF_COUNT_HW_CACHE_MISSES),
        HW("bus-cycles",            PERF_COUNT_HW_BUS_CYCLES),
        HW("ref-cycles",            PERF_COUNT_HW_REF_CPU_CYCLES),
        HW("stalled-cycles-fe",     PERF_COUNT_HW_STALLED_CYCLES_FRONTEND),
        HW("stalled-cycles-be",     PERF_COUNT_HW_STALLED_CYCLES_BACKEND),

        C("L1d-read",       PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("L1d-read-miss",  PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("L1d-write",      PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_WRITE,    PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("L1d-prefetch",   PERF_COUNT_HW_CACHE_L1D,  PERF_COUNT_HW_CACHE_OP_PREFETCH, PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("L1i-read-miss",  PERF_COUNT_HW_CACHE_L1I,  PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("LLC-read",       PERF_COUNT_HW_CACHE_LL,   PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("LLC-read-miss",  PERF_COUNT_HW_CACHE_LL,   PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("LLC-write",      PERF_COUNT_HW_CACHE_LL,   PERF_COUNT_HW_CACHE_OP_WRITE,    PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("LLC-write-miss", PERF_COUNT_HW_CACHE_LL,   PERF_COUNT_HW_CACHE_OP_WRITE,    PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("dTLB-read",      PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("dTLB-read-miss", PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("dTLB-write-miss",PERF_COUNT_HW_CACHE_DTLB, PERF_COUNT_HW_CACHE_OP_WRITE,    PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("iTLB-read-miss", PERF_COUNT_HW_CACHE_ITLB, PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),
        C("BPU-read",       PERF_COUNT_HW_CACHE_BPU,  PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_ACCESS),
        C("BPU-read-miss",  PERF_COUNT_HW_CACHE_BPU,  PERF_COUNT_HW_CACHE_OP_READ,     PERF_COUNT_HW_CACHE_RESULT_MISS),

        // --- software events: free, no PMU slot, always available ---
        SW("task-clock-ns",    PERF_COUNT_SW_TASK_CLOCK),
        SW("page-faults",      PERF_COUNT_SW_PAGE_FAULTS),
        SW("page-faults-maj",  PERF_COUNT_SW_PAGE_FAULTS_MAJ),
        SW("context-switches", PERF_COUNT_SW_CONTEXT_SWITCHES),
        SW("cpu-migrations",   PERF_COUNT_SW_CPU_MIGRATIONS),
    };
}

// Can this event actually be opened on this machine?
inline bool probe(event_def const& e) {
    try {
        counter_group g;
        g.add(e.type, e.config);
        return true;
    } catch (...) {
        return false;
    }
}

inline std::vector<event_def>
probe_supported(std::vector<event_def> const& in,
                std::vector<std::string>* rejected = nullptr) {
    std::vector<event_def> out;
    for (auto const& e : in) {
        if (probe(e)) out.push_back(e);
        else if (rejected) rejected->push_back(e.name);
    }
    return out;
}

// How many PMU events can run concurrently before the kernel starts
// time-multiplexing? Determined empirically: open N duplicate general-purpose
// events, do a little work, and ask whether enabled == running.
inline int detect_pmu_slots(int cap = 12) {
    int best = 1;
    for (int n = 1; n <= cap; ++n) {
        try {
            counter_group g;
            for (int i = 0; i < n; ++i)
                g.add(PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS);
            g.reset();
            g.enable();
            std::uint64_t volatile s = 0;
            for (int i = 0; i < 200000; ++i) s += std::uint64_t(i);
            g.disable();
            if (g.read().multiplexed()) break;
            best = n;
        } catch (...) {
            break;
        }
    }
    return best;
}

// Split the events into passes that each fit the PMU without multiplexing.
// Core events (instructions, cycles) are repeated in every pass: they are the
// cross-pass consistency check, since the same workload must retire the same
// instruction count in each pass. Software events are free and ride along.
inline std::vector<std::vector<event_def>>
plan_passes(std::vector<event_def> const& supported, int slots) {
    std::vector<event_def> core, rot, soft;
    for (auto const& e : supported) {
        if (!e.pmu)      soft.push_back(e);
        else if (e.core) core.push_back(e);
        else             rot.push_back(e);
    }

    int const per_pass = std::max(1, slots - int(core.size()));
    std::vector<std::vector<event_def>> passes;
    for (std::size_t i = 0; i < rot.size(); i += per_pass) {
        std::vector<event_def> p = core;
        for (std::size_t j = i; j < rot.size() && j < i + per_pass; ++j)
            p.push_back(rot[j]);
        passes.push_back(std::move(p));
    }
    if (passes.empty()) passes.push_back(core);
    for (auto& p : passes)
        p.insert(p.end(), soft.begin(), soft.end());
    return passes;
}

// Kernel's own PMU event aliases, e.g. cache-misses -> event=0x64,umask=0x09.
// Recorded in the fingerprint so a reader knows what an abstract name meant on
// the machine that produced the numbers - this differs between vendors and
// even between microarchitectures from one vendor.
inline std::map<std::string, std::string> sysfs_aliases() {
    std::map<std::string, std::string> m;
    char const* dir = "/sys/bus/event_source/devices/cpu/events";
    std::string cmd = std::string("ls ") + dir + " 2>/dev/null";
    if (FILE* p = ::popen(cmd.c_str(), "r")) {
        char name[256];
        while (std::fgets(name, sizeof name, p)) {
            std::string n(name);
            if (!n.empty() && n.back() == '\n') n.pop_back();
            if (n.empty()) continue;
            std::ifstream f(std::string(dir) + "/" + n);
            std::string v;
            if (f && std::getline(f, v)) m[n] = v;
        }
        ::pclose(p);
    }
    return m;
}

}  // namespace perf
