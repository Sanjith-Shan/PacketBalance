// SPDX-License-Identifier: MIT
//
// pbctl: command-line client for the packetbalance daemon. Translates
// `pbctl <noun> <verb> ...` into one JSON request (docs/API.md), prints the
// result as a table, or raw with --json. Exit 0 on success, 1 on an API
// error or connection failure, 2 on a usage error.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "client.h"
#include "format.h"
#include "packetbalance/vipspec.h"

namespace {

using nlohmann::json;

constexpr const char* kUsage = R"(usage: pbctl [--socket PATH] [--json] <command> ...

  vip add VIP [--no-conntrack]        VIP is ADDR:PORT/PROTO, e.g. 198.51.100.1:80/tcp
  vip del VIP
  vip list
  real add VIP ADDR [--weight N]
  real del VIP ADDR
  real weight VIP ADDR N
  real drain VIP ADDR                 no new flows; tracked flows continue
  real undrain VIP ADDR
  stats [--vip VIP]
  flows [--vip VIP] [--real ADDR] [--limit N]
  ring show VIP
  health
  reload                              re-read the daemon's YAML config
  config
  ping

  --socket PATH   daemon socket (default /run/packetbalance.sock, or $PBCTL_SOCKET)
  --json          print the raw JSON result
)";

[[noreturn]] void usage(const std::string& msg = "") {
    if (msg.empty()) {
        std::fputs(kUsage, stdout);
        std::exit(0);
    }
    std::fprintf(stderr, "pbctl: %s (see pbctl --help)\n", msg.c_str());
    std::exit(2);
}

// Positional arguments plus --flag [value] options, in any order.
class Args {
public:
    Args(std::vector<std::string> words, std::vector<std::string> value_flags,
         std::vector<std::string> bool_flags) {
        for (size_t i = 0; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w.rfind("--", 0) != 0) {
                positional_.push_back(w);
            } else if (contains(bool_flags, w)) {
                flags_[w] = "";
            } else if (contains(value_flags, w)) {
                if (i + 1 >= words.size()) usage(w + " needs a value");
                flags_[w] = words[++i];
            } else {
                usage("unknown option " + w);
            }
        }
    }
    const std::string& pos(size_t i, const char* what) const {
        if (i >= positional_.size()) usage(std::string("missing ") + what);
        return positional_[i];
    }
    void expect_count(size_t n) const {
        if (positional_.size() > n) usage("unexpected argument " + positional_[n]);
    }
    bool has(const std::string& f) const { return flags_.count(f) > 0; }
    const std::string& flag(const std::string& f) const { return flags_.at(f); }

private:
    static bool contains(const std::vector<std::string>& v, const std::string& s) {
        for (const auto& x : v)
            if (x == s) return true;
        return false;
    }
    std::vector<std::string> positional_;
    std::map<std::string, std::string> flags_;
};

std::string vip_arg(const std::string& s) {
    try {
        pb::VipSpec::parse(s);  // fail fast, with the daemon's own wording
    } catch (const std::exception& e) {
        usage(e.what());
    }
    return s;
}

unsigned long number(const std::string& s, const char* what) {
    try {
        size_t used = 0;
        const unsigned long n = std::stoul(s, &used);
        if (used != s.size()) throw std::invalid_argument(s);
        return n;
    } catch (const std::exception&) {
        usage(std::string("bad ") + what + ": " + s);
    }
}

