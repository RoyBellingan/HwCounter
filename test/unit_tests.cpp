// unit_tests - Boost.Test cases for the project invariants.
//
//   make test
//
// No test here opens a perf event, so the tests also run where
// perf_event_paranoid forbids counting (CI containers).

#define BOOST_TEST_MODULE hwCounter
#include <boost/test/included/unit_test.hpp>

#include <perf/session.hpp>

#include <hwc/csv.hpp>
#include <hwc/derived.hpp>
#include <hwc/http_util.hpp>
#include <hwc/store.hpp>

#include <sqlite3.h>

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

perf::event_def pmu(char const* n, bool core = false) {
    return perf::event_def{n, PERF_TYPE_HARDWARE, 0, true, core};
}
perf::event_def soft(char const* n) {
    return perf::event_def{n, PERF_TYPE_SOFTWARE, 0, false, false};
}

std::vector<perf::event_def> sample_events(int n_rot) {
    std::vector<perf::event_def> v{pmu("instructions", true), pmu("cycles", true)};
    static std::vector<std::string> names;
    names.clear();
    for (int i = 0; i < n_rot; ++i) names.push_back("ev" + std::to_string(i));
    for (auto const& n : names) v.push_back(pmu(n.c_str()));
    v.push_back(soft("task-clock-ns"));
    v.push_back(soft("page-faults"));
    return v;
}

// Every pass fits the slots, every event is measured, soft events ride along.
void check_plan(std::vector<perf::event_def> const& ev, int slots) {
    auto passes = perf::plan_passes(ev, slots);
    BOOST_TEST_REQUIRE(!passes.empty());
    std::set<std::string> seen;
    for (auto const& p : passes) {
        int n_pmu = 0, n_soft = 0;
        for (auto const& e : p) {
            seen.insert(e.name);
            if (e.pmu) ++n_pmu; else ++n_soft;
        }
        BOOST_TEST(n_pmu <= std::max(1, slots));
        BOOST_TEST(n_soft == 2);
    }
    for (auto const& e : ev) BOOST_TEST(seen.count(e.name) == 1u, e.name);
}

struct TempDb {
    fs::path path;
    TempDb() {
        path = fs::temp_directory_path() /
               ("hwc_test_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter()++) + ".sqlite");
        remove();
    }
    ~TempDb() { remove(); }
    void remove() {
        std::error_code ec;
        for (auto sfx : {"", "-wal", "-shm"})
            fs::remove(path.string() + sfx, ec);
    }
    static int& counter() { static int c = 0; return c; }
};

boost::json::object machine(std::string host, std::string key = "AMD|25|97|2|7.0",
                            std::string compiler = "gcc 15") {
    boost::json::object m;
    m["hostname"] = host;
    m["comparability_key"] = key;
    m["compiler"] = compiler;
    return m;
}

boost::json::value body(boost::json::object m, std::string sha, std::string flags,
                        double ins_per_call, std::string label = "") {
    boost::json::object meta;
    meta["json_sha"] = sha;
    meta["cxxflags"] = flags;
    meta["label"] = label;
    boost::json::array rows;
    for (auto ds : {"a.json", "b.json"}) {
        boost::json::object r;
        r["dataset"] = ds;
        r["impl"] = "boost";
        r["op"] = "parse";
        r["bytes"] = 1000;
        r["calls"] = 10;
        r["ns_per_call"] = 500.0;
        r["pass_drift"] = 0.0;
        r["instructions"] = ins_per_call;
        r["cycles"] = ins_per_call / 4;
        r["task-clock-ns"] = 400.0;
        r["branches"] = 100.0;
        rows.push_back(std::move(r));
    }
    boost::json::object b;
    b["meta"] = std::move(meta);
    b["machine"] = std::move(m);
    b["rows"] = std::move(rows);
    return b;
}

std::string const kBase = hwc::kUatBaselineSha;
std::string const kOther = "1111111111111111111111111111111111111111";

}  // namespace

// ---------------------------------------------------------------- planning

BOOST_AUTO_TEST_SUITE(pass_planning)

BOOST_AUTO_TEST_CASE(many_slots_keep_both_core_events_in_every_pass) {
    auto ev = sample_events(7);
    for (int slots : {3, 4, 5, 6, 12}) check_plan(ev, slots);
    auto passes = perf::plan_passes(ev, 5);
    BOOST_TEST(passes.size() == 3u);  // 7 rotated / (5 - 2) per pass
    BOOST_TEST(perf::reference_events(passes) == 2u);
}

