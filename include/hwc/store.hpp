#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace hwc {

inline constexpr char const* kUatBaselineSha =
    "e93cf9c254619142b9f3cfa9022b93fb1d4e5edb";

// Shortest SHA prefix a filter may use. Shorter text must match exactly, so
// "e" does not select every run.
inline constexpr std::size_t kMinShaPrefix = 7;

struct Query {
    std::string hostname, json_sha, compiler, cxxflags, label, metric;
    bool hide_unknown = true;
};

class Store {
public:
    explicit Store(std::string path);
    ~Store();
    Store(Store const&) = delete;
    Store& operator=(Store const&) = delete;

    struct InsertResult { std::int64_t id = 0; int n_samples = 0; };
    InsertResult insert_run(boost::json::value const& body);

    boost::json::value list_runs(Query const& q);
    boost::json::value suggest(std::string_view field, std::string_view q, int limit = 80);
    boost::json::value get_run(std::int64_t id);
    boost::json::value stability(Query const& q);
    boost::json::value compare(std::optional<std::int64_t> base,
                               std::optional<std::int64_t> cand);
    boost::json::value metrics();

private:
    sqlite3* db_ = nullptr;
    void exec(char const* sql);
    void migrate();
    // Store median ins/byte and median drift vs the first run with the same
    // (hostname, json_sha, cxxflags), so list_runs does not read samples.
    void refresh_run_summary(std::int64_t run_id);
    std::int64_t upsert_machine(boost::json::object const& machine);
};

}  // namespace hwc
