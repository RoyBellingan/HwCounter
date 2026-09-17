#include <hwc/app.hpp>
#include <hwc/http_util.hpp>
#include <hwc/store.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unordered_map>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace hwc {
namespace {

struct Cfg {
    std::string db = "data/hwc.sqlite";
    std::string bind = "0.0.0.0";
    unsigned short port = 8080;
    std::string token;
    std::string web = "web";
    bool allow_open_writes = false;
    std::uint64_t max_body = 8u << 20;   // bytes; a UAT push is ~100 KB
    int max_conns = 64;
};

std::string mime_of(std::string const& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".js"))   return "application/javascript; charset=utf-8";
    if (path.ends_with(".css"))  return "text/css; charset=utf-8";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".svg"))  return "image/svg+xml";
    if (path.ends_with(".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

http::response<http::string_body>
make_res(http::status st, std::string body, std::string const& type,
         unsigned ver) {
    http::response<http::string_body> res{st, ver};
    res.set(http::field::server, "hwc");
    res.set(http::field::content_type, type);
    res.set(http::field::cache_control, "no-store");
    res.keep_alive(false);
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

http::response<http::string_body>
json_res(http::status st, boost::json::value const& v, unsigned ver) {
    return make_res(st, json_dump(v) + "\n", "application/json", ver);
}

http::response<http::string_body>
err_res(http::status st, std::string const& msg, unsigned ver) {
    boost::json::object o;
    o["error"] = msg;
    return json_res(st, o, ver);
}

bool write_ok(http::request<http::string_body> const& req, Cfg const& cfg) {
    if (cfg.token.empty()) return true;
    auto it = req.find(http::field::authorization);
    if (it == req.end()) return false;
    return constant_time_equal(it->value(), "Bearer " + cfg.token);
}

Query query_from(std::unordered_map<std::string, std::string> const& q) {
    Query out;
    if (auto it = q.find("hostname"); it != q.end()) out.hostname = it->second;
    if (auto it = q.find("json_sha"); it != q.end()) out.json_sha = it->second;
    if (auto it = q.find("compiler"); it != q.end()) out.compiler = it->second;
    if (auto it = q.find("cxxflags"); it != q.end()) out.cxxflags = it->second;
    if (auto it = q.find("label"); it != q.end()) out.label = it->second;
    if (auto it = q.find("metric"); it != q.end()) out.metric = it->second;
    if (auto it = q.find("hide_unknown"); it != q.end())
        out.hide_unknown = (it->second != "0" && it->second != "false");
    return out;
}

http::response<http::string_body>
serve_static(std::string path, Cfg const& cfg, unsigned ver) {
    namespace fs = std::filesystem;
    if (path == "/" || path.empty()) path = "/index.html";
    if (path.find("..") != std::string::npos)
        return err_res(http::status::bad_request, "bad path", ver);
    fs::path root = fs::absolute(fs::path(cfg.web));
    if (!fs::exists(root))
        return err_res(http::status::not_found, "web root missing", ver);
    root = fs::weakly_canonical(root);
    fs::path full = fs::weakly_canonical(root / path.substr(1));
    if (!path_within(root, full))
        return err_res(http::status::bad_request, "bad path", ver);
    if (!fs::exists(full) || !fs::is_regular_file(full))
        return err_res(http::status::not_found, "not found", ver);
    std::ifstream f(full, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return make_res(http::status::ok, ss.str(), mime_of(full.string()), ver);
}

http::response<http::string_body>
handle(http::request<http::string_body> const& req, Store& store, Cfg const& cfg) {
    unsigned ver = req.version();
    auto [path, qs] = split_query(std::string(req.target()));
    auto q = parse_query(qs);
    auto method = req.method();

    try {
        if (method == http::verb::post && path == "/api/runs") {
            if (!write_ok(req, cfg))
                return err_res(http::status::unauthorized, "missing or bad token", ver);
            auto body = boost::json::parse(req.body());
            auto r = store.insert_run(body);
            boost::json::object o;
            o["id"] = r.id;
            o["n_samples"] = r.n_samples;
            return json_res(http::status::created, o, ver);
        }
        if (method == http::verb::get && path == "/api/runs")
            return json_res(http::status::ok, store.list_runs(query_from(q)), ver);
        if (method == http::verb::get && path == "/api/suggest") {
            std::string field = q.count("field") ? q["field"] : "";
            std::string term;
            if (auto it = q.find("q"); it != q.end()) term = it->second;
            else if (auto it = q.find("term"); it != q.end()) term = it->second;
            int limit = 80;
            if (auto it = q.find("limit"); it != q.end()) {
                try { limit = std::stoi(it->second); } catch (...) {}
            }
            return json_res(http::status::ok, store.suggest(field, term, limit), ver);
        }
        if (method == http::verb::get && path.rfind("/api/runs/", 0) == 0) {
            auto id = parse_i64(path.substr(std::string("/api/runs/").size()));
            if (!id) return err_res(http::status::bad_request, "bad run id", ver);
            return json_res(http::status::ok, store.get_run(*id), ver);
        }
        if (method == http::verb::get && path == "/api/stability")
            return json_res(http::status::ok, store.stability(query_from(q)), ver);
        if (method == http::verb::get && path == "/api/compare") {
            auto base = q.count("base") ? parse_i64(q["base"]) : std::nullopt;
            auto cand = q.count("cand") ? parse_i64(q["cand"]) : std::nullopt;
            return json_res(http::status::ok, store.compare(base, cand), ver);
        }
        if (method == http::verb::get && path == "/api/metrics")
            return json_res(http::status::ok, store.metrics(), ver);
        if (method == http::verb::get)
            return serve_static(path, cfg, ver);
        return err_res(http::status::method_not_allowed, "method not allowed", ver);
    } catch (std::exception const& e) {
        std::string msg = e.what();
        auto st = http::status::bad_request;
        if (msg == "run not found") st = http::status::not_found;
        return err_res(st, msg, ver);
    }
}

void set_sock_timeout(tcp::socket& sock, std::chrono::seconds sec) {
    timeval tv{};
    tv.tv_sec = static_cast<long>(sec.count());
    auto fd = sock.native_handle();
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void session(tcp::socket sock, Store& store, Cfg const& cfg, std::mutex& mu) {
    set_sock_timeout(sock, std::chrono::seconds(15));
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(cfg.max_body);
    try {
        beast::error_code ec;
        http::read(sock, buffer, parser, ec);
        http::response<http::string_body> res;
        if (ec == http::error::body_limit) {
            res = err_res(http::status::payload_too_large, "request body too large", 11);
        } else if (ec) {
            throw beast::system_error(ec);
        } else {
            auto const& req = parser.get();
            std::lock_guard<std::mutex> lock(mu);
            res = handle(req, store, cfg);
        }
        http::write(sock, res);
    } catch (std::exception const& e) {
        std::cerr << "session: " << e.what() << "\n";
    }
    beast::error_code ec;
    sock.shutdown(tcp::socket::shutdown_both, ec);
}

}  // namespace

int cmd_serve(int argc, char** argv) {
    Cfg cfg;
    std::string token_file;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if      (a == "--db")    cfg.db = next();
        else if (a == "--bind")  cfg.bind = next();
        else if (a == "--port")  cfg.port = static_cast<unsigned short>(std::stoi(next()));
        else if (a == "--token") cfg.token = next();
        else if (a == "--token-file") token_file = next();
        else if (a == "--web")   cfg.web = next();
        else if (a == "--allow-open-writes") cfg.allow_open_writes = true;
        else if (a == "--max-body-mb")
            cfg.max_body = std::uint64_t(std::stoul(next())) << 20;
        else if (a == "--max-conns") cfg.max_conns = std::max(1, std::stoi(next()));
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "hwc serve [--db data/hwc.sqlite] [--bind 0.0.0.0] [--port 8080] [--web web]\n"
                "          [--token-file FILE | --token TOKEN] [--allow-open-writes]\n"
                "          [--max-body-mb 8] [--max-conns 64]\n"
                "  The write token comes from --token-file, then --token, then $HWC_TOKEN.\n"
                "  Without a token the server does not start, unless --allow-open-writes.\n";
            return 0;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        }
    }
    if (!token_file.empty()) {
        std::ifstream f(token_file);
        if (!f) { std::cerr << "cannot read token file " << token_file << "\n"; return 2; }
        std::getline(f, cfg.token);
        while (!cfg.token.empty() && (cfg.token.back() == '\r' || cfg.token.back() == ' '))
            cfg.token.pop_back();
    }
    if (cfg.token.empty())
        if (char const* env = std::getenv("HWC_TOKEN")) cfg.token = env;
    if (cfg.token.empty() && !cfg.allow_open_writes) {
        std::cerr << "hwc serve: no write token. Set HWC_TOKEN, --token-file or --token,\n"
                     "           or pass --allow-open-writes to accept POSTs from anyone.\n";
        return 2;
    }

    Store store(cfg.db);
    net::io_context ioc{1};
    auto addr = net::ip::make_address(cfg.bind);
    tcp::acceptor acc{ioc, {addr, cfg.port}};
    acc.listen();
    std::cerr << "hwc serve  http://" << cfg.bind << ":" << cfg.port
              << "  db=" << cfg.db << "  web=" << cfg.web;
    if (cfg.token.empty())
        std::cerr << "  (WRITES OPEN: --allow-open-writes)\n";
    else
        std::cerr << "  (writes require token)\n";

    // Browsers open several connections at once; a blocking one-at-a-time
    // loop lets an idle socket stall the whole UI. Store is not thread-safe,
    // so handlers run under `mu`.
    //
    // This loop never returns: the detached threads hold references to
    // store, cfg and mu on this stack frame. Accept errors (for example
    // EMFILE) are logged and retried, never thrown out of this function.
    std::mutex mu;
    std::atomic<int> active{0};
    for (;;) {
        tcp::socket sock{ioc};
        beast::error_code ec;
        acc.accept(sock, ec);
        if (ec) {
            std::cerr << "accept: " << ec.message() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (active.load() >= cfg.max_conns) {
            sock.close(ec);
            continue;
        }
        ++active;
        try {
            std::thread([&store, &cfg, &mu, &active, sock = std::move(sock)]() mutable {
                session(std::move(sock), store, cfg, mu);
                --active;
            }).detach();
        } catch (std::exception const& e) {
            --active;
            std::cerr << "thread: " << e.what() << "\n";
        }
    }
}

}  // namespace hwc