// words = everything after the global options.
json build_request(const std::vector<std::string>& words) {
    if (words.empty()) usage("missing command");
    const std::string noun = words[0];
    const std::string verb = words.size() > 1 ? words[1] : "";
    const std::vector<std::string> rest(words.begin() + std::min<size_t>(2, words.size()), words.end());

    if (noun == "ping" || noun == "health" || noun == "reload" || noun == "config") {
        if (words.size() > 1) usage("unexpected argument " + words[1]);
        return {{"cmd", noun}};
    }
    if (noun == "vip") {
        if (verb == "add") {
            Args a(rest, {}, {"--no-conntrack"});
            a.expect_count(1);
            json req = {{"cmd", "vip.add"}, {"vip", vip_arg(a.pos(0, "VIP"))}};
            if (a.has("--no-conntrack")) req["no_conntrack"] = true;
            return req;
        }
        if (verb == "del") {
            Args a(rest, {}, {});
            a.expect_count(1);
            return {{"cmd", "vip.del"}, {"vip", vip_arg(a.pos(0, "VIP"))}};
        }
        if (verb == "list") {
            Args(rest, {}, {}).expect_count(0);
            return {{"cmd", "vip.list"}};
        }
        usage("vip needs add|del|list");
    }
    if (noun == "real") {
        if (verb == "add") {
            Args a(rest, {"--weight"}, {});
            a.expect_count(2);
            json req = {{"cmd", "real.add"}, {"vip", vip_arg(a.pos(0, "VIP"))}, {"addr", a.pos(1, "ADDR")}};
            if (a.has("--weight")) req["weight"] = number(a.flag("--weight"), "weight");
            return req;
        }
        if (verb == "weight") {
            Args a(rest, {}, {});
            a.expect_count(3);
            return {{"cmd", "real.weight"},
                    {"vip", vip_arg(a.pos(0, "VIP"))},
                    {"addr", a.pos(1, "ADDR")},
                    {"weight", number(a.pos(2, "WEIGHT"), "weight")}};
        }
        if (verb == "del" || verb == "drain" || verb == "undrain") {
            Args a(rest, {}, {});
            a.expect_count(2);
            return {{"cmd", "real." + verb}, {"vip", vip_arg(a.pos(0, "VIP"))}, {"addr", a.pos(1, "ADDR")}};
        }
        usage("real needs add|del|weight|drain|undrain");
    }
    if (noun == "stats") {
        Args a(std::vector<std::string>(words.begin() + 1, words.end()), {"--vip"}, {});
        a.expect_count(0);
        json req = {{"cmd", "stats"}};
        if (a.has("--vip")) req["vip"] = vip_arg(a.flag("--vip"));
        return req;
    }
    if (noun == "flows") {
        Args a(std::vector<std::string>(words.begin() + 1, words.end()), {"--vip", "--real", "--limit"}, {});
        a.expect_count(0);
        json req = {{"cmd", "flows"}};
        if (a.has("--vip")) req["vip"] = vip_arg(a.flag("--vip"));
        if (a.has("--real")) req["real"] = a.flag("--real");
        if (a.has("--limit")) req["limit"] = number(a.flag("--limit"), "limit");
        return req;
    }
    if (noun == "ring") {
        if (verb != "show") usage("ring needs show");
        Args a(rest, {}, {});
        a.expect_count(1);
        return {{"cmd", "ring.show"}, {"vip", vip_arg(a.pos(0, "VIP"))}};
    }
    usage("unknown command " + noun);
}

}  // namespace

int main(int argc, char** argv) {
    std::string socket_path = "/run/packetbalance.sock";
    if (const char* env = std::getenv("PBCTL_SOCKET")) socket_path = env;
    bool raw_json = false;

    std::vector<std::string> words;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket") {
            if (i + 1 >= argc) usage("--socket needs a value");
            socket_path = argv[++i];
        } else if (a == "--json") {
            raw_json = true;
        } else if (a == "-h" || a == "--help" || a == "help") {
            usage();
        } else {
            words.push_back(a);
        }
    }

    const json request = build_request(words);
    try {
        const json resp = pbctl::call(socket_path, request);
        if (!resp.value("ok", false)) {
            std::fprintf(stderr, "error: %s\n", resp.value("error", std::string("unknown error")).c_str());
            return 1;
        }
        const json& result = resp["result"];
        if (raw_json)
            std::cout << result.dump(2) << '\n';
        else
            pbctl::print_result(std::cout, request["cmd"].get<std::string>(), result);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
