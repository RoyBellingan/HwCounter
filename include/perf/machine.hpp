// perf/machine.hpp - machine fingerprint emitted alongside every result set.
//
// Counter values are only comparable across machines for architecture-neutral
// events (instructions retired). Cycles, cache and TLB events depend on the
// microarchitecture, the cache hierarchy and even the kernel's event mapping.
// So every run records what it was measured on, and the report refuses to
// diff two runs whose fingerprints disagree on the parts that matter.

#pragma once

#include "events.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace perf {

struct machine_info {
    std::string hostname, kernel, cpu_model, vendor, microarch;
    std::string cpu_family, cpu_model_id, stepping, flags_hash;
    std::string compiler, l3_topology, governor, boost;
    int  nproc = 0, paranoid = -1, pmu_slots = 0;
    bool smt = false;
    std::map<std::string, std::string> aliases;
    std::vector<std::string> unsupported;

    // Fields that MUST match for two runs to be numerically comparable
    // beyond instruction counts.
    std::string comparability_key() const {
        return vendor + "|" + cpu_family + "|" + cpu_model_id + "|" +
               stepping + "|" + kernel;
    }
};

namespace detail {
inline std::string run(char const* cmd) {
    std::string out;
    if (FILE* p = ::popen(cmd, "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof buf, p)) out += buf;
        ::pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}
inline std::string cpuinfo(char const* key) {
    std::ifstream f("/proc/cpuinfo");
    std::string line, want(key);
    while (std::getline(f, line)) {
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k == want) return line.substr(c + 2);
    }
    return {};
}
inline std::string slurp(char const* path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}
}  // namespace detail

// Best-effort microarchitecture label. Only for human readability in reports -
// nothing numeric depends on it, so an unknown part degrades to "unknown"
// rather than to a wrong assumption.
inline std::string guess_microarch(std::string const& vendor,
                                   std::string const& fam,
                                   std::string const& model) {
    int f = std::atoi(fam.c_str()), m = std::atoi(model.c_str());
    if (vendor == "AuthenticAMD") {
        if (f == 0x17) return (m >= 0x60) ? "Zen2" : "Zen/Zen+";
        if (f == 0x19) {
            // Zen3: Vermeer 0x21, Cezanne 0x50; Zen4: Raphael 0x61 (7900X3D),
            // Genoa 0x11, Phoenix 0x74. Anything unlisted stays generic.
            if (m <= 0x0f || (m >= 0x20 && m <= 0x5f)) return "Zen3";
            if ((m >= 0x10 && m <= 0x1f) || m >= 0x60)  return "Zen4";
            return "Zen3/Zen4";
        }
        if (f == 0x1a) return "Zen5";
    } else if (vendor == "GenuineIntel") {
        if (f == 6 && m >= 0xb7) return "Raptor Cove";
        if (f == 6 && m >= 0x97) return "Golden Cove";
        if (f == 6)              return "Intel Core (family 6)";
    }
    return "unknown";
}

inline machine_info probe_machine() {
    machine_info mi;
    mi.hostname   = detail::run("hostname 2>/dev/null");
    mi.kernel     = detail::run("uname -r");
    mi.cpu_model  = detail::cpuinfo("model name");
    mi.vendor     = detail::cpuinfo("vendor_id");
    mi.cpu_family = detail::cpuinfo("cpu family");
    mi.cpu_model_id = detail::cpuinfo("model");
    mi.stepping   = detail::cpuinfo("stepping");
    mi.microarch  = guess_microarch(mi.vendor, mi.cpu_family, mi.cpu_model_id);
    mi.nproc      = std::atoi(detail::run("nproc").c_str());
    mi.paranoid   = std::atoi(
        detail::slurp("/proc/sys/kernel/perf_event_paranoid").c_str());
    mi.smt = detail::run("cat /sys/devices/system/cpu/smt/active 2>/dev/null") == "1";
    mi.l3_topology = detail::run(
        "cat /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list "
        "2>/dev/null | sort -u | tr '\\n' ';'");
    mi.governor = detail::run(
        "cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null "
        "| sort -u | tr '\\n' '/'");
    mi.boost = detail::run(
        "cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || "
        "cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null");
    mi.flags_hash = detail::run(
        "grep -m1 '^flags' /proc/cpuinfo | md5sum | cut -c1-12");
#if defined(__clang__)
    mi.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    mi.compiler = "gcc " __VERSION__;
#else
    mi.compiler = "unknown";
#endif
    mi.aliases   = sysfs_aliases();
    mi.pmu_slots = detect_pmu_slots();
    return mi;
}

inline std::string to_json(machine_info const& mi) {
    auto esc = [](std::string const& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    };
    std::ostringstream o;
    o << "{\n";
    o << "  \"hostname\": \""   << esc(mi.hostname)  << "\",\n";
    o << "  \"kernel\": \""     << esc(mi.kernel)    << "\",\n";
    o << "  \"cpu_model\": \""  << esc(mi.cpu_model) << "\",\n";
    o << "  \"vendor\": \""     << esc(mi.vendor)    << "\",\n";
    o << "  \"microarch\": \""  << esc(mi.microarch) << "\",\n";
    o << "  \"cpu_family\": \"" << esc(mi.cpu_family)<< "\",\n";
    o << "  \"cpu_model_id\": \""<< esc(mi.cpu_model_id) << "\",\n";
    o << "  \"stepping\": \""   << esc(mi.stepping)  << "\",\n";
    o << "  \"cpu_flags_md5\": \"" << esc(mi.flags_hash) << "\",\n";
    o << "  \"nproc\": "        << mi.nproc          << ",\n";
    o << "  \"smt_active\": "   << (mi.smt ? "true" : "false") << ",\n";
    o << "  \"l3_topology\": \""<< esc(mi.l3_topology) << "\",\n";
    o << "  \"cpufreq_governor\": \"" << esc(mi.governor) << "\",\n";
    o << "  \"boost\": \"" << esc(mi.boost) << "\",\n";
    o << "  \"perf_event_paranoid\": " << mi.paranoid << ",\n";
    o << "  \"pmu_slots_detected\": "  << mi.pmu_slots << ",\n";
    o << "  \"compiler\": \""   << esc(mi.compiler)  << "\",\n";
    o << "  \"comparability_key\": \"" << esc(mi.comparability_key()) << "\",\n";
    o << "  \"pmu_aliases\": {";
    bool first = true;
    for (auto const& [k, v] : mi.aliases) {
        o << (first ? "\n" : ",\n") << "    \"" << esc(k) << "\": \""
          << esc(v) << "\"";
        first = false;
    }
    o << (first ? "" : "\n  ") << "},\n";
    o << "  \"unsupported_events\": [";
    for (std::size_t i = 0; i < mi.unsupported.size(); ++i)
        o << (i ? ", " : "") << "\"" << esc(mi.unsupported[i]) << "\"";
    o << "]\n}\n";
    return o.str();
}

}  // namespace perf
