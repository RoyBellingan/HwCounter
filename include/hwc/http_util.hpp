#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace hwc {

inline std::string url_decode(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                o.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        o.push_back(s[i] == '+' ? ' ' : s[i]);
    }
    return o;
}

inline std::pair<std::string, std::string> split_query(std::string_view target) {
    auto q = target.find('?');
    if (q == std::string_view::npos) return {std::string(target), {}};
    return {std::string(target.substr(0, q)), std::string(target.substr(q + 1))};
}

inline std::unordered_map<std::string, std::string> parse_query(std::string_view qs) {
    std::unordered_map<std::string, std::string> m;
    std::size_t i = 0;
    while (i < qs.size()) {
        auto amp = qs.find('&', i);
        if (amp == std::string_view::npos) amp = qs.size();
        auto chunk = qs.substr(i, amp - i);
        auto eq = chunk.find('=');
        if (eq == std::string_view::npos)
            m[url_decode(chunk)] = "";
        else
            m[url_decode(chunk.substr(0, eq))] = url_decode(chunk.substr(eq + 1));
        i = amp + 1;
    }
    return m;
}

inline std::optional<std::int64_t> parse_i64(std::string const& s) {
    if (s.empty()) return std::nullopt;
    try { return std::stoll(s); } catch (...) { return std::nullopt; }
}

inline std::string json_dump(boost::json::value const& v) {
    return boost::json::serialize(v);
}

struct Url {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

inline Url parse_http_url(std::string s) {
    Url u;
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    else if (s.rfind("https://", 0) == 0)
        throw std::runtime_error("https is not supported in this UAT build");
    auto slash = s.find('/');
    std::string hp = slash == std::string::npos ? s : s.substr(0, slash);
    u.path = slash == std::string::npos ? "/" : s.substr(slash);
    auto colon = hp.rfind(':');
    if (colon != std::string::npos) {
        u.host = hp.substr(0, colon);
        u.port = hp.substr(colon + 1);
    } else {
        u.host = hp;
    }
    if (u.host.empty()) throw std::runtime_error("empty host in URL");
    if (u.path.empty()) u.path = "/";
    return u;
}

}  // namespace hwc
