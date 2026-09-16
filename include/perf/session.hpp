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
#include <functional>
#include <iomanip>
#include <numeric>
#include <ostream>

namespace perf {

struct row_key {
    std::string dataset, impl, op;
    bool operator<(row_key const& o) const {
        return std::tie(dataset, impl, op) < std::tie(o.dataset, o.impl, o.op);
    }
};

struct row {
    std::size_t bytes = 0;     // payload size, for per-byte normalisation
    std::size_t calls = 0;     // iterations measured
    double      ns    = 0;     // best wall time observed
    std::map<std::string, double> ev;           // event name -> count
    std::map<std::size_t, double> ins_by_pass;  // pass -> best instr/call
    double ins_spread = 0;                      // rep-to-rep noise, all passes
    std::map<std::size_t, double> cyc_by_pass;  // pass -> best cycles/call
    double cyc_spread = 0;                      // rep-to-rep cycle noise
    bool multiplexed = false;

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
        for (auto const& [p, v] : ins_by_pass) { lo = std::min(lo, v); hi = std::max(hi, v); }
        return lo > 0 ? (hi - lo) / lo : 0.0;
    }
};

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
        bool better = dst.calls == 0 || (ns / calls) < (dst.ns / dst.calls);
        if (better) { dst.ns = ns; dst.calls = calls; }
        if (r.multiplexed()) dst.multiplexed = true;

        // Normalise to per-call immediately: each pass calibrates its own
        // iteration count, so raw totals are not comparable across passes.
        double const inv = calls ? 1.0 / double(calls) : 0.0;
        for (std::size_t i = 0; i < passes_[p].size(); ++i) {
            auto const& name = passes_[p][i].name;
            double v = r.scaled(i) * inv;
            auto track = [&](std::map<std::size_t, double>& by, double& spr) {
                auto ip = by.find(p);
                if (ip == by.end()) by[p] = v;
                else {
                    spr = std::max(spr,
                        std::abs(v - ip->second) / std::max(ip->second, 1.0));
                    ip->second = std::min(ip->second, v);
                }
            };
            if (name == "instructions") track(dst.ins_by_pass, dst.ins_spread);
            else if (name == "cycles")  track(dst.cyc_by_pass, dst.cyc_spread);
            // Core events appear in every pass: keep the minimum, which is the
            // least contaminated sample.
            auto it = dst.ev.find(name);
            if (it == dst.ev.end() || v < it->second) dst.ev[name] = v;
        }
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
        os << "dataset,impl,op,bytes,calls,ns_per_call,pass_drift,ins_spread,cyc_spread,multiplexed";
        for (auto const& c : cols) os << "," << c;
        os << "\n";
        os << std::fixed;
        for (auto const& [k, r] : rows_) {
            os << k.dataset << "," << k.impl << "," << k.op << ","
               << r.bytes << "," << r.calls << ","
               << std::setprecision(1) << (r.calls ? r.ns / r.calls : 0.0) << ","
               << std::setprecision(6) << r.pass_drift() << ","
               << std::setprecision(6) << r.ins_spread << ","
               << std::setprecision(6) << r.cyc_spread << ","
               << (r.multiplexed ? 1 : 0);
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
