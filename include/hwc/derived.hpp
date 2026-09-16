#pragma once

#include <boost/json.hpp>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace hwc {

inline std::optional<double> as_finite(boost::json::value const* v) {
    if (!v || v->is_null()) return std::nullopt;
    if (v->is_double()) {
        double x = v->as_double();
        if (!std::isfinite(x)) return std::nullopt;
        return x;
    }
    if (v->is_int64()) return static_cast<double>(v->as_int64());
    if (v->is_uint64()) return static_cast<double>(v->as_uint64());
    return std::nullopt;
}

inline std::optional<double> get_num(boost::json::object const& o, char const* k) {
    auto it = o.find(k);
    if (it == o.end()) return std::nullopt;
    return as_finite(&it->value());
}

inline boost::json::value num_or_null(double v) {
    if (!std::isfinite(v)) return nullptr;
    return v;
}

inline boost::json::value opt_or_null(std::optional<double> v) {
    return v ? num_or_null(*v) : boost::json::value(nullptr);
}

// Same formulas as tools/report.py derived().
inline boost::json::object derived_of(boost::json::object const& row) {
    double bytes = get_num(row, "bytes").value_or(1.0);
    if (bytes <= 0) bytes = 1.0;
    double ns = get_num(row, "ns_per_call").value_or(1.0);
    if (ns == 0) ns = 1.0;
    auto ns_raw = get_num(row, "ns_per_call");
    auto ins = get_num(row, "instructions");
    auto cyc = get_num(row, "cycles");
    auto tc  = get_num(row, "task-clock-ns");
    if (!tc) tc = get_num(row, "task_clock_ns");

    boost::json::object d;
    d["ns/call"] = opt_or_null(ns_raw);
    d["task-clock-ns"] = opt_or_null(tc);
    d["MB/s"]     = (bytes / ns) * 1000.0;
    d["ins/byte"] = ins ? num_or_null(*ins / bytes) : boost::json::value(nullptr);
    d["cyc/byte"] = cyc ? num_or_null(*cyc / bytes) : boost::json::value(nullptr);
    if (ins && cyc && *cyc != 0)
        d["IPC"] = *ins / *cyc;
    else
        d["IPC"] = nullptr;
    if (cyc && tc && *tc != 0)
        d["GHz"] = *cyc / *tc;
    else
        d["GHz"] = nullptr;

    if (auto spr = get_num(row, "cyc_spread"))
        d["IPC stab%"] = 100.0 * *spr;

    auto br = get_num(row, "branches");
    auto bm = get_num(row, "branch-misses");
    if (bm) d["br-miss%"] = (br && *br) ? num_or_null(100.0 * *bm / *br) : boost::json::value(nullptr);

    auto l1 = get_num(row, "L1d-read");
    auto l1m = get_num(row, "L1d-read-miss");
    if (l1m) d["L1d-miss%"] = (l1 && *l1) ? num_or_null(100.0 * *l1m / *l1) : boost::json::value(nullptr);

    auto cr = get_num(row, "cache-references");
    auto cm = get_num(row, "cache-misses");
    if (cm) d["cache-miss%"] = (cr && *cr) ? num_or_null(100.0 * *cm / *cr) : boost::json::value(nullptr);

    auto fe = get_num(row, "stalled-cycles-fe");
    if (fe && cyc && *cyc)
        d["fe-stall%"] = 100.0 * *fe / *cyc;
    else if (fe)
        d["fe-stall%"] = nullptr;

    d["context-switches"] = opt_or_null(get_num(row, "context-switches"));
    d["page-faults"] = opt_or_null(get_num(row, "page-faults"));
    d["cpu-migrations"] = opt_or_null(get_num(row, "cpu-migrations"));

    return d;
}

inline std::optional<double> derived_metric(boost::json::object const& der, std::string const& name) {
    auto it = der.find(name);
    if (it == der.end()) return std::nullopt;
    return as_finite(&it->value());
}

inline double geomean_ratio(double a, double b) {
    if (!(a > 0) || !(b > 0)) return std::numeric_limits<double>::quiet_NaN();
    return a / b;
}

}  // namespace hwc
