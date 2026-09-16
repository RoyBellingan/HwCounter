#include <hwc/store.hpp>
#include <hwc/derived.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace hwc {
namespace {

char const* kSchema = R"SQL(
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS machines (
  id INTEGER PRIMARY KEY,
  hostname TEXT NOT NULL,
  comparability_key TEXT NOT NULL,
  vendor TEXT,
  cpu_family TEXT,
  cpu_model_id TEXT,
  stepping TEXT,
  cpu_model TEXT,
  microarch TEXT,
  kernel TEXT,
  compiler TEXT,
  nproc INTEGER,
  smt INTEGER,
  pmu_slots INTEGER,
  governor TEXT,
  boost TEXT,
  machine_json TEXT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_machines_fp
  ON machines(comparability_key, hostname, compiler);
CREATE TABLE IF NOT EXISTS runs (
  id INTEGER PRIMARY KEY,
  created_at TEXT NOT NULL,
  machine_id INTEGER NOT NULL REFERENCES machines(id),
  hostname TEXT,
  json_sha TEXT NOT NULL,
  json_ref TEXT,
  hwc_sha TEXT,
  compiler TEXT,
  cxxflags TEXT NOT NULL,
  pin INTEGER,
  min_ms REAL,
  reps INTEGER,
  label TEXT,
  note TEXT,
  started_at TEXT,
  finished_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_runs_sha ON runs(json_sha);
CREATE INDEX IF NOT EXISTS idx_runs_host ON runs(hostname);
CREATE TABLE IF NOT EXISTS samples (
  id INTEGER PRIMARY KEY,
  run_id INTEGER NOT NULL REFERENCES runs(id),
  dataset TEXT NOT NULL,
  impl TEXT NOT NULL,
  op TEXT NOT NULL,
  bytes INTEGER,
  calls INTEGER,
  ns_per_call REAL,
  pass_drift REAL,
  ins_spread REAL,
  cyc_spread REAL,
  multiplexed INTEGER,
  instructions REAL,
  cycles REAL,
  task_clock_ns REAL,
  counters_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_samples_run ON samples(run_id);
)SQL";

char const* kIndexed[] = {
    "dataset", "impl", "op", "bytes", "calls", "ns_per_call",
    "pass_drift", "ins_spread", "cyc_spread", "multiplexed",
    "instructions", "cycles", "task-clock-ns", "task_clock_ns"
};

bool is_indexed(std::string const& k) {
    for (auto* s : kIndexed) if (k == s) return true;
    return false;
}

std::string now_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string jstr(boost::json::object const& o, char const* k, char const* def = "") {
    auto it = o.find(k);
    if (it == o.end() || it->value().is_null()) return def;
    if (it->value().is_string()) return std::string(it->value().as_string());
    if (it->value().is_int64()) return std::to_string(it->value().as_int64());
    if (it->value().is_double()) return std::to_string(it->value().as_double());
    return def;
}

std::optional<double> jnum(boost::json::object const& o, char const* k) {
    return get_num(o, k);
}

int jint(boost::json::object const& o, char const* k, int def = 0) {
    if (auto v = get_num(o, k)) return static_cast<int>(*v);
    return def;
}

std::string require_str(boost::json::object const& o, char const* k) {
    auto s = jstr(o, k);
    if (s.empty()) throw std::runtime_error(std::string("missing required field: ") + k);
    return s;
}

boost::json::object row_for_derived(boost::json::object const& counters,
                                    double bytes, double ns, double ins, double cyc,
                                    double tc, double cyc_spread) {
    boost::json::object r = counters;
    r["bytes"] = bytes;
    r["ns_per_call"] = ns;
    r["instructions"] = ins;
    r["cycles"] = cyc;
    r["task-clock-ns"] = tc;
    r["cyc_spread"] = cyc_spread;
    return r;
}

double median(std::vector<double> v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    auto n = v.size();
    if (n % 2) return v[n / 2];
    return 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double mean_of(std::vector<double> const& v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

double cv_pct(std::vector<double> const& v) {
    if (v.size() < 2) return 0.0;
    double m = mean_of(v);
    if (!(m > 0) && !(m < 0)) return 0.0;
    double acc = 0;
    for (double x : v) acc += (x - m) * (x - m);
    double sd = std::sqrt(acc / static_cast<double>(v.size()));
    return 100.0 * sd / std::abs(m);
}

bool sha_match(std::string const& have, std::string const& want) {
    if (want.empty()) return true;
    if (have.size() >= want.size() && have.compare(0, want.size(), want) == 0)
        return true;
    return have == want;
}

struct Sample {
    std::int64_t run_id = 0;
    std::string dataset, impl, op;
    double bytes = 0, calls = 0, ns = 0;
    double pass_drift = 0, ins_spread = 0, cyc_spread = 0;
    int multiplexed = 0;
    double instructions = 0, cycles = 0, task_clock_ns = 0;
    boost::json::object counters;
    boost::json::object derived() const {
        return derived_of(row_for_derived(counters, bytes, ns, instructions,
                                          cycles, task_clock_ns, cyc_spread));
    }
};

struct RunRec {
    std::int64_t id = 0, machine_id = 0;
    std::string created_at, hostname, json_sha, json_ref, hwc_sha;
    std::string compiler, cxxflags, label, note, started_at, finished_at;
    std::string cpu_model, comparability_key, machine_json;
    int pin = -1, reps = 0;
    double min_ms = 0;
};

class Stmt {
public:
    Stmt(sqlite3* db, char const* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    Stmt(Stmt const&) = delete;
    sqlite3_stmt* get() { return st_; }
    void bind_text(int i, std::string const& s) {
        sqlite3_bind_text(st_, i, s.c_str(), -1, SQLITE_TRANSIENT);
    }
    void bind_null(int i) { sqlite3_bind_null(st_, i); }
    void bind_i64(int i, std::int64_t v) { sqlite3_bind_int64(st_, i, v); }
    void bind_int(int i, int v) { sqlite3_bind_int(st_, i, v); }
    void bind_dbl(int i, double v) {
        if (std::isfinite(v)) sqlite3_bind_double(st_, i, v);
        else sqlite3_bind_null(st_, i);
    }
    void bind_opt(int i, std::optional<double> v) {
        if (v) bind_dbl(i, *v); else bind_null(i);
    }
    int step() { return sqlite3_step(st_); }
    void reset() { sqlite3_reset(st_); sqlite3_clear_bindings(st_); }
    std::string col_text(int i) {
        auto* p = reinterpret_cast<char const*>(sqlite3_column_text(st_, i));
        return p ? std::string(p) : std::string();
    }
    std::int64_t col_i64(int i) { return sqlite3_column_int64(st_, i); }
    double col_dbl(int i) {
        if (sqlite3_column_type(st_, i) == SQLITE_NULL)
            return std::numeric_limits<double>::quiet_NaN();
        return sqlite3_column_double(st_, i);
    }
    int col_int(int i) { return sqlite3_column_int(st_, i); }
private:
    sqlite3_stmt* st_ = nullptr;
};

boost::json::object run_json(RunRec const& r, int n_samples = -1) {
    boost::json::object o;
    o["id"] = r.id;
    o["created_at"] = r.created_at;
    o["hostname"] = r.hostname;
    o["json_sha"] = r.json_sha;
    o["json_ref"] = r.json_ref;
    o["hwc_sha"] = r.hwc_sha;
    o["compiler"] = r.compiler;
    o["cxxflags"] = r.cxxflags;
    o["pin"] = r.pin;
    o["min_ms"] = r.min_ms;
    o["reps"] = r.reps;
    o["label"] = r.label;
    o["note"] = r.note;
    o["started_at"] = r.started_at;
    o["finished_at"] = r.finished_at;
    o["cpu_model"] = r.cpu_model;
    o["comparability_key"] = r.comparability_key;
    if (n_samples >= 0) o["n_samples"] = n_samples;
    return o;
}

Sample sample_from_stmt(Stmt& st) {
    Sample s;
    s.run_id = st.col_i64(0);
    s.dataset = st.col_text(1);
    s.impl = st.col_text(2);
    s.op = st.col_text(3);
    s.bytes = st.col_dbl(4);
    s.calls = st.col_dbl(5);
    s.ns = st.col_dbl(6);
    s.pass_drift = st.col_dbl(7);
    s.ins_spread = st.col_dbl(8);
    s.cyc_spread = st.col_dbl(9);
    s.multiplexed = st.col_int(10);
    s.instructions = st.col_dbl(11);
    s.cycles = st.col_dbl(12);
    s.task_clock_ns = st.col_dbl(13);
    auto cj = st.col_text(14);
    if (!cj.empty()) {
        auto v = boost::json::parse(cj);
        if (v.is_object()) s.counters = v.as_object();
    }
    return s;
}

char const* kSampleSelect =
    "SELECT run_id, dataset, impl, op, bytes, calls, ns_per_call, pass_drift,"
    " ins_spread, cyc_spread, multiplexed, instructions, cycles, task_clock_ns,"
    " counters_json FROM samples";

}  // namespace

Store::Store(std::string path) {
    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.has_parent_path() && !p.parent_path().empty())
        fs::create_directories(p.parent_path());
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string e = db_ ? sqlite3_errmsg(db_) : "sqlite open failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(e);
    }
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA journal_mode = WAL");
    exec(kSchema);
}

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

void Store::exec(char const* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "exec failed";
        sqlite3_free(err);
        throw std::runtime_error(e);
    }
}

std::int64_t Store::upsert_machine(boost::json::object const& m) {
    auto hostname = jstr(m, "hostname");
    auto key = jstr(m, "comparability_key");
    auto compiler = jstr(m, "compiler");
    if (hostname.empty() || key.empty())
        throw std::runtime_error("machine.json missing hostname or comparability_key");

    {
        Stmt st(db_, "SELECT id FROM machines WHERE comparability_key=? AND hostname=? AND compiler=?");
        st.bind_text(1, key);
        st.bind_text(2, hostname);
        st.bind_text(3, compiler);
        if (st.step() == SQLITE_ROW) return st.col_i64(0);
    }

    Stmt st(db_,
        "INSERT INTO machines(hostname,comparability_key,vendor,cpu_family,cpu_model_id,"
        "stepping,cpu_model,microarch,kernel,compiler,nproc,smt,pmu_slots,governor,boost,"
        "machine_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    st.bind_text(1, hostname);
    st.bind_text(2, key);
    st.bind_text(3, jstr(m, "vendor"));
    st.bind_text(4, jstr(m, "cpu_family"));
    st.bind_text(5, jstr(m, "cpu_model_id"));
    st.bind_text(6, jstr(m, "stepping"));
    st.bind_text(7, jstr(m, "cpu_model"));
    st.bind_text(8, jstr(m, "microarch"));
    st.bind_text(9, jstr(m, "kernel"));
    st.bind_text(10, compiler);
    st.bind_int(11, jint(m, "nproc"));
    st.bind_int(12, m.contains("smt_active") && m.at("smt_active").is_bool()
                    ? (m.at("smt_active").as_bool() ? 1 : 0) : 0);
    st.bind_int(13, jint(m, "pmu_slots_detected"));
    st.bind_text(14, jstr(m, "cpufreq_governor"));
    st.bind_text(15, jstr(m, "boost"));
    st.bind_text(16, boost::json::serialize(m));
    if (st.step() != SQLITE_DONE)
        throw std::runtime_error(std::string("insert machine: ") + sqlite3_errmsg(db_));
    return sqlite3_last_insert_rowid(db_);
}

Store::InsertResult Store::insert_run(boost::json::value const& body) {
    if (!body.is_object()) throw std::runtime_error("body must be a JSON object");
    auto const& o = body.as_object();
    if (!o.contains("meta") || !o.at("meta").is_object())
        throw std::runtime_error("missing meta object");
    if (!o.contains("machine") || !o.at("machine").is_object())
        throw std::runtime_error("missing machine object");
    if (!o.contains("rows") || !o.at("rows").is_array())
        throw std::runtime_error("missing rows array");
    auto const& meta = o.at("meta").as_object();
    auto const& machine = o.at("machine").as_object();
    auto const& rows = o.at("rows").as_array();
    if (rows.empty()) throw std::runtime_error("rows array is empty");

    auto json_sha = require_str(meta, "json_sha");
    auto cxxflags = require_str(meta, "cxxflags");

    exec("BEGIN IMMEDIATE");
    InsertResult r;
    try {
        auto mid = upsert_machine(machine);
        Stmt st(db_,
            "INSERT INTO runs(created_at,machine_id,hostname,json_sha,json_ref,hwc_sha,"
            "compiler,cxxflags,pin,min_ms,reps,label,note,started_at,finished_at)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        st.bind_text(1, now_utc());
        st.bind_i64(2, mid);
        st.bind_text(3, jstr(machine, "hostname"));
        st.bind_text(4, json_sha);
        st.bind_text(5, jstr(meta, "json_ref"));
        st.bind_text(6, jstr(meta, "hwc_sha"));
        st.bind_text(7, jstr(machine, "compiler"));
        st.bind_text(8, cxxflags);
        st.bind_int(9, jint(meta, "pin", -1));
        st.bind_opt(10, jnum(meta, "min_ms"));
        st.bind_int(11, jint(meta, "reps"));
        st.bind_text(12, jstr(meta, "label"));
        st.bind_text(13, jstr(meta, "note"));
        st.bind_text(14, jstr(meta, "started_at"));
        st.bind_text(15, jstr(meta, "finished_at"));
        if (st.step() != SQLITE_DONE)
            throw std::runtime_error(std::string("insert run: ") + sqlite3_errmsg(db_));
        r.id = sqlite3_last_insert_rowid(db_);

        Stmt ins(db_,
            "INSERT INTO samples(run_id,dataset,impl,op,bytes,calls,ns_per_call,pass_drift,"
            "ins_spread,cyc_spread,multiplexed,instructions,cycles,task_clock_ns,counters_json)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        for (auto const& rv : rows) {
            if (!rv.is_object()) throw std::runtime_error("row is not an object");
            auto const& row = rv.as_object();
            auto dataset = jstr(row, "dataset");
            auto impl = jstr(row, "impl");
            auto op = jstr(row, "op");
            if (dataset.empty() || impl.empty() || op.empty())
                throw std::runtime_error("row missing dataset/impl/op");
            boost::json::object counters;
            for (auto const& [k, v] : row) {
                std::string ks(k);
                if (!is_indexed(ks)) counters[ks] = v;
            }
            auto tc = jnum(row, "task-clock-ns");
            if (!tc) tc = jnum(row, "task_clock_ns");
            ins.reset();
            ins.bind_i64(1, r.id);
            ins.bind_text(2, dataset);
            ins.bind_text(3, impl);
            ins.bind_text(4, op);
            ins.bind_opt(5, jnum(row, "bytes"));
            ins.bind_opt(6, jnum(row, "calls"));
            ins.bind_opt(7, jnum(row, "ns_per_call"));
            ins.bind_opt(8, jnum(row, "pass_drift"));
            ins.bind_opt(9, jnum(row, "ins_spread"));
            ins.bind_opt(10, jnum(row, "cyc_spread"));
            ins.bind_int(11, jint(row, "multiplexed"));
            ins.bind_opt(12, jnum(row, "instructions"));
            ins.bind_opt(13, jnum(row, "cycles"));
            ins.bind_opt(14, tc);
            ins.bind_text(15, boost::json::serialize(counters));
            if (ins.step() != SQLITE_DONE)
                throw std::runtime_error(std::string("insert sample: ") + sqlite3_errmsg(db_));
            ++r.n_samples;
        }
        exec("COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return r;
}

namespace {

RunRec load_run_row(Stmt& st) {
    RunRec r;
    r.id = st.col_i64(0);
    r.created_at = st.col_text(1);
    r.machine_id = st.col_i64(2);
    r.hostname = st.col_text(3);
    r.json_sha = st.col_text(4);
    r.json_ref = st.col_text(5);
    r.hwc_sha = st.col_text(6);
    r.compiler = st.col_text(7);
    r.cxxflags = st.col_text(8);
    r.pin = st.col_int(9);
    r.min_ms = st.col_dbl(10);
    r.reps = st.col_int(11);
    r.label = st.col_text(12);
    r.note = st.col_text(13);
    r.started_at = st.col_text(14);
    r.finished_at = st.col_text(15);
    r.cpu_model = st.col_text(16);
    r.comparability_key = st.col_text(17);
    r.machine_json = st.col_text(18);
    return r;
}

char const* kRunJoin =
    "SELECT r.id,r.created_at,r.machine_id,r.hostname,r.json_sha,r.json_ref,r.hwc_sha,"
    "r.compiler,r.cxxflags,r.pin,r.min_ms,r.reps,r.label,r.note,r.started_at,r.finished_at,"
    "m.cpu_model,m.comparability_key,m.machine_json "
    "FROM runs r JOIN machines m ON m.id=r.machine_id";

std::vector<RunRec> load_runs(sqlite3* db, Query const& q) {
    std::string sql = std::string(kRunJoin) + " WHERE 1=1";
    if (!q.hostname.empty()) sql += " AND r.hostname = ?";
    if (!q.compiler.empty()) sql += " AND r.compiler = ?";
    if (!q.cxxflags.empty()) sql += " AND r.cxxflags = ?";
    if (!q.label.empty()) sql += " AND r.label = ?";
    sql += " ORDER BY r.id ASC";
    Stmt st(db, sql.c_str());
    int i = 1;
    if (!q.hostname.empty()) st.bind_text(i++, q.hostname);
    if (!q.compiler.empty()) st.bind_text(i++, q.compiler);
    if (!q.cxxflags.empty()) st.bind_text(i++, q.cxxflags);
    if (!q.label.empty()) st.bind_text(i++, q.label);
    std::vector<RunRec> out;
    while (st.step() == SQLITE_ROW) {
        auto r = load_run_row(st);
        if (!sha_match(r.json_sha, q.json_sha)) continue;
        if (q.hide_unknown && r.json_sha == "unknown") continue;
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Sample> load_samples_for(sqlite3* db, std::int64_t run_id) {
    std::string sql = std::string(kSampleSelect) + " WHERE run_id=?";
    Stmt st(db, sql.c_str());
    st.bind_i64(1, run_id);
    std::vector<Sample> out;
    while (st.step() == SQLITE_ROW) out.push_back(sample_from_stmt(st));
    return out;
}

std::optional<RunRec> load_run_id(sqlite3* db, std::int64_t id) {
    std::string sql = std::string(kRunJoin) + " WHERE r.id=?";
    Stmt st(db, sql.c_str());
    st.bind_i64(1, id);
    if (st.step() != SQLITE_ROW) return std::nullopt;
    return load_run_row(st);
}

using Key = std::tuple<std::string, std::string, std::string>;

std::optional<double> sample_metric(Sample const& s, std::string const& metric) {
    auto d = s.derived();
    std::string name = metric.empty() ? "ins/byte" : metric;
    return derived_metric(d, name);
}

}  // namespace

boost::json::value Store::list_runs(Query const& q) {
    Query q2 = q;
    q2.hide_unknown = false;  // list shows everything; UI can filter
    auto runs = load_runs(db_, q2);
    // first run of (host, sha, flags) for drift
    std::map<std::tuple<std::string, std::string, std::string>, std::int64_t> first;
    std::map<std::int64_t, std::map<Key, double>> ins;
    for (auto const& r : runs) {
        auto key = std::make_tuple(r.hostname, r.json_sha, r.cxxflags);
        if (!first.count(key)) first[key] = r.id;
        auto ss = load_samples_for(db_, r.id);
        for (auto const& s : ss) {
            auto ib = sample_metric(s, "ins/byte");
            if (ib) ins[r.id][Key{s.dataset, s.impl, s.op}] = *ib;
        }
    }
    boost::json::array arr;
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        auto const& r = *it;
        auto samples = ins[r.id];
        std::vector<double> ibs, drifts;
        for (auto const& [k, v] : samples) ibs.push_back(v);
        auto fk = std::make_tuple(r.hostname, r.json_sha, r.cxxflags);
        auto fid = first[fk];
        if (fid != r.id) {
            auto const& base = ins[fid];
            for (auto const& [k, v] : samples) {
                auto b = base.find(k);
                if (b != base.end() && b->second != 0)
                    drifts.push_back(100.0 * (v - b->second) / b->second);
            }
        } else {
            drifts.push_back(0);
        }
        auto o = run_json(r, static_cast<int>(samples.size()));
        o["median_ins_byte"] = num_or_null(median(ibs));
        o["median_drift_pct"] = num_or_null(median(drifts));
        arr.push_back(std::move(o));
    }
    boost::json::object out;
    out["runs"] = std::move(arr);
    return out;
}

std::string like_contains(std::string_view q) {
    std::string pat;
    pat.reserve(q.size() + 2);
    pat.push_back('%');
    for (char c : q) {
        if (c == '%' || c == '_' || c == '\\') pat.push_back('\\');
        pat.push_back(c);
    }
    pat.push_back('%');
    return pat;
}

boost::json::value Store::suggest(std::string_view field, std::string_view q, int limit) {
    char const* col = nullptr;
    bool short_sha = false;
    if (field == "hostname" || field == "host") col = "hostname";
    else if (field == "json_sha" || field == "sha") {
        col = "json_sha";
        short_sha = true;
    } else if (field == "label") col = "label";
    else throw std::runtime_error("unknown suggest field");
    if (limit < 1) limit = 80;
    if (limit > 200) limit = 200;

    std::string sql = std::string("SELECT DISTINCT ") + col +
        " FROM runs WHERE " + col + " IS NOT NULL AND TRIM(" + col + ") != ''";
    std::string pat;
    if (!q.empty()) {
        sql += " AND ";
        sql += col;
        sql += " LIKE ? ESCAPE '\\'";
        pat = like_contains(q);
    }
    sql += " ORDER BY ";
    sql += col;
    sql += " COLLATE NOCASE LIMIT ?";

    Stmt st(db_, sql.c_str());
    int i = 1;
    if (!pat.empty()) st.bind_text(i++, pat);
    st.bind_int(i, limit);

    boost::json::array results;
    while (st.step() == SQLITE_ROW) {
        auto v = st.col_text(0);
        boost::json::object o;
        o["id"] = v;
        o["text"] = (short_sha && v.size() > 12) ? v.substr(0, 12) : v;
        results.push_back(std::move(o));
    }
    boost::json::object out;
    out["results"] = std::move(results);
    return out;
}

boost::json::value Store::get_run(std::int64_t id) {
    auto r = load_run_id(db_, id);
    if (!r) throw std::runtime_error("run not found");
    auto ss = load_samples_for(db_, id);
    boost::json::array samples;
    for (auto const& s : ss) {
        boost::json::object o;
        o["dataset"] = s.dataset;
        o["impl"] = s.impl;
        o["op"] = s.op;
        o["bytes"] = s.bytes;
        o["calls"] = s.calls;
        o["ns_per_call"] = num_or_null(s.ns);
        o["pass_drift"] = num_or_null(s.pass_drift);
        o["ins_spread"] = num_or_null(s.ins_spread);
        o["cyc_spread"] = num_or_null(s.cyc_spread);
        o["multiplexed"] = s.multiplexed != 0;
        o["instructions"] = num_or_null(s.instructions);
        o["cycles"] = num_or_null(s.cycles);
        o["task_clock_ns"] = num_or_null(s.task_clock_ns);
        o["counters"] = s.counters;
        o["derived"] = s.derived();
        samples.push_back(std::move(o));
    }
    boost::json::object out;
    out["run"] = run_json(*r, static_cast<int>(ss.size()));
    auto mj = boost::json::parse(r->machine_json);
    out["machine"] = mj;
    out["samples"] = std::move(samples);
    return out;
}

boost::json::value Store::stability(Query const& q) {
    std::string metric = q.metric.empty() ? "ins/byte" : q.metric;
    auto runs = load_runs(db_, q);

    struct GKey {
        std::string host, ckey, sha, compiler, flags, dataset, impl, op;
        bool operator<(GKey const& o) const {
            return std::tie(host, ckey, sha, compiler, flags, dataset, impl, op) <
                   std::tie(o.host, o.ckey, o.sha, o.compiler, o.flags, o.dataset, o.impl, o.op);
        }
    };
    struct Acc {
        std::vector<double> xs;
        std::vector<double> spreads, drifts;
    };
    std::map<GKey, Acc> groups;

    for (auto const& r : runs) {
        for (auto const& s : load_samples_for(db_, r.id)) {
            auto v = sample_metric(s, metric);
            if (!v) continue;
            GKey k{r.hostname, r.comparability_key, r.json_sha, r.compiler,
                   r.cxxflags, s.dataset, s.impl, s.op};
            auto& a = groups[k];
            a.xs.push_back(*v);
            a.spreads.push_back(s.ins_spread);
            a.drifts.push_back(s.pass_drift);
        }
    }

    boost::json::array garr;
    for (auto const& [k, a] : groups) {
        boost::json::object o;
        o["hostname"] = k.host;
        o["comparability_key"] = k.ckey;
        o["json_sha"] = k.sha;
        o["compiler"] = k.compiler;
        o["cxxflags"] = k.flags;
        o["dataset"] = k.dataset;
        o["impl"] = k.impl;
        o["op"] = k.op;
        o["n"] = static_cast<std::int64_t>(a.xs.size());
        o["min"] = num_or_null(*std::min_element(a.xs.begin(), a.xs.end()));
        o["max"] = num_or_null(*std::max_element(a.xs.begin(), a.xs.end()));
        o["mean"] = num_or_null(mean_of(a.xs));
        o["cv_pct"] = num_or_null(cv_pct(a.xs));
        o["mean_ins_spread"] = num_or_null(mean_of(a.spreads));
        o["mean_pass_drift"] = num_or_null(mean_of(a.drifts));
        garr.push_back(std::move(o));
    }

    struct XKey {
        std::string sha, flags, dataset, impl, op;
        bool operator<(XKey const& o) const {
            return std::tie(sha, flags, dataset, impl, op) <
                   std::tie(o.sha, o.flags, o.dataset, o.impl, o.op);
        }
    };
    struct HostMean {
        std::string host, ckey;
        double mean = 0;
    };
    std::map<XKey, std::vector<HostMean>> xh;
    for (auto const& [k, a] : groups) {
        xh[XKey{k.sha, k.flags, k.dataset, k.impl, k.op}].push_back(
            HostMean{k.host, k.ckey, mean_of(a.xs)});
    }
    boost::json::array xarr;
    for (auto const& [k, hosts] : xh) {
        if (hosts.size() < 2) continue;
        boost::json::object o;
        o["json_sha"] = k.sha;
        o["cxxflags"] = k.flags;
        o["dataset"] = k.dataset;
        o["impl"] = k.impl;
        o["op"] = k.op;
        boost::json::array ha;
        std::vector<double> means;
        std::string first_key;
        bool same_key = true;
        for (auto const& h : hosts) {
            boost::json::object ho;
            ho["hostname"] = h.host;
            ho["mean"] = num_or_null(h.mean);
            ho["comparability_key"] = h.ckey;
            ha.push_back(std::move(ho));
            if (std::isfinite(h.mean)) means.push_back(h.mean);
            if (first_key.empty()) first_key = h.ckey;
            else if (h.ckey != first_key) same_key = false;
        }
        o["hosts"] = std::move(ha);
        o["cv_pct"] = num_or_null(cv_pct(means));
        o["comparable"] = same_key;
        xarr.push_back(std::move(o));
    }

    boost::json::object out;
    out["metric"] = metric;
    out["groups"] = std::move(garr);
    out["cross_host"] = std::move(xarr);
    return out;
}

boost::json::value Store::compare(std::optional<std::int64_t> base_id,
                                  std::optional<std::int64_t> cand_id) {
    Query open;
    open.hide_unknown = false;
    auto all = load_runs(db_, open);

    auto pick_cand = [&]() -> std::optional<RunRec> {
        if (cand_id) return load_run_id(db_, *cand_id);
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (it->json_sha != "unknown") return *it;
        if (!all.empty()) return all.back();
        return std::nullopt;
    };
    auto cand = pick_cand();
    if (!cand) throw std::runtime_error("no candidate run");

    auto pick_base = [&]() -> std::optional<RunRec> {
        if (base_id) return load_run_id(db_, *base_id);
        std::optional<RunRec> any_base, same_host;
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->id == cand->id) continue;
            if (!sha_match(it->json_sha, kUatBaselineSha) &&
                it->json_sha != kUatBaselineSha)
                continue;
            if (!any_base) any_base = *it;
            if (it->hostname == cand->hostname && it->compiler == cand->compiler) {
                same_host = *it;
                break;
            }
        }
        if (same_host) return same_host;
        if (any_base) return any_base;
        for (auto const& r : all)
            if (r.id != cand->id && r.hostname == cand->hostname) return r;
        return std::nullopt;
    };
    auto base = pick_base();
    if (!base) throw std::runtime_error("no baseline run");

    auto bs = load_samples_for(db_, base->id);
    auto cs = load_samples_for(db_, cand->id);
    std::map<Key, Sample> bm, cm;
    for (auto& s : bs) bm[Key{s.dataset, s.impl, s.op}] = s;
    for (auto& s : cs) cm[Key{s.dataset, s.impl, s.op}] = s;

    bool comparable = base->comparability_key == cand->comparability_key;
    boost::json::array parse, serialize;
    std::vector<double> logs;
    int n = 0, within = 0, regressions = 0;

    std::vector<Key> keys;
    for (auto const& [k, _] : cm) if (bm.count(k)) keys.push_back(k);
    std::sort(keys.begin(), keys.end());

    for (auto const& k : keys) {
        auto const& b = bm[k];
        auto const& c = cm[k];
        auto db = b.derived();
        auto dc = c.derived();
        auto ib = derived_metric(db, "ins/byte");
        auto ic = derived_metric(dc, "ins/byte");
        if (!ib || !ic || *ib == 0) continue;
        double delta = 100.0 * (*ic - *ib) / *ib;
        double ratio = geomean_ratio(*ib, *ic);
        ++n;
        if (std::abs(delta) <= 1.0) ++within;
        if (delta > 1.0) ++regressions;
        if (std::isfinite(ratio) && ratio > 0) logs.push_back(std::log(ratio));

        boost::json::object row;
        row["dataset"] = b.dataset;
        row["impl"] = b.impl;
        row["op"] = b.op;
        row["ins_byte_base"] = *ib;
        row["ins_byte_cand"] = *ic;
        row["delta_pct"] = delta;
        row["ns_base"] = num_or_null(b.ns);
        row["ns_cand"] = num_or_null(c.ns);
        row["task_clock_base"] = num_or_null(b.task_clock_ns);
        row["task_clock_cand"] = num_or_null(c.task_clock_ns);
        row["mb_s_base"] = opt_or_null(derived_metric(db, "MB/s"));
        row["mb_s_cand"] = opt_or_null(derived_metric(dc, "MB/s"));
        row["ipc_base"] = opt_or_null(derived_metric(db, "IPC"));
        row["ipc_cand"] = opt_or_null(derived_metric(dc, "IPC"));
        row["ghz_base"] = opt_or_null(derived_metric(db, "GHz"));
        row["ghz_cand"] = opt_or_null(derived_metric(dc, "GHz"));
        row["cyc_byte_base"] = opt_or_null(derived_metric(db, "cyc/byte"));
        row["cyc_byte_cand"] = opt_or_null(derived_metric(dc, "cyc/byte"));
        row["fe_stall_base"] = opt_or_null(derived_metric(db, "fe-stall%"));
        row["fe_stall_cand"] = opt_or_null(derived_metric(dc, "fe-stall%"));
        row["br_miss_base"] = opt_or_null(derived_metric(db, "br-miss%"));
        row["br_miss_cand"] = opt_or_null(derived_metric(dc, "br-miss%"));
        row["l1d_miss_base"] = opt_or_null(derived_metric(db, "L1d-miss%"));
        row["l1d_miss_cand"] = opt_or_null(derived_metric(dc, "L1d-miss%"));
        row["cache_miss_base"] = opt_or_null(derived_metric(db, "cache-miss%"));
        row["cache_miss_cand"] = opt_or_null(derived_metric(dc, "cache-miss%"));
        row["pass_drift_base"] = num_or_null(b.pass_drift);
        row["pass_drift_cand"] = num_or_null(c.pass_drift);
        row["ctx_base"] = opt_or_null(derived_metric(db, "context-switches"));
        row["ctx_cand"] = opt_or_null(derived_metric(dc, "context-switches"));
        row["pf_base"] = opt_or_null(derived_metric(db, "page-faults"));
        row["pf_cand"] = opt_or_null(derived_metric(dc, "page-faults"));
        row["mig_base"] = opt_or_null(derived_metric(db, "cpu-migrations"));
        row["mig_cand"] = opt_or_null(derived_metric(dc, "cpu-migrations"));
        if (std::get<2>(k) == "serialize") serialize.push_back(std::move(row));
        else parse.push_back(std::move(row));
    }

    double score = std::numeric_limits<double>::quiet_NaN();
    if (!logs.empty()) {
        double m = std::accumulate(logs.begin(), logs.end(), 0.0) /
                   static_cast<double>(logs.size());
        score = std::exp(m);
    }

    boost::json::object blocks;
    blocks["parse"] = std::move(parse);
    blocks["serialize"] = std::move(serialize);

    boost::json::object out;
    out["base"] = run_json(*base, static_cast<int>(bs.size()));
    out["cand"] = run_json(*cand, static_cast<int>(cs.size()));
    out["comparable"] = comparable;
    out["score"] = num_or_null(score);
    out["within_1pct"] = n ? 100.0 * within / n : 0.0;
    out["regressions"] = regressions;
    out["n_rows"] = n;
    out["blocks"] = std::move(blocks);
    return out;
}

boost::json::value Store::metrics() {
    Stmt st(db_, "SELECT counters_json FROM samples LIMIT 200");
    std::map<std::string, int> names;
    names["instructions"] = 1;
    names["cycles"] = 1;
    names["task-clock-ns"] = 1;
    while (st.step() == SQLITE_ROW) {
        auto t = st.col_text(0);
        if (t.empty()) continue;
        auto v = boost::json::parse(t);
        if (!v.is_object()) continue;
        for (auto const& [k, val] : v.as_object()) {
            if (!val.is_null()) names[std::string(k)] = 1;
        }
    }
    boost::json::array ev;
    for (auto const& [k, _] : names) ev.push_back(boost::json::value(k));
    boost::json::object out;
    out["events"] = std::move(ev);
    return out;
}

}  // namespace hwc