BOOST_AUTO_TEST_CASE(two_slots_keep_instructions_and_rotate_cycles) {
    auto ev = sample_events(3);
    check_plan(ev, 2);
    auto passes = perf::plan_passes(ev, 2);
    BOOST_TEST(passes.size() == 4u);  // cycles + 3 rotated, one each
    for (auto const& p : passes) BOOST_TEST(p.front().name == "instructions");
    BOOST_TEST(perf::reference_events(passes) == 1u);
}

BOOST_AUTO_TEST_CASE(one_slot_gives_one_event_per_pass) {
    auto ev = sample_events(3);
    check_plan(ev, 1);
    auto passes = perf::plan_passes(ev, 1);
    BOOST_TEST(passes.size() == 5u);
    BOOST_TEST(perf::reference_events(passes) == 0u);
}

BOOST_AUTO_TEST_CASE(core_only_does_not_loop) {
    auto ev = sample_events(0);
    for (int slots : {0, 1, 2, 3}) check_plan(ev, slots);
    BOOST_TEST(perf::plan_passes(ev, 2).size() == 1u);
    BOOST_TEST(perf::plan_passes(ev, 1).size() == 2u);
}

BOOST_AUTO_TEST_CASE(everything_fits_in_one_pass) {
    auto ev = sample_events(2);
    auto passes = perf::plan_passes(ev, 4);
    BOOST_TEST(passes.size() == 1u);
    BOOST_TEST(perf::reference_events(passes) == 2u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- session math

BOOST_AUTO_TEST_SUITE(session_math)

BOOST_AUTO_TEST_CASE(spread_does_not_depend_on_sample_order) {
    std::vector<double> v{1000, 1030, 1010, 1002};
    std::sort(v.begin(), v.end());
    double first = -1;
    do {
        perf::min_max m;
        for (double x : v) m.add(x);
        if (first < 0) first = m.spread();
        BOOST_TEST(m.spread() == first);
    } while (std::next_permutation(v.begin(), v.end()));
    BOOST_TEST(first == 0.03, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(pass_drift_uses_per_pass_minimum) {
    perf::row r;
    r.ins_by_pass[0].add(1000);
    r.ins_by_pass[0].add(1500);  // noisy rep, must not count as drift
    r.ins_by_pass[1].add(1010);
    BOOST_TEST(r.pass_drift() == 0.01, boost::test_tools::tolerance(1e-12));
    BOOST_TEST(r.ins_spread() == 0.5, boost::test_tools::tolerance(1e-12));
}

BOOST_AUTO_TEST_CASE(best_sample_ratios) {
    perf::row r;
    BOOST_TEST(std::isnan(r.best_ipc()));
    BOOST_TEST(std::isnan(r.best_ghz()));
    r.best_ins = 4000; r.best_cyc = 1000; r.best_tc = 250;
    BOOST_TEST(r.best_ipc() == 4.0);
    BOOST_TEST(r.best_ghz() == 4.0);
}

BOOST_AUTO_TEST_CASE(csv_field_round_trip) {
    std::vector<std::string> in{"plain", "a,b.json", "say \"hi\"", "", "x\"y,z"};
    std::string line;
    for (std::size_t i = 0; i < in.size(); ++i)
        line += (i ? "," : "") + perf::csv_field(in[i]);
    auto out = hwc::split_csv_line(line);
    BOOST_TEST(out == in, boost::test_tools::per_element());
    BOOST_TEST(perf::csv_field("plain") == "plain");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- fingerprint

BOOST_AUTO_TEST_SUITE(fingerprint)

BOOST_AUTO_TEST_CASE(md5_known_vectors) {
    BOOST_TEST(perf::detail::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    BOOST_TEST(perf::detail::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    BOOST_TEST(perf::detail::md5_hex(std::string(1000, 'a')) ==
               "cabe45dcc9ae5b66ba86600cca6b8ba8");
}

BOOST_AUTO_TEST_CASE(to_json_escapes_control_bytes) {
    perf::machine_info mi;
    mi.hostname = "tab\there";
    mi.cpu_model = std::string("nul\x01 \"quoted\" back\\slash\r\n");
    mi.compiler = "gcc 15";
    mi.aliases["weird\x1f"] = "event=0x1";
    mi.unsupported = {"a\bb"};
    auto v = boost::json::parse(perf::to_json(mi));
    auto const& o = v.as_object();
    BOOST_TEST(std::string(o.at("hostname").as_string()) == mi.hostname);
    BOOST_TEST(std::string(o.at("cpu_model").as_string()) == mi.cpu_model);
    BOOST_TEST(std::string(o.at("instruction_key").as_string()) == "gcc 15");
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- http

BOOST_AUTO_TEST_SUITE(http_util)

BOOST_AUTO_TEST_CASE(parse_urls) {
    auto u = hwc::parse_http_url("http://host:9000/x");
    BOOST_TEST(u.host == "host");
    BOOST_TEST(u.port == "9000");
    BOOST_TEST(u.path == "/x");

    u = hwc::parse_http_url("http://[::1]:8080");
    BOOST_TEST(u.host == "::1");
    BOOST_TEST(u.port == "8080");
    BOOST_TEST(u.path == "/");

    u = hwc::parse_http_url("[fe80::1]/api/runs");
    BOOST_TEST(u.host == "fe80::1");
    BOOST_TEST(u.port == "80");

    BOOST_CHECK_THROW(hwc::parse_http_url("http://::1:8080"), std::runtime_error);
    BOOST_CHECK_THROW(hwc::parse_http_url("http://[::1"), std::runtime_error);
    BOOST_CHECK_THROW(hwc::parse_http_url("http://host:80a"), std::runtime_error);
    BOOST_CHECK_THROW(hwc::parse_http_url("https://host"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(path_boundary) {
    BOOST_TEST(hwc::path_within("/tmp/web", "/tmp/web/index.html"));
    BOOST_TEST(hwc::path_within("/tmp/web", "/tmp/web"));
    BOOST_TEST(hwc::path_within("/tmp/web/", "/tmp/web/app.js"));
    BOOST_TEST(!hwc::path_within("/tmp/web", "/tmp/website/index.html"));
    BOOST_TEST(!hwc::path_within("/tmp/web", "/tmp"));
    BOOST_TEST(!hwc::path_within("/tmp/web", "/etc/passwd"));
}

BOOST_AUTO_TEST_CASE(token_compare) {
    BOOST_TEST(hwc::constant_time_equal("Bearer abc", "Bearer abc"));
    BOOST_TEST(!hwc::constant_time_equal("Bearer abc", "Bearer abd"));
    BOOST_TEST(!hwc::constant_time_equal("Bearer ab", "Bearer abc"));
    BOOST_TEST(!hwc::constant_time_equal("", "x"));
    BOOST_TEST(hwc::constant_time_equal("", ""));
}

BOOST_AUTO_TEST_CASE(query_parsing) {
    auto q = hwc::parse_query("a=1&b=x%20y&c=+&flag");
    BOOST_TEST(q["a"] == "1");
    BOOST_TEST(q["b"] == "x y");
    BOOST_TEST(q["c"] == " ");
    BOOST_TEST(q.count("flag") == 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- derived

BOOST_AUTO_TEST_SUITE(derived)

BOOST_AUTO_TEST_CASE(ratios_prefer_best_sample_columns) {
    boost::json::object row;
    row["bytes"] = 100;
    row["ns_per_call"] = 1000.0;
    row["instructions"] = 400.0;
    row["cycles"] = 200.0;
    row["task-clock-ns"] = 50.0;
    auto d = hwc::derived_of(row);
    BOOST_TEST(d.at("IPC").as_double() == 2.0);
    BOOST_TEST(d.at("GHz").as_double() == 4.0);
    BOOST_TEST(d.at("ins/byte").as_double() == 4.0);

    row["best_ipc"] = 1.5;
    row["best_ghz"] = 3.5;
    d = hwc::derived_of(row);
    BOOST_TEST(d.at("IPC").as_double() == 1.5);
    BOOST_TEST(d.at("GHz").as_double() == 3.5);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- store

BOOST_AUTO_TEST_SUITE(store)

BOOST_AUTO_TEST_CASE(default_baseline_needs_same_build) {
    TempDb db;
    hwc::Store s(db.path.string());
    auto b_o2 = s.insert_run(body(machine("h1"), kBase, "-O2", 4000)).id;
    auto b_o3 = s.insert_run(body(machine("h1"), kBase, "-O3", 3000)).id;
    auto c_o2 = s.insert_run(body(machine("h1"), kOther, "-O2", 4030)).id;
    (void)b_o3;

    auto r = s.compare(std::nullopt, c_o2).as_object();
    BOOST_TEST(r.at("base").as_object().at("id").as_int64() == b_o2);
    BOOST_TEST(r.at("instructions_comparable").as_bool());
    BOOST_TEST(r.at("comparable").as_bool());
    BOOST_TEST(r.at("warnings").as_array().empty());
    BOOST_TEST(r.at("regressions").as_int64() == 0);  // +0.75% is within 1%

    // No baseline built with -Os: refuse instead of comparing across flags.
    auto c_os = s.insert_run(body(machine("h1"), kOther, "-Os", 4000)).id;
    BOOST_CHECK_THROW(s.compare(std::nullopt, c_os), std::runtime_error);

    // Picked by hand: allowed, but flagged.
    auto x = s.compare(b_o3, c_o2).as_object();
    BOOST_TEST(!x.at("instructions_comparable").as_bool());
    BOOST_TEST(!x.at("comparable").as_bool());
    BOOST_TEST(!x.at("warnings").as_array().empty());
}

BOOST_AUTO_TEST_CASE(default_baseline_prefers_same_host_then_same_compiler) {
    TempDb db;
    hwc::Store s(db.path.string());
    auto other_host = s.insert_run(body(machine("h2"), kBase, "-O2", 4000)).id;
    auto other_cc = s.insert_run(body(machine("h1", "AMD|25|97|2|7.0", "clang 20"),
                                      kBase, "-O2", 4000)).id;
    auto cand = s.insert_run(body(machine("h1"), kOther, "-O2", 4000)).id;
    (void)other_cc;
    auto r = s.compare(std::nullopt, cand).as_object();
    BOOST_TEST(r.at("base").as_object().at("id").as_int64() == other_host);

    auto same = s.insert_run(body(machine("h1"), kBase, "-O2", 4000)).id;
    r = s.compare(std::nullopt, cand).as_object();
    BOOST_TEST(r.at("base").as_object().at("id").as_int64() == same);
}

BOOST_AUTO_TEST_CASE(machine_key_changes_only_second_tier) {
    TempDb db;
    hwc::Store s(db.path.string());
    auto b = s.insert_run(body(machine("h1", "AMD|25|97|2|7.0"), kBase, "-O2", 4000)).id;
    auto c = s.insert_run(body(machine("h1", "AMD|25|97|2|7.1"), kOther, "-O2", 4000)).id;
    auto r = s.compare(b, c).as_object();
    BOOST_TEST(r.at("instructions_comparable").as_bool());
    BOOST_TEST(!r.at("comparable").as_bool());
}

BOOST_AUTO_TEST_CASE(run_summary_is_stored) {
    TempDb db;
    hwc::Store s(db.path.string());
    s.insert_run(body(machine("h1"), kBase, "-O2", 4000));
    s.insert_run(body(machine("h1"), kBase, "-O2", 4040));
    auto runs = s.list_runs({}).as_object().at("runs").as_array();
    BOOST_TEST_REQUIRE(runs.size() == 2u);
    auto const& newest = runs[0].as_object();
    BOOST_TEST(newest.at("n_samples").as_int64() == 2);
    BOOST_TEST(newest.at("median_ins_byte").as_double() == 4.04,
               boost::test_tools::tolerance(1e-9));
    BOOST_TEST(newest.at("median_drift_pct").as_double() == 1.0,
               boost::test_tools::tolerance(1e-9));
    BOOST_TEST(runs[1].as_object().at("median_drift_pct").as_double() == 0.0);
}

BOOST_AUTO_TEST_CASE(sha_filter_needs_minimum_prefix) {
    TempDb db;
    hwc::Store s(db.path.string());
    s.insert_run(body(machine("h1"), kBase, "-O2", 4000));
    s.insert_run(body(machine("h1"), kOther, "-O2", 4000));
    auto count = [&](std::string sha) {
        hwc::Query q;
        q.json_sha = sha;
        return s.list_runs(q).as_object().at("runs").as_array().size();
    };
    BOOST_TEST(count("") == 2u);
    BOOST_TEST(count("e") == 0u);
    BOOST_TEST(count("e93cf9c") == 1u);
    BOOST_TEST(count(kBase) == 1u);
}

BOOST_AUTO_TEST_CASE(duplicate_samples_are_rejected) {
    TempDb db;
    hwc::Store s(db.path.string());
    auto b = body(machine("h1"), kBase, "-O2", 4000);
    auto& rows = b.as_object()["rows"].as_array();
    rows.push_back(rows[0]);
    BOOST_CHECK_THROW(s.insert_run(b), std::runtime_error);
    BOOST_TEST(s.list_runs({}).as_object().at("runs").as_array().empty());
}

BOOST_AUTO_TEST_CASE(metrics_sees_every_counter) {
    TempDb db;
    hwc::Store s(db.path.string());
    for (int i = 0; i < 150; ++i)
        s.insert_run(body(machine("h1"), kBase, "-O2", 4000));
    auto late = body(machine("h1"), kBase, "-O2", 4000);
    late.as_object()["rows"].as_array()[0].as_object()["late-event"] = 1.0;
    s.insert_run(late);
    auto ev = s.metrics().as_object().at("events").as_array();
    bool found = std::any_of(ev.begin(), ev.end(), [](auto const& v) {
        return v.as_string() == "late-event";
    });
    BOOST_TEST(found);
}

BOOST_AUTO_TEST_CASE(old_schema_is_migrated) {
    TempDb db;
    {
        sqlite3* raw = nullptr;
        BOOST_TEST_REQUIRE(sqlite3_open(db.path.c_str(), &raw) == SQLITE_OK);
        char const* old = R"SQL(
CREATE TABLE machines (id INTEGER PRIMARY KEY, hostname TEXT NOT NULL,
  comparability_key TEXT NOT NULL, vendor TEXT, cpu_family TEXT, cpu_model_id TEXT,
  stepping TEXT, cpu_model TEXT, microarch TEXT, kernel TEXT, compiler TEXT,
  nproc INTEGER, smt INTEGER, pmu_slots INTEGER, governor TEXT, boost TEXT,
  machine_json TEXT NOT NULL);
CREATE TABLE runs (id INTEGER PRIMARY KEY, created_at TEXT NOT NULL,
  machine_id INTEGER NOT NULL REFERENCES machines(id), hostname TEXT,
  json_sha TEXT NOT NULL, json_ref TEXT, hwc_sha TEXT, compiler TEXT,
  cxxflags TEXT NOT NULL, pin INTEGER, min_ms REAL, reps INTEGER, label TEXT,
  note TEXT, started_at TEXT, finished_at TEXT);
CREATE TABLE samples (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES runs(id),
  dataset TEXT NOT NULL, impl TEXT NOT NULL, op TEXT NOT NULL, bytes INTEGER,
  calls INTEGER, ns_per_call REAL, pass_drift REAL, ins_spread REAL, cyc_spread REAL,
  multiplexed INTEGER, instructions REAL, cycles REAL, task_clock_ns REAL,
  counters_json TEXT NOT NULL);
INSERT INTO machines VALUES(1,'h1','k',NULL,NULL,NULL,NULL,'cpu',NULL,NULL,'gcc',
  0,0,0,NULL,NULL,'{}');
INSERT INTO runs VALUES(1,'2026-01-01',1,'h1','abc','','','gcc','-O2',-1,200,3,
  '','','','');
INSERT INTO samples VALUES(1,1,'a.json','boost','parse',100,1,1,0,0,0,0,500,100,1,'{}');
INSERT INTO samples VALUES(2,1,'a.json','boost','parse',100,1,1,0,0,0,0,500,100,1,'{}');
)SQL";
        BOOST_TEST_REQUIRE(sqlite3_exec(raw, old, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(raw);
    }
    // Duplicate samples: the unique index is skipped with a warning, the
    // store still opens and the summary is back-filled.
    hwc::Store s(db.path.string());
    auto runs = s.list_runs({}).as_object().at("runs").as_array();
    BOOST_TEST_REQUIRE(runs.size() == 1u);
    BOOST_TEST(runs[0].as_object().at("n_samples").as_int64() == 2);
    BOOST_TEST(runs[0].as_object().at("median_ins_byte").as_double() == 5.0);
}

BOOST_AUTO_TEST_SUITE_END()
