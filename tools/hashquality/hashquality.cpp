// SPDX-License-Identifier: MIT
//
// hashquality: Experiment 5, "Hash quality".
//
// One million synthetic 5-tuples toward one VIP are hashed with pb_flow_hash
// (the data plane's hash, bit for bit) and looked up in rings built for four
// reals with weights 1:1:2:4, once with Maglev and once with modulo. Reported:
//
//   share       each real's fraction of the flows against weight / sum(weights)
//   disruption  the fraction of flows that change real when real3 is removed and
//               when a fifth real is added, beside the fraction of ring slots
//               that change, against the minimum any scheme must move
//
// Usage: hashquality [--flows N] [--seed S] [--json PATH] [--md PATH]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "packetbalance/hash.h"
#include "packetbalance/maglev.h"
#include "packetbalance/vipspec.h"

namespace {

using pb::Backend;
using pb::HashMode;

// SplitMix64 (Steele, Lea, Flood 2014). Tiny, seedable, and identical on every
// platform, so the same seed always produces the same million flows.
struct SplitMix64 {
    uint64_t state;
    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
};

// Random client address and ephemeral port, fixed VIP 198.51.100.1:80/tcp.
std::vector<uint32_t> make_flow_hashes(size_t n, uint64_t seed, const pb::VipSpec& vip) {
    SplitMix64 rng{seed};
    std::vector<uint32_t> hashes(n);
    for (auto& h : hashes) {
        const uint64_t r = rng.next();
        const uint32_t saddr = static_cast<uint32_t>(r);
        const uint16_t sport = pb::port_to_be(static_cast<uint16_t>(1024 + (r >> 32) % (65536 - 1024)));
        h = pb_flow_hash(saddr, vip.addr_be, sport, vip.port_be, vip.proto);
    }
    return hashes;
}

std::vector<uint32_t> lookup(const std::vector<uint32_t>& ring, const std::vector<uint32_t>& hashes) {
    std::vector<uint32_t> real(hashes.size());
    for (size_t i = 0; i < hashes.size(); ++i)
        real[i] = ring[pb::ring_index(hashes[i], static_cast<uint32_t>(ring.size()))];
    return real;
}

double flow_disruption(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t moved = 0;
    for (size_t i = 0; i < a.size(); ++i) moved += a[i] != b[i];
    return static_cast<double>(moved) / static_cast<double>(a.size());
}

struct ShareRow {
    std::string real;
    uint32_t weight;
    double expected, observed;
};

struct DisruptionRow {
    std::string event;
    double target;  // minimum fraction any scheme must move for this event
    double flows, slots;
};

struct ModeResult {
    HashMode mode;
    std::vector<ShareRow> shares;
    std::vector<DisruptionRow> disruption;
    double build_ms;
};

ModeResult run_mode(HashMode mode, const std::vector<Backend>& reals, const Backend& fifth,
                    size_t removed_index, const std::vector<uint32_t>& hashes) {
    ModeResult res{mode, {}, {}, 0};

    const auto t0 = std::chrono::steady_clock::now();
    const auto ring = pb::build_ring(reals, mode);
    res.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto base = lookup(ring, hashes);

    double total_w = 0;
    for (const Backend& b : reals) total_w += b.weight;
    for (const Backend& b : reals) {
        size_t n = 0;
        for (uint32_t r : base) n += r == b.real_id;
        res.shares.push_back({pb::ipv4_to_string(b.addr), b.weight, b.weight / total_w,
                              static_cast<double>(n) / static_cast<double>(hashes.size())});
    }

    auto removed = reals;
    removed.erase(removed.begin() + static_cast<long>(removed_index));
    const auto ring_rm = pb::build_ring(removed, mode);
    res.disruption.push_back({"remove " + pb::ipv4_to_string(reals[removed_index].addr) +
                                  " (w=" + std::to_string(reals[removed_index].weight) + ")",
                              reals[removed_index].weight / total_w,
                              flow_disruption(base, lookup(ring_rm, hashes)),
                              pb::ring_disruption(ring, ring_rm)});

    auto added = reals;
    added.push_back(fifth);
    const auto ring_add = pb::build_ring(added, mode);
    res.disruption.push_back({"add " + pb::ipv4_to_string(fifth.addr) + " (w=" +
                                  std::to_string(fifth.weight) + ")",
                              fifth.weight / (total_w + fifth.weight),
                              flow_disruption(base, lookup(ring_add, hashes)),
                              pb::ring_disruption(ring, ring_add)});
    return res;
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::gmtime(&t));
    return buf;
}

std::string fmt(double v, int prec = 4) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

std::string pct(double v) { return fmt(100.0 * v, 2) + "%"; }

