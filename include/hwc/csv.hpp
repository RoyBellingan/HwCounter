#pragma once

#include <boost/json.hpp>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hwc {

inline std::vector<std::string> split_csv_line(std::string const& line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_q) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else in_q = false;
            } else cur += c;
        } else if (c == '"') {
            in_q = true;
        } else if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

inline boost::json::value parse_cell(std::string const& s) {
    if (s.empty()) return nullptr;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() && *end == '\0') return v;
    return boost::json::value(s);
}

inline boost::json::array load_csv(std::string const& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string header;
    if (!std::getline(f, header)) throw std::runtime_error("empty CSV: " + path);
    if (!header.empty() && header.back() == '\r') header.pop_back();
    auto cols = split_csv_line(header);
    boost::json::array rows;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto cells = split_csv_line(line);
        boost::json::object o;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            std::string const& k = cols[i];
            if (i < cells.size()) o[k] = parse_cell(cells[i]);
            else o[k] = nullptr;
        }
        rows.push_back(std::move(o));
    }
    return rows;
}

}  // namespace hwc
