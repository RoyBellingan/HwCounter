// perf/machine.hpp - machine fingerprint emitted alongside every result set.
//
// Counter values are only comparable across machines for architecture-neutral
// events (instructions retired). Cycles, cache and TLB events depend on the
// microarchitecture, the cache hierarchy and even the kernel's event mapping.
// So every run records what it was measured on, and the report refuses to
// diff two runs whose fingerprints disagree on the parts that matter.

#pragma once

#include "events.hpp"

#include <sched.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
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
    // beyond instruction counts. Cycle, cache and TLB events also need a
    // matching instruction_key(): different code gives different cycles.
    std::string comparability_key() const {
        return vendor + "|" + cpu_family + "|" + cpu_model_id + "|" +
               stepping + "|" + kernel;
    }
    // Fields that MUST match for instruction counts to be comparable. The
    // compiler flags are not known to the binary; they come from meta.json
    // and the collector and report.py add them to this key.
    std::string instruction_key() const { return compiler; }
};

namespace detail {
inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r'))
        s.pop_back();
    return s;
}
inline std::string cpuinfo_line(char const* key) {
    std::ifstream f("/proc/cpuinfo");
    std::string line, want(key);
    while (std::getline(f, line)) {
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k == want) return line;
    }
    return {};
}
inline std::string cpuinfo(char const* key) {
    auto line = cpuinfo_line(key);
    auto c = line.find(':');
    if (c == std::string::npos || c + 2 > line.size()) return {};
    return line.substr(c + 2);
}
inline std::string slurp(std::string const& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return trim(s);
}
// First line of `file` below every /sys/devices/system/cpu/cpuN directory,
// de-duplicated and sorted, each followed by `sep`.
inline std::string per_cpu_unique(char const* file, char sep) {
    std::set<std::string> vals;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/sys/devices/system/cpu", ec), end;
         !ec && it != end; it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0 ||
            name.find_first_not_of("0123456789", 3) != std::string::npos)
            continue;
        auto path = it->path() / file;
        if (!std::filesystem::exists(path)) continue;
        vals.insert(slurp(path.string()));
    }
    std::string out;
    for (auto const& v : vals) { out += v; out += sep; }
    return out;
}

// RFC 1321 MD5, only to keep cpu_flags_md5 identical to `md5sum` output.
inline std::string md5_hex(std::string const& msg) {
    static constexpr std::uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static constexpr int R[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22, 5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23, 6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    std::string m = msg;
    std::uint64_t bits = std::uint64_t(msg.size()) * 8;
    m.push_back(char(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 0; i < 8; ++i) m.push_back(char((bits >> (8 * i)) & 0xff));
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    auto rotl = [](std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); };
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(std::uint8_t(m[off + 4 * i])) |
                   std::uint32_t(std::uint8_t(m[off + 4 * i + 1])) << 8 |
                   std::uint32_t(std::uint8_t(m[off + 4 * i + 2])) << 16 |
                   std::uint32_t(std::uint8_t(m[off + 4 * i + 3])) << 24;
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f; int g;
            if      (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            std::uint32_t t = d;
            d = c; c = b;
            b = b + rotl(a + f + K[i] + w[g], R[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::string out;
    char buf[3];
    for (auto v : h)
        for (int i = 0; i < 4; ++i) {
            std::snprintf(buf, sizeof buf, "%02x", (v >> (8 * i)) & 0xff);
            out += buf;
        }
    return out;
}

// JSON string escape for the RFC 8259 set.
inline std::string json_escape(std::string const& s) {
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                o += buf;
            } else {
                o += char(c);
            }
        }
    }
    return o;
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
    char host[256] = {};
    if (::gethostname(host, sizeof host - 1) == 0) mi.hostname = host;
    struct utsname u{};
    if (::uname(&u) == 0) mi.kernel = u.release;
    mi.cpu_model  = detail::cpuinfo("model name");
    mi.vendor     = detail::cpuinfo("vendor_id");
    mi.cpu_family = detail::cpuinfo("cpu family");
    mi.cpu_model_id = detail::cpuinfo("model");
    mi.stepping   = detail::cpuinfo("stepping");
    mi.microarch  = guess_microarch(mi.vendor, mi.cpu_family, mi.cpu_model_id);
    // Same value as `nproc`: CPUs this process may run on.
    cpu_set_t cs;
    CPU_ZERO(&cs);
    if (::sched_getaffinity(0, sizeof cs, &cs) == 0) mi.nproc = CPU_COUNT(&cs);
    else mi.nproc = int(::sysconf(_SC_NPROCESSORS_ONLN));
    mi.paranoid   = std::atoi(
        detail::slurp("/proc/sys/kernel/perf_event_paranoid").c_str());
    mi.smt = detail::slurp("/sys/devices/system/cpu/smt/active") == "1";
    mi.l3_topology = detail::per_cpu_unique("cache/index3/shared_cpu_list", ';');
    mi.governor = detail::per_cpu_unique("cpufreq/scaling_governor", '/');
    if (std::filesystem::exists("/sys/devices/system/cpu/cpufreq/boost"))
        mi.boost = detail::slurp("/sys/devices/system/cpu/cpufreq/boost");
    else
        mi.boost = detail::slurp("/sys/devices/system/cpu/intel_pstate/no_turbo");
    // md5 of the first "flags" line plus newline, as `grep | md5sum` gave.
    auto flags = detail::cpuinfo_line("flags");
    mi.flags_hash = detail::md5_hex(flags + "\n").substr(0, 12);
#if defined(__clang__)
    mi.compiler = "clang " __clang_version__;
#elif defined(__GNUC__)
    mi.compiler = "gcc " __VERSION__;
#else
    mi.compiler = "unknown";
#endif
    mi.aliases   = sysfs_aliases();
    mi.pmu_slots = detect_pmu_slots(probe_supported(catalog()));
    return mi;
}

inline std::string to_json(machine_info const& mi) {
    auto const& esc = detail::json_escape;
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
    o << "  \"instruction_key\": \"" << esc(mi.instruction_key()) << "\",\n";
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
