// SPDX-License-Identifier: MIT
#include "format.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ostream>
#include <string>
#include <vector>

namespace pbctl {

using nlohmann::json;

namespace {

// Left-aligned text columns sized to their widest cell.
class Table {
public:
    explicit Table(std::vector<std::string> header) { rows_.push_back(std::move(header)); }
    void add(std::vector<std::string> row) { rows_.push_back(std::move(row)); }
    void print(std::ostream& out) const {
        std::vector<size_t> width;
        for (const auto& r : rows_)
            for (size_t i = 0; i < r.size(); ++i) {
                if (width.size() <= i) width.push_back(0);
                width[i] = std::max(width[i], r[i].size());
            }
        for (const auto& r : rows_) {
            std::string line;
            for (size_t i = 0; i < r.size(); ++i) {
                line += r[i];
                if (i + 1 < r.size()) line += std::string(width[i] - r[i].size() + 2, ' ');
            }
            out << line << '\n';
        }
    }

private:
    std::vector<std::vector<std::string>> rows_;
};

std::string str(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "yes" : "no";
    if (v.is_null()) return "-";
    return v.dump();
}

std::string percent(double part, double whole) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f%%", whole > 0 ? 100.0 * part / whole : 0.0);
    return buf;
}

std::string ago(int64_t epoch_ms) {
    using namespace std::chrono;
    const int64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1fs ago", (now - epoch_ms) / 1000.0);
    return buf;
}

void print_vips(std::ostream& out, const json& vips) {
    Table t({"VIP", "ID", "FLAGS", "GEN", "REAL", "RID", "WEIGHT", "UP", "DRAIN", "IN_RING", "MAC"});
    for (const json& v : vips) {
        const std::string flags = (v.value("flags", 0) & 1) ? "no-ct" : "-";
        if (v["reals"].empty())
            t.add({str(v["vip"]), str(v["vip_id"]), flags, str(v.value("generation", json())), "(none)"});
        for (const json& r : v["reals"])
            t.add({str(v["vip"]), str(v["vip_id"]), flags, str(v.value("generation", json())),
                   str(r["addr"]), str(r["real_id"]), str(r["weight"]), str(r["up"]), str(r["draining"]),
                   str(r.value("in_ring", json())), str(r.value("mac", json()))});
    }
    t.print(out);
}

void print_stats(std::ostream& out, const json& s) {
    const std::vector<std::string> cols = {"packets", "bytes", "ct_hit", "ct_miss", "hash", "syn", "tx"};
    Table t({"VIP", "PACKETS", "BYTES", "CT_HIT", "CT_MISS", "HASH", "SYN", "TX", "DROPS"});
    auto row = [&](const std::string& name, const json& c) {
        std::vector<std::string> r{name};
        for (const auto& k : cols) r.push_back(str(c.value(k, json(0))));
        uint64_t drops = 0;
        for (const auto& [k, v] : c.items())
            if (k.rfind("drop_", 0) == 0) drops += v.get<uint64_t>();
        r.push_back(std::to_string(drops));
        t.add(r);
    };
    for (const auto& [name, c] : s["vips"].items()) row(name, c);
    t.print(out);

    out << "\nglobal:";
    for (const auto& [k, v] : s["global"].items())
        if (v.get<uint64_t>() != 0) out << ' ' << k << '=' << v.get<uint64_t>();
    out << '\n';

    if (!s["reals"].empty()) {
        out << '\n';
        Table r({"REAL", "PACKETS", "BYTES"});
        for (const auto& [addr, c] : s["reals"].items()) r.add({addr, str(c["packets"]), str(c["bytes"])});
        r.print(out);
    }
}

void print_flows(std::ostream& out, const json& flows) {
    Table t({"SRC", "SPORT", "DST", "DPORT", "PROTO", "REAL", "CPU", "AGE_MS"});
    for (const json& f : flows)
        t.add({str(f["src"]), str(f["sport"]), str(f["dst"]), str(f["dport"]), str(f["proto"]),
               str(f["real"]), str(f["cpu"]), str(f["age_ms"])});
    t.print(out);
    out << flows.size() << " flow entr" << (flows.size() == 1 ? "y" : "ies") << '\n';
}

void print_ring(std::ostream& out, const json& r) {
    const double size = r["size"].get<double>();
    out << "hash " << str(r["hash"]) << ", " << r["size"].get<uint64_t>() << " slots, generation "
        << str(r["generation"]) << "\n\n";
    Table t({"REAL", "SLOTS", "SHARE"});
    for (const auto& [addr, n] : r["slots"].items())
        if (addr != "none" || n.get<uint64_t>() > 0) t.add({addr, str(n), percent(n.get<double>(), size)});
    t.print(out);
}

void print_health(std::ostream& out, const json& rows) {
    Table t({"VIP", "REAL", "UP", "CHECKED", "OK", "FAIL", "RTT_US", "LAST_CHANGE"});
    for (const json& h : rows)
        t.add({str(h["vip"]), str(h["addr"]), str(h["up"]), str(h.value("checked", json(true))),
               str(h["consecutive_ok"]), str(h["consecutive_fail"]), str(h["last_rtt_us"]),
               ago(h["last_change_ms"].get<int64_t>())});
    t.print(out);
}

}  // namespace

void print_result(std::ostream& out, const std::string& command, const json& result) {
    if (command == "vip.list") return print_vips(out, result);
    if (command == "stats") return print_stats(out, result);
    if (command == "flows") return print_flows(out, result);
    if (command == "ring.show") return print_ring(out, result);
    if (command == "health") return print_health(out, result);
    if (command == "config") {
        out << result.dump(2) << '\n';
        return;
    }
    if (result.is_string()) {
        out << result.get<std::string>() << '\n';
        return;
    }
    if (result.is_object() && !result.empty()) {
        for (const auto& [k, v] : result.items()) out << k << ' ' << str(v) << '\n';
        return;
    }
    out << "ok\n";
}

}  // namespace pbctl
