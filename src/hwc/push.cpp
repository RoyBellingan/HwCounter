#include <hwc/app.hpp>
#include <hwc/csv.hpp>
#include <hwc/http_util.hpp>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace hwc {
namespace {

boost::json::value load_json_file(std::string const& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return boost::json::parse(ss.str());
}

}  // namespace

int cmd_push(int argc, char** argv) {
  try {
    std::string url, token, prefix;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : std::string{};
        };
        if      (a == "--url")   url = next();
        else if (a == "--token") token = next();
        else if (a == "--help" || a == "-h") {
            std::cout << "hwc push --url http://host:8080 [--token TOKEN] <prefix>\n"
                         "  reads <prefix>.csv, <prefix>.machine.json, <prefix>.meta.json\n"
                         "  token: --token, else $HWC_TOKEN (preferred: not visible in ps)\n";
            return 0;
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "unknown option: " << a << "\n";
            return 2;
        } else {
            prefix = a;
        }
    }
    if (token.empty())
        if (char const* env = std::getenv("HWC_TOKEN")) token = env;
    if (url.empty() || prefix.empty()) {
        std::cerr << "usage: hwc push --url http://host:8080 --token TOKEN <prefix>\n";
        return 2;
    }

    auto rows = load_csv(prefix + ".csv");
    auto machine = load_json_file(prefix + ".machine.json");
    auto meta = load_json_file(prefix + ".meta.json");
    if (!meta.is_object()) throw std::runtime_error("meta.json must be an object");
    auto const& mo = meta.as_object();
    if (!mo.contains("json_sha") || !mo.at("json_sha").is_string() ||
        mo.at("json_sha").as_string().empty())
        throw std::runtime_error("meta.json missing json_sha");
    if (!mo.contains("cxxflags") || !mo.at("cxxflags").is_string() ||
        mo.at("cxxflags").as_string().empty())
        throw std::runtime_error("meta.json missing cxxflags");

    boost::json::object body;
    body["meta"] = meta;
    body["machine"] = machine;
    body["rows"] = std::move(rows);
    auto payload = boost::json::serialize(body);

    auto u = parse_http_url(url);
    if (u.path == "/") u.path = "/api/runs";
    else if (u.path.back() == '/') u.path += "api/runs";
    else if (u.path.find("/api/runs") == std::string::npos) u.path += "/api/runs";

    net::io_context ioc;
    tcp::resolver resolver{ioc};
    beast::tcp_stream stream{ioc};
    auto const results = resolver.resolve(u.host, u.port);
    // Without a deadline a dropped SYN or a stuck server blocks CI forever.
    stream.expires_after(std::chrono::seconds(30));
    stream.connect(results);

    http::request<http::string_body> req{http::verb::post, u.path, 11};
    req.set(http::field::host,
            u.host.find(':') != std::string::npos ? "[" + u.host + "]:" + u.port
                                                  : u.host + ":" + u.port);
    req.set(http::field::user_agent, "hwc-push");
    req.set(http::field::content_type, "application/json");
    if (!token.empty())
        req.set(http::field::authorization, "Bearer " + token);
    req.body() = std::move(payload);
    req.prepare_payload();

    stream.expires_after(std::chrono::seconds(60));
    http::write(stream, req);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    beast::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    std::cout << res.body();
    if (res.result() != http::status::created && res.result() != http::status::ok) {
        std::cerr << "push failed: HTTP " << static_cast<int>(res.result()) << "\n";
        return 1;
    }
    return 0;
  } catch (std::exception const& e) {
    std::cerr << "push: " << e.what() << "\n";
    return 1;
  }
}

}  // namespace hwc
