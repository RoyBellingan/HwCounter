// json_perf - hardware-counter benchmark driver for Boost.JSON.
//
//   json_perf [options] <file.json|dir>...
//     --out <prefix>     output prefix (default: results)
//     --min-ms <n>       minimum time per measurement (default 200)
//     --reps <n>         repetitions per (pass, key), best is kept (default 3)
//     --pin <cpu>        pin to a CPU (default: none)
//     --ops p,s          parse / serialize (default both)
//
// Emits <prefix>.csv (one row per dataset x impl x op, one column per event)
// and <prefix>.machine.json (the fingerprint).

#include <perf/session.hpp>

#include <boost/json/src.hpp>
#include <boost/json/basic_parser_impl.hpp>

#include <sched.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <functional>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace {

std::string read_file(fs::path const& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct dataset {
    std::string name, text;
};

// --- implementations under test -------------------------------------------
// Each returns a checksum so the optimiser cannot delete the work.

std::size_t parse_pool(std::string const& s) {
    unsigned char buf[4096];
    json::monotonic_resource mr(buf, sizeof buf);
    auto v = json::parse(s, &mr);
    return v.is_null() ? 0 : 1;
}

std::size_t parse_default(std::string const& s) {
    auto v = json::parse(s);
    return v.is_null() ? 0 : 1;
}

// Pure scanner cost: drives basic_parser with a handler that does nothing, so
// it validates the grammar without building a DOM or allocating. Mirrors the
// null_parser in the upstream Boost.JSON bench.
struct null_handler {
    static constexpr std::size_t max_object_size = std::size_t(-1);
    static constexpr std::size_t max_array_size  = std::size_t(-1);
    static constexpr std::size_t max_key_size    = std::size_t(-1);
    static constexpr std::size_t max_string_size = std::size_t(-1);
    bool on_document_begin(boost::system::error_code&) { return true; }
    bool on_document_end(boost::system::error_code&) { return true; }
    bool on_object_begin(boost::system::error_code&) { return true; }
    bool on_object_end(std::size_t, boost::system::error_code&) { return true; }
    bool on_array_begin(boost::system::error_code&) { return true; }
    bool on_array_end(std::size_t, boost::system::error_code&) { return true; }
    bool on_key_part(json::string_view, std::size_t, boost::system::error_code&) { return true; }
    bool on_key(json::string_view, std::size_t, boost::system::error_code&) { return true; }
    bool on_string_part(json::string_view, std::size_t, boost::system::error_code&) { return true; }
    bool on_string(json::string_view, std::size_t, boost::system::error_code&) { return true; }
    bool on_number_part(json::string_view, boost::system::error_code&) { return true; }
    bool on_int64(std::int64_t, json::string_view, boost::system::error_code&) { return true; }
    bool on_uint64(std::uint64_t, json::string_view, boost::system::error_code&) { return true; }
    bool on_double(double, json::string_view, boost::system::error_code&) { return true; }
    bool on_bool(bool, boost::system::error_code&) { return true; }
    bool on_null(boost::system::error_code&) { return true; }
    bool on_comment_part(json::string_view, boost::system::error_code&) { return true; }
    bool on_comment(json::string_view, boost::system::error_code&) { return true; }
};

std::size_t parse_null_parser(std::string const& s) {
    json::basic_parser<null_handler> p{json::parse_options{}};
    boost::system::error_code ec;
    p.reset();
    auto n = p.write_some(false, s.data(), s.size(), ec);
    return ec ? 0 : n;
}

struct serializer_fixture {
    json::value v;
    explicit serializer_fixture(std::string const& s) : v(json::parse(s)) {}
    std::size_t run() {
        json::serializer sr;
        sr.reset(&v);
        char buf[4096];
        std::size_t n = 0;
        while (!sr.done()) {
            auto sv = sr.read(buf, sizeof buf);
            n += sv.size();
        }
        return n;
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string prefix = "results";
    double min_ms = 200;
    int reps = 3, pin_cpu = -1;
    std::string ops = "ps";
    std::vector<fs::path> inputs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if      (a == "--out")    prefix = next();
        else if (a == "--min-ms") min_ms = std::atof(next().c_str());
        else if (a == "--reps")   reps   = std::atoi(next().c_str());
        else if (a == "--pin")    pin_cpu= std::atoi(next().c_str());
        else if (a == "--ops")    ops    = next();
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        } else inputs.push_back(a);
    }
    if (inputs.empty()) {
        std::cerr << "usage: json_perf [options] <file.json|dir>...\n";
        return 2;
    }
    if (!(min_ms > 0) || reps < 1) {
        std::cerr << "--min-ms must be > 0 and --reps must be >= 1\n";
        return 2;
    }
    if (ops.find_first_not_of("ps") != std::string::npos ||
        ops.find_first_of("ps") == std::string::npos) {
        std::cerr << "--ops takes p, s or ps\n";
        return 2;
    }

    if (pin_cpu >= 0) {
        cpu_set_t s;
        CPU_ZERO(&s);
        CPU_SET(pin_cpu, &s);
        if (sched_setaffinity(0, sizeof s, &s) != 0)
            std::cerr << "warning: could not pin to cpu " << pin_cpu << "\n";
    }

    // Collect datasets.
    std::vector<dataset> data;
    for (auto const& in : inputs) {
        if (fs::is_directory(in)) {
            std::vector<fs::path> files;
            for (auto const& e : fs::directory_iterator(in))
                if (e.path().extension() == ".json") files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (auto const& f : files)
                data.push_back({f.filename().string(), read_file(f)});
        } else {
            data.push_back({in.filename().string(), read_file(in)});
        }
    }
    if (data.empty()) { std::cerr << "no .json inputs found\n"; return 2; }

    perf::session sess(perf::probe_machine());
    auto const& mi = sess.machine();

    std::cerr << "cpu        : " << mi.cpu_model << "  (" << mi.microarch << ")\n"
              << "kernel     : " << mi.kernel << "   paranoid=" << mi.paranoid << "\n"
              << "pmu slots  : " << mi.pmu_slots
              << "  (reference events per pass: "
              << perf::reference_events(sess.passes()) << ")\n"
              << "events     : " << sess.event_count() << " supported, "
              << mi.unsupported.size() << " unavailable\n"
              << "passes     : " << sess.passes().size()
              << "  (workload repeated once per pass)\n"
              << "datasets   : " << data.size() << "\n";
    if (!mi.unsupported.empty()) {
        std::cerr << "unavailable: ";
        for (auto const& u : mi.unsupported) std::cerr << u << " ";
        std::cerr << "\n";
    }
    std::cerr << "\n";

    struct impl_def {
        char const* name;
        char op;
        std::size_t (*fn)(std::string const&);
    };
    std::vector<impl_def> impls;
    if (ops.find('p') != std::string::npos) {
        impls.push_back({"boost (pool)",   'p', &parse_pool});
        impls.push_back({"boost",          'p', &parse_default});
        impls.push_back({"boost (null)",   'p', &parse_null_parser});
    }

    // Build the task list and calibrate iteration counts ONCE, before any
    // pass runs. Calibrating per pass would make each pass execute a different
    // amount of work, which shows up as instruction drift in the stitch check.
    struct task {
        perf::row_key key;
        std::size_t bytes;
        std::size_t rep;
        std::function<std::size_t()> once;
    };
    std::vector<task> tasks;
    std::vector<std::unique_ptr<serializer_fixture>> fixtures;

    auto calibrate = [&](std::function<std::size_t()> const& once) {
        std::size_t rep = 1;
        for (;;) {
            auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < rep; ++i) { volatile auto s = once(); (void)s; }
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (ms >= min_ms || rep >= (1u << 22)) return rep;
            rep = std::max<std::size_t>(rep * 2,
                std::size_t(rep * min_ms / std::max(ms, 0.01)));
        }
    };

    for (auto const& d : data) {
        for (auto const& im : impls) {
            std::function<std::size_t()> once = [&d, fn = im.fn] { return fn(d.text); };
            tasks.push_back({{d.name, im.name, "parse"}, d.text.size(),
                             calibrate(once), once});
        }
        if (ops.find('s') != std::string::npos) {
            fixtures.push_back(std::make_unique<serializer_fixture>(d.text));
            auto* fx = fixtures.back().get();
            std::function<std::size_t()> once = [fx] { return fx->run(); };
            tasks.push_back({{d.name, "boost", "serialize"}, d.text.size(),
                             calibrate(once), once});
        }
    }

    auto const npass = sess.passes().size();
    for (std::size_t p = 0; p < npass; ++p) {
        std::cerr << "pass " << (p + 1) << "/" << npass << ": ";
        for (auto const& e : sess.passes()[p])
            if (e.pmu) std::cerr << e.name << " ";
        std::cerr << "\n";

        for (auto const& t : tasks)
            for (int r = 0; r < reps; ++r)
                sess.measure(t.key, t.bytes, p, [&] {
                    for (std::size_t i = 0; i < t.rep; ++i) {
                        volatile auto s = t.once();
                        (void)s;
                    }
                    return t.rep;
                });
    }

    { std::ofstream f(prefix + ".csv");            sess.write_csv(f); }
    { std::ofstream f(prefix + ".machine.json");   f << perf::to_json(mi); }
    std::cerr << "\nwrote " << prefix << ".csv and "
              << prefix << ".machine.json\n";

    // Report any row whose instruction count drifted between passes.
    int bad = 0;
    for (auto const& [k, r] : sess.rows())
        if (r.pass_drift() > 0.005) {
            if (!bad++) std::cerr << "\nWARNING: instruction drift across passes:\n";
            std::cerr << "  " << k.dataset << " / " << k.impl << " / " << k.op
                      << "  drift=" << (r.pass_drift() * 100) << "%\n";
        }
    if (bad) std::cerr << "  (stitched counters for these rows are suspect)\n";
    return 0;
}