std::string to_json(const std::vector<ModeResult>& results, const std::string& date, size_t flows,
                    uint64_t seed, const std::string& vip) {
    std::ostringstream o;
    o << "{\n  \"experiment\": \"exp5_hash_quality\",\n"
      << "  \"date\": \"" << date << "\",\n"
      << "  \"ring_size\": " << PB_RING_SIZE << ",\n"
      << "  \"tuples\": " << flows << ",\n"
      << "  \"seed\": " << seed << ",\n"
      << "  \"vip\": \"" << vip << "\",\n"
      << "  \"hash\": \"pb_flow_hash (jhash 3 words, include/packetbalance/hash.h)\",\n"
      << "  \"modes\": [\n";
    for (size_t m = 0; m < results.size(); ++m) {
        const ModeResult& r = results[m];
        o << "    {\n      \"mode\": \"" << pb::hash_mode_name(r.mode) << "\",\n"
          << "      \"build_ms\": " << fmt(r.build_ms, 3) << ",\n      \"shares\": [\n";
        for (size_t i = 0; i < r.shares.size(); ++i) {
            const ShareRow& s = r.shares[i];
            o << "        {\"real\": \"" << s.real << "\", \"weight\": " << s.weight
              << ", \"expected\": " << fmt(s.expected, 6) << ", \"observed\": " << fmt(s.observed, 6)
              << ", \"deviation\": " << fmt(s.observed - s.expected, 6) << "}"
              << (i + 1 < r.shares.size() ? "," : "") << "\n";
        }
        o << "      ],\n      \"disruption\": [\n";
        for (size_t i = 0; i < r.disruption.size(); ++i) {
            const DisruptionRow& d = r.disruption[i];
            o << "        {\"event\": \"" << d.event << "\", \"target\": " << fmt(d.target, 6)
              << ", \"flows_moved\": " << fmt(d.flows, 6) << ", \"slots_moved\": " << fmt(d.slots, 6)
              << "}" << (i + 1 < r.disruption.size() ? "," : "") << "\n";
        }
        o << "      ]\n    }" << (m + 1 < results.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

std::string to_markdown(const std::vector<ModeResult>& results, const std::string& date,
                        size_t flows, uint64_t seed, const std::string& vip) {
    std::ostringstream o;
    o << "### Experiment 5: hash quality\n\n"
      << flows << " synthetic flows to " << vip << ", hashed with the data plane's `pb_flow_hash`, "
      << "ring size " << PB_RING_SIZE << ", seed " << seed << ", " << date
      << ". Generated by `tools/hashquality`.\n\n"
      << "**Share of flows per real (weights 1:1:2:4)**\n\n"
      << "| Real | Weight | Expected |";
    for (const ModeResult& r : results) o << " " << pb::hash_mode_name(r.mode) << " observed | deviation |";
    o << "\n|---|---:|---:|";
    for (size_t m = 0; m < results.size(); ++m) o << "---:|---:|";
    o << "\n";
    for (size_t i = 0; i < results[0].shares.size(); ++i) {
        const ShareRow& s0 = results[0].shares[i];
        o << "| " << s0.real << " | " << s0.weight << " | " << pct(s0.expected) << " |";
        for (const ModeResult& r : results) {
            const ShareRow& s = r.shares[i];
            const std::string dev = fmt(100.0 * (s.observed - s.expected), 2);
            // Print "+0.00", not "-0.00", for a deviation that rounds to zero.
            o << " " << pct(s.observed) << " | "
              << (dev == "-0.00" ? "+0.00" : dev[0] == '-' ? dev : "+" + dev) << " pp |";
        }
        o << "\n";
    }
    o << "\n**Disruption: fraction that changes real**\n\n"
      << "| Event | Minimum possible | Mode | Flows moved | Slots moved |\n"
      << "|---|---:|---|---:|---:|\n";
    for (size_t e = 0; e < results[0].disruption.size(); ++e)
        for (const ModeResult& r : results) {
            const DisruptionRow& d = r.disruption[e];
            o << "| " << d.event << " | " << pct(d.target) << " | " << pb::hash_mode_name(r.mode)
              << " | " << pct(d.flows) << " | " << pct(d.slots) << " |\n";
        }
    return o.str();
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    if (!f || !(f << body)) {
        std::cerr << "hashquality: cannot write " << path << "\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    size_t flows = 1000000;
    uint64_t seed = 5;
    std::string json_path, md_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "hashquality: " << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--flows") flows = std::stoull(value());
        else if (a == "--seed") seed = std::stoull(value());
        else if (a == "--json") json_path = value();
        else if (a == "--md") md_path = value();
        else {
            std::cerr << "usage: hashquality [--flows N] [--seed S] [--json PATH] [--md PATH]\n";
            return 2;
        }
    }
    if (flows == 0) {
        std::cerr << "hashquality: --flows must be > 0\n";
        return 2;
    }

    const pb::VipSpec vip = pb::VipSpec::parse("198.51.100.1:80/tcp");
    const std::vector<Backend> reals = {
        {1, pb::parse_ipv4("10.0.0.21"), 1},
        {2, pb::parse_ipv4("10.0.0.22"), 1},
        {3, pb::parse_ipv4("10.0.0.23"), 2},
        {4, pb::parse_ipv4("10.0.0.24"), 4},
    };
    const Backend fifth{5, pb::parse_ipv4("10.0.0.25"), 1};
    const size_t removed_index = 2;  // real3, 10.0.0.23, weight 2

    const auto hashes = make_flow_hashes(flows, seed, vip);
    std::vector<ModeResult> results;
    for (HashMode mode : {HashMode::Maglev, HashMode::Modulo})
        results.push_back(run_mode(mode, reals, fifth, removed_index, hashes));

    const std::string date = today();
    const std::string md = to_markdown(results, date, flows, seed, vip.str());
    std::cout << md;
    if (!json_path.empty()) write_file(json_path, to_json(results, date, flows, seed, vip.str()));
    if (!md_path.empty()) write_file(md_path, md);
    return 0;
}
