// perf/session.hpp - multi-pass measurement session.
//
// More events are wanted than the PMU has slots, and time-multiplexing
// under-reports. So instead of multiplexing, the same workload is run once per
// event-set ("pass") and the results stitched together by key. "instructions"
// and "cycles" are re-measured in every pass, which gives a free integrity
// check: if a row's instruction count drifts between passes, the stitch is not
// trustworthy and the row is flagged rather than quietly averaged.

#pragma once

#include "machine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

namespace perf {

struct row_key {
    std::string dataset, impl, op;
    bool operator<(row_key const& o) const {
        return std::tie(dataset, impl, op) < std::tie(o.dataset, o.impl, o.op);
    }
};

// Lowest and highest value seen. The spread is computed after all samples,
// so it does not depend on the order of the samples.
struct min_max {
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    void add(double v) { lo = std::min(lo, v); hi = std::max(hi, v); }
    bool empty() const { return lo > hi; }
    double spread() const {
        if (empty()) return 0.0;
        return (hi - lo) / std::max(lo, 1.0);
    }
};

struct row {
    std::size_t bytes = 0;     // payload size, for per-byte normalisation
    std::size_t calls = 0;     // iterations measured
    double      ns    = 0;     // best wall time observed
    std::map<std::string, double> ev;           // event name -> min per call
    std::map<std::size_t, min_max> ins_by_pass; // pass -> instr/call range
    std::map<std::size_t, min_max> cyc_by_pass; // pass -> cycles/call range
    bool multiplexed = false;

    // Counters of the ONE sample with the best wall time. Ratios (IPC, GHz)
    // must come from values that occurred together; the per-event minima in
    // `ev` can come from different reps.
    double best_ins = std::nan(""), best_cyc = std::nan(""), best_tc = std::nan("");

    double get(std::string const& n) const {
        auto it = ev.find(n);
        return it == ev.end() ? std::nan("") : it->second;
    }
    // Stitch integrity: do the per-pass BEST instruction counts agree? If
    // they do not, events measured in different passes describe different
    // work and must not be combined. Rep-to-rep noise is excluded by taking
    // each pass's minimum first - that is reported separately as ins_spread.
    double pass_drift() const {
        if (ins_by_pass.size() < 2) return 0.0;
        double lo = 1e300, hi = 0;
        for (auto const& [p, v] : ins_by_pass) { lo = std::min(lo, v.lo); hi = std::max(hi, v.lo); }
        return lo > 0 ? (hi - lo) / lo : 0.0;
    }
    // Rep-to-rep noise: worst (max - min) / min within any one pass.
    static double worst_spread(std::map<std::size_t, min_max> const& by) {
        double s = 0.0;
        for (auto const& [p, v] : by) s = std::max(s, v.spread());
        return s;
    }
    double ins_spread() const { return worst_spread(ins_by_pass); }
    double cyc_spread() const { return worst_spread(cyc_by_pass); }
    double best_ipc() const {
        return (best_cyc > 0) ? best_ins / best_cyc : std::nan("");
    }
    double best_ghz() const {
        return (best_tc > 0) ? best_cyc / best_tc : std::nan("");
    }
};

// RFC 4180: quote a field that holds a comma, quote or line break.
inline std::string csv_field(std::string const& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += '"';
        o += c;
    }
    o += '"';
    return o;
}

class session {
public:
    explicit session(machine_info mi) : mi_(std::move(mi)) {
        auto all = catalog();
        events_ = probe_supported(all, &mi_.unsupported);
        passes_ = plan_passes(events_, mi_.pmu_slots);
    }

    machine_info const& machine() const { return mi_; }
    std::vector<std::vector<event_def>> const& passes() const { return passes_; }
    std::size_t event_count() const { return events_.size(); }

    // Run `body` under the events of pass `p`, recording into `key`.
    // `body` returns the number of iterations it performed.
    template <class F>
    void measure(row_key const& key, std::size_t bytes, std::size_t p, F&& body) {
        counter_group g;
        for (auto const& e : passes_[p]) g.add(e.type, e.config);

        g.reset();
        g.enable();
        auto t0 = std::chrono::steady_clock::now();
        std::size_t calls = body();
        auto t1 = std::chrono::steady_clock::now();
        g.disable();

        auto r = g.read();
        auto& dst = rows_[key];
        dst.bytes = bytes;
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();

        // Keep the best (lowest-time) observation; noise is strictly additive.
        bool better = calls > 0 &&
            (dst.calls == 0 || (ns / calls) < (dst.ns / dst.calls));
        if (better) { dst.ns = ns; dst.calls = calls; }
        if (r.multiplexed()) dst.multiplexed = true;

        // Normalise to per-call immediately: pass totals are only comparable
        // per call, even though iteration counts are calibrated once.
        double const inv = calls ? 1.0 / double(calls) : 0.0;
        double ins = std::nan(""), cyc = std::nan(""), tc = std::nan("");
        for (std::size_t i = 0; i < passes_[p].size(); ++i) {
            auto const& name = passes_[p][i].name;
            double v = r.scaled(i) * inv;
            if (name == "instructions")       { dst.ins_by_pass[p].add(v); ins = v; }
            else if (name == "cycles")        { dst.cyc_by_pass[p].add(v); cyc = v; }
            else if (name == "task-clock-ns") tc = v;
            // Core events appear in every pass: keep the minimum, which is the
            // least contaminated sample.
            auto it = dst.ev.find(name);
            if (it == dst.ev.end() || v < it->second) dst.ev[name] = v;
        }
        if (better) { dst.best_ins = ins; dst.best_cyc = cyc; dst.best_tc = tc; }
    }

    std::map<row_key, row> const& rows() const { return rows_; }

    // Column order: core, then hardware, then software - stable across
    // machines so two CSVs can be diffed even if one lacks some events.
    std::vector<std::string> columns() const {
        std::vector<std::string> c;
        for (auto const& e : catalog()) {
            for (auto const& s : events_)
                if (s.name == e.name) { c.push_back(e.name); break; }
        }
        return c;
    }

    void write_csv(std::ostream& os) const {
        auto cols = columns();
        os << "dataset,impl,op,bytes,calls,ns_per_call,pass_drift,ins_spread,cyc_spread,multiplexed,best_ipc,best_ghz";
        for (auto const& c : cols) os << "," << c;
        os << "\n";
        os << std::fixed;
        for (auto const& [k, r] : rows_) {
            auto opt = [&os](double v, int prec) {
                os << ",";
                if (!std::isnan(v)) os << std::setprecision(prec) << v;
            };
            os << csv_field(k.dataset) << "," << csv_field(k.impl) << ","
               << csv_field(k.op) << ","
               << r.bytes << "," << r.calls << ","
               << std::setprecision(1) << (r.calls ? r.ns / r.calls : 0.0) << ","
               << std::setprecision(6) << r.pass_drift() << ","
               << std::setprecision(6) << r.ins_spread() << ","
               << std::setprecision(6) << r.cyc_spread() << ","
               << (r.multiplexed ? 1 : 0);
            opt(r.best_ipc(), 4);
            opt(r.best_ghz(), 6);
            for (auto const& c : cols) {
                double v = r.get(c);
                os << ",";
                if (std::isnan(v)) os << "";            // absent != zero
                else os << std::setprecision(v < 100 ? 3 : 0) << v;
            }
            os << "\n";
        }
    }

private:
    machine_info mi_;
    std::vector<event_def> events_;
    std::vector<std::vector<event_def>> passes_;
    std::map<row_key, row> rows_;
};

}  // namespace perf
