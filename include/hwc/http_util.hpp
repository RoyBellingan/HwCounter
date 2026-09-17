#pragma once

#include <boost/json.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
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
    std::string port;
    if (!hp.empty() && hp.front() == '[') {
        // IPv6 literal: [addr] or [addr]:port
        auto close = hp.find(']');
        if (close == std::string::npos)
            throw std::runtime_error("unterminated [ in URL host");
        u.host = hp.substr(1, close - 1);
        auto rest = hp.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':')
                throw std::runtime_error("bad text after ] in URL host");
            port = rest.substr(1);
        }
    } else {
        auto colon = hp.find(':');
        if (colon != std::string::npos && hp.find(':', colon + 1) != std::string::npos)
            throw std::runtime_error("IPv6 host must be in brackets: http://[::1]:8080");
        if (colon != std::string::npos) {
            u.host = hp.substr(0, colon);
            port = hp.substr(colon + 1);
        } else {
            u.host = hp;
        }
    }
    if (!port.empty()) {
        if (port.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("bad port in URL: " + port);
        u.port = port;
    }
    if (u.host.empty()) throw std::runtime_error("empty host in URL");
    if (u.path.empty()) u.path = "/";
    return u;
}

// Compare without an early exit, so the time does not show how many leading
// bytes of a guessed token were right.
inline bool constant_time_equal(std::string_view a, std::string_view b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    std::size_t const n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(x ^ y);
    }
    return diff == 0;
}

// True if `full` is `root` or is below it. Compares path elements, so
// /tmp/website is NOT inside /tmp/web. Both paths must be canonical.
inline bool path_within(std::filesystem::path const& root,
                        std::filesystem::path const& full) {
    auto r = root.begin(), f = full.begin();
    for (; r != root.end(); ++r, ++f) {
        if (r->empty() && std::next(r) == root.end()) break;  // trailing '/'
        if (f == full.end() || *r != *f) return false;
    }
    return true;
}

}  // namespace hwc
