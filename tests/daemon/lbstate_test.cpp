// SPDX-License-Identifier: MIT
//
// Control-plane tests on real BPF maps.
//
// The skeleton is loaded exactly as the daemon loads it, but never attached
// and with every pin path cleared, so the maps are private to the test and die
// with it. LbState, MapReader and CommandHandler then run against those map
// fds, and the tests read the maps back to check what the data plane would
// see: which real_ids the ring references, which `reals` slots are filled,
// what the flow walker and the counters report.
//
// Needs root (map creation). Without it every test is skipped.

#include <arpa/inet.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "commands.h"
#include "lb_state.h"
#include "map_reader.h"
#include "metrics.h"
#include "packetbalance.skel.h"
#include "ring_map.h"

namespace pb {
namespace {

constexpr const char* kSkipMsg = "daemon tests need root";

uint32_t ip(const char* s) { return parse_ipv4(s); }

const VipSpec kTcpVip = VipSpec::parse("198.51.100.1:80/tcp");
const VipSpec kTcpVip2 = VipSpec::parse("198.51.100.1:443/tcp");
const VipSpec kUdpVip = VipSpec::parse("198.51.100.1:5000/udp");
constexpr uint32_t kRise = 2;
constexpr uint32_t kFall = 3;

class ControlPlane : public ::testing::Test {
protected:
    packetbalance_bpf* skel_ = nullptr;
    MapFds fds_;
    std::unique_ptr<MapReader> reader_;
    std::unique_ptr<LbState> state_;
    std::unique_ptr<CommandHandler> commands_;

    void SetUp() override {
        if (geteuid() != 0) GTEST_SKIP() << kSkipMsg << " (not running as root)";
        skel_ = packetbalance_bpf__open();
        if (!skel_) {
            if (errno == EPERM) GTEST_SKIP() << kSkipMsg << " (open: EPERM)";
            FAIL() << "skeleton open failed: " << strerror(errno);
        }
        // Never touch a running daemon's pins.
        bpf_map* m;
        bpf_object__for_each_map(m, skel_->obj) bpf_map__set_pin_path(m, nullptr);
        ASSERT_EQ(bpf_map__set_max_entries(skel_->maps.conntrack, 1024), 0);
        int err = packetbalance_bpf__load(skel_);
        if (err == -EPERM) GTEST_SKIP() << kSkipMsg << " (load: EPERM)";
        ASSERT_EQ(err, 0) << strerror(-err);

        fds_ = MapFds{
            .vip_map = bpf_map__fd(skel_->maps.vip_map),
            .rings = bpf_map__fd(skel_->maps.rings),
            .reals = bpf_map__fd(skel_->maps.reals),
            .neigh = bpf_map__fd(skel_->maps.neigh),
            .conntrack = bpf_map__fd(skel_->maps.conntrack),
            .stats = bpf_map__fd(skel_->maps.stats),
            .real_stats = bpf_map__fd(skel_->maps.real_stats),
            .config = bpf_map__fd(skel_->maps.config),
        };
        reader_ = std::make_unique<MapReader>(fds_);
        // Every real "resolves" to one MAC, so eligibility is decided by
        // weight, drain and health alone.
        state_ = std::make_unique<LbState>(fds_, *reader_, "lo", [](uint32_t) {
            return Mac::parse("02:aa:00:00:00:01");
        });
        commands_ = std::make_unique<CommandHandler>(
            *state_, *reader_,
            CommandHandler::Hooks{.reload = [] {}, .config = [] { return nlohmann::json::object(); },
                                  .reals_changed = [] {}});
    }

    void TearDown() override {
        commands_.reset();
        state_.reset();
        reader_.reset();
        if (skel_) packetbalance_bpf__destroy(skel_);
    }

    uint32_t vip_id(const VipSpec& v) { return state_->find(v)->vip_id; }

    // Slot count per real_id in the ring currently installed for the VIP.
    std::map<uint32_t, uint32_t> ring_counts(const VipSpec& v) {
        std::map<uint32_t, uint32_t> out;
        for (uint32_t id : read_ring(fds_.rings, vip_id(v))) ++out[id];
        return out;
    }

    uint32_t reals_addr(uint32_t id) {
        pb_real r{};
        EXPECT_EQ(bpf_map_lookup_elem(fds_.reals, &id, &r), 0);
        return r.addr;
    }

    nlohmann::json call(const std::string& line) { return nlohmann::json::parse(commands_->handle_line(line)); }

    void report(const VipSpec& v, uint32_t addr, bool ok, int times) {
        for (const HealthTarget& t : state_->health_targets())
            if (t.vip == v && t.addr == addr)
                for (int i = 0; i < times; ++i)
                    state_->report_health(t, ProbeResult{ok, 100, ok ? "connected" : "refused"}, kRise, kFall);
    }
};

// ---- real ids and the ring ---------------------------------------------------

TEST_F(ControlPlane, RealZeroAddressRejected) {
    state_->add_vip(kTcpVip, false);
    EXPECT_THROW(state_->add_real(kTcpVip, 0, 1), ApiError);
    const auto resp = call(R"({"cmd":"real.add","vip":"198.51.100.1:80/tcp","addr":"0.0.0.0"})");
    EXPECT_FALSE(resp["ok"].get<bool>());
    EXPECT_NE(resp["error"].get<std::string>().find("0.0.0.0"), std::string::npos);
    EXPECT_TRUE(state_->find(kTcpVip)->reals.empty());
}

TEST_F(ControlPlane, RingReferencesOnlyLiveRealsAndIdZeroIsNeverUsed) {
    state_->add_vip(kTcpVip, false);
    std::set<uint32_t> ids;
    for (const char* a : {"10.0.0.21", "10.0.0.22", "10.0.0.23"}) {
        const uint32_t id = state_->add_real(kTcpVip, ip(a), 1);
        EXPECT_NE(id, 0u) << "real_id 0 is reserved";
        EXPECT_EQ(reals_addr(id), ip(a)) << "reals[id] written before the ring uses it";
        ids.insert(id);
    }
    auto counts = ring_counts(kTcpVip);
    EXPECT_EQ(counts.count(PB_REAL_NONE), 0u);
    for (const auto& [id, n] : counts) EXPECT_TRUE(ids.count(id)) << "ring holds unknown id " << id;

    const uint32_t gone = *state_->real_id_of(ip("10.0.0.22"));
    state_->del_real(kTcpVip, ip("10.0.0.22"));
    counts = ring_counts(kTcpVip);
    EXPECT_EQ(counts.count(gone), 0u) << "deleted real still in the ring";
    EXPECT_EQ(reals_addr(gone), 0u) << "reals slot of a deleted real must be cleared";
    EXPECT_EQ(counts.count(PB_REAL_NONE), 0u);
}

TEST_F(ControlPlane, RealSharedByTwoVipsKeepsItsSlotUntilLastUse) {
    state_->add_vip(kTcpVip, false);
    state_->add_vip(kTcpVip2, false);
    const uint32_t a = state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    const uint32_t b = state_->add_real(kTcpVip2, ip("10.0.0.21"), 1);
    EXPECT_EQ(a, b) << "one real_id per address";
    state_->del_real(kTcpVip, ip("10.0.0.21"));
    EXPECT_EQ(reals_addr(a), ip("10.0.0.21")) << "still used by the second VIP";
    state_->del_real(kTcpVip2, ip("10.0.0.21"));
    EXPECT_EQ(reals_addr(a), 0u);
}

TEST_F(ControlPlane, DeleteVipRemovesEntryAndRing) {
    state_->add_vip(kTcpVip, false);
    const uint32_t id = state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    const uint32_t vid = vip_id(kTcpVip);
    state_->del_vip(kTcpVip);
    const pb_vip_key key = kTcpVip.key();
    pb_vip_value value{};
    EXPECT_NE(bpf_map_lookup_elem(fds_.vip_map, &key, &value), 0);
    EXPECT_TRUE(read_ring(fds_.rings, vid).empty());
    EXPECT_EQ(reals_addr(id), 0u);
}

TEST_F(ControlPlane, DrainKeepsWeightButTakesNoSlots) {
    state_->add_vip(kTcpVip, false);
    state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    const uint32_t id = state_->add_real(kTcpVip, ip("10.0.0.22"), 3);
    const uint64_t gen = state_->find(kTcpVip)->generation;

    state_->set_draining(kTcpVip, ip("10.0.0.22"), true);
    EXPECT_EQ(ring_counts(kTcpVip).count(id), 0u);
    const auto view = state_->find(kTcpVip);
    EXPECT_EQ(view->generation, gen + 1);
    for (const RealView& r : view->reals)
        if (r.addr == ip("10.0.0.22")) {
            EXPECT_TRUE(r.draining);
            EXPECT_EQ(r.weight, 3u) << "drain must keep the configured weight";
            EXPECT_FALSE(r.in_ring);
        }
    EXPECT_EQ(reals_addr(id), ip("10.0.0.22")) << "tracked flows still need the address";

    state_->set_draining(kTcpVip, ip("10.0.0.22"), false);
    EXPECT_GT(ring_counts(kTcpVip)[id], 0u);
}

// ---- health -------------------------------------------------------------------

TEST_F(ControlPlane, HealthFallAndRiseSwapTheRing) {
    state_->add_vip(kTcpVip, false);
    state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    const uint32_t id = state_->add_real(kTcpVip, ip("10.0.0.22"), 1);
    EXPECT_GT(ring_counts(kTcpVip)[id], 0u) << "a new real starts UP";

    report(kTcpVip, ip("10.0.0.22"), false, kFall - 1);
    EXPECT_GT(ring_counts(kTcpVip)[id], 0u) << "fall-1 failures must not take it out";
    report(kTcpVip, ip("10.0.0.22"), false, 1);
    EXPECT_EQ(ring_counts(kTcpVip).count(id), 0u) << "fall failures take it out";

    report(kTcpVip, ip("10.0.0.22"), true, kRise - 1);
    EXPECT_EQ(ring_counts(kTcpVip).count(id), 0u);
    report(kTcpVip, ip("10.0.0.22"), true, 1);
    EXPECT_GT(ring_counts(kTcpVip)[id], 0u) << "rise successes put it back";
}

TEST_F(ControlPlane, UdpVipsAreNotHealthChecked) {
    state_->add_vip(kUdpVip, false);
    state_->add_real(kUdpVip, ip("10.0.0.21"), 1);
    EXPECT_TRUE(state_->health_targets().empty());
    EXPECT_FALSE(state_->find(kUdpVip)->reals[0].checked);
    EXPECT_TRUE(state_->find(kUdpVip)->reals[0].up);
}

TEST_F(ControlPlane, ProbeOfADeletedAndReaddedRealIsIgnored) {
    state_->add_vip(kTcpVip, false);
    state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    const HealthTarget stale = state_->health_targets().at(0);
    state_->del_real(kTcpVip, ip("10.0.0.21"));
    const uint32_t id = state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    for (uint32_t i = 0; i < kFall; ++i)
        state_->report_health(stale, ProbeResult{false, 0, "refused"}, kRise, kFall);
    EXPECT_GT(ring_counts(kTcpVip)[id], 0u);
}

// ---- map reader ---------------------------------------------------------------

TEST_F(ControlPlane, FlowWalkerSkipsOtherCpusZeroValues) {
    const int ncpu = reader_->num_cpus();
    pb_ct_key key{};
    key.saddr = ip("10.0.0.10");
    key.daddr = kTcpVip.addr_be;
    key.sport = htons(40000);
    key.dport = kTcpVip.port_be;
    key.proto = IPPROTO_TCP;
    // One CPU saw the flow; every other CPU holds the kernel's zero fill.
    std::vector<pb_ct_value> values(static_cast<size_t>(ncpu));
    values[0] = pb_ct_value{.real_id = 7, .vip_id = 0, .last_seen_ns = 12345};
    ASSERT_EQ(bpf_map_update_elem(fds_.conntrack, &key, values.data(), BPF_ANY), 0);
    // A key whose every value is zero: present in the map, seen by no CPU.
    pb_ct_key key2 = key;
    key2.sport = htons(40001);
    std::vector<pb_ct_value> zeros(static_cast<size_t>(ncpu));
    ASSERT_EQ(bpf_map_update_elem(fds_.conntrack, &key2, zeros.data(), BPF_ANY), 0);

    const auto rows = reader_->flows(nullptr, 100);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].cpu, 0u);
    EXPECT_EQ(rows[0].value.real_id, 7u);
    EXPECT_EQ(reader_->conntrack_entries(), 2u) << "entries count keys, not per-CPU values";
}

TEST_F(ControlPlane, CountersAreSummedOverCpus) {
    state_->add_vip(kTcpVip, false);
    const uint32_t vid = vip_id(kTcpVip);
    const int ncpu = reader_->num_cpus();
    std::vector<pb_stats> v(static_cast<size_t>(ncpu));
    for (auto& s : v) {
        s.c[PB_CNT_PACKETS] = 2;
        s.c[PB_CNT_TX] = 1;
    }
    ASSERT_EQ(bpf_map_update_elem(fds_.stats, &vid, v.data(), BPF_ANY), 0);
    const Counters c = reader_->counters(vid);
    EXPECT_EQ(c[PB_CNT_PACKETS], 2u * static_cast<uint64_t>(ncpu));
    EXPECT_EQ(c[PB_CNT_TX], static_cast<uint64_t>(ncpu));

    const auto resp = call(R"({"cmd":"stats","vip":"198.51.100.1:80/tcp"})");
    ASSERT_TRUE(resp["ok"].get<bool>());
    EXPECT_EQ(resp["result"]["vips"]["198.51.100.1:80/tcp"]["packets"].get<uint64_t>(),
              2u * static_cast<uint64_t>(ncpu));
}

// ---- JSON API -----------------------------------------------------------------

TEST_F(ControlPlane, ApiRejectsNonBooleanNoConntrack) {
    const auto resp = call(R"({"cmd":"vip.add","vip":"198.51.100.1:80/tcp","no_conntrack":"yes"})");
    EXPECT_FALSE(resp["ok"].get<bool>());
    EXPECT_EQ(resp["error"], "argument 'no_conntrack' must be a boolean");
    EXPECT_FALSE(state_->find(kTcpVip).has_value());
}

TEST_F(ControlPlane, ApiResultShapesMatchDocs) {
    auto r = call(R"({"cmd":"vip.add","vip":"198.51.100.1:80/tcp","no_conntrack":true})");
    ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
    EXPECT_TRUE(r["result"].contains("vip_id"));
    r = call(R"({"cmd":"real.add","vip":"198.51.100.1:80/tcp","addr":"10.0.0.21","weight":2})");
    ASSERT_TRUE(r["ok"].get<bool>()) << r.dump();
    EXPECT_TRUE(r["result"].contains("real_id"));

    r = call(R"({"cmd":"vip.list"})");
    const auto& vip = r["result"].at(0);
    for (const char* k : {"vip", "vip_id", "flags", "generation", "reals"}) EXPECT_TRUE(vip.contains(k)) << k;
    EXPECT_EQ(vip["flags"].get<uint32_t>(), PB_VIP_F_NO_CONNTRACK);
    for (const char* k : {"addr", "real_id", "weight", "up", "draining", "in_ring", "mac"})
        EXPECT_TRUE(vip["reals"].at(0).contains(k)) << k;

    r = call(R"({"cmd":"ring.show","vip":"198.51.100.1:80/tcp"})");
    EXPECT_EQ(r["result"]["size"].get<uint32_t>(), PB_RING_SIZE);
    EXPECT_EQ(r["result"]["slots"]["10.0.0.21"].get<uint32_t>(), PB_RING_SIZE);
    EXPECT_EQ(r["result"]["slots"]["none"].get<uint32_t>(), 0u);

    r = call(R"({"cmd":"health"})");
    for (const char* k : {"vip", "addr", "up", "checked", "consecutive_ok", "consecutive_fail",
                          "last_change_ms", "last_rtt_us"})
        EXPECT_TRUE(r["result"].at(0).contains(k)) << k;

    r = call(R"({"cmd":"real.del","vip":"198.51.100.1:80/tcp","addr":"10.0.0.99"})");
    EXPECT_EQ(r["error"], "unknown real 10.0.0.99 on vip 198.51.100.1:80/tcp");
    r = call(R"({"cmd":"vip.del","vip":"198.51.100.2:80/tcp"})");
    EXPECT_EQ(r["error"], "unknown vip 198.51.100.2:80/tcp");
}

// ---- metrics ------------------------------------------------------------------

// Prometheus text format 0.0.4: each family has one HELP and one TYPE line, and
// every sample follows the TYPE line of its own family.
TEST_F(ControlPlane, MetricsExpositionIsWellFormed) {
    state_->add_vip(kTcpVip, false);
    state_->add_real(kTcpVip, ip("10.0.0.21"), 1);
    state_->add_vip(kUdpVip, false);
    state_->add_real(kUdpVip, ip("10.0.0.21"), 1);
    const std::string text = render_metrics(*state_, *reader_, XdpMode::Generic);

    std::set<std::string> typed, helped;
    std::string current;
    std::istringstream in(text);
    std::string line;
    int samples = 0;
    while (std::getline(in, line)) {
        ASSERT_FALSE(line.empty());
        if (line.rfind("# HELP ", 0) == 0) {
            const std::string name = line.substr(7, line.find(' ', 7) - 7);
            EXPECT_TRUE(helped.insert(name).second) << "duplicate HELP " << name;
            continue;
        }
        if (line.rfind("# TYPE ", 0) == 0) {
            const std::string name = line.substr(7, line.find(' ', 7) - 7);
            EXPECT_TRUE(typed.insert(name).second) << "duplicate TYPE " << name;
            current = name;
            continue;
        }
        const std::string name = line.substr(0, line.find_first_of("{ "));
        EXPECT_EQ(name, current) << "sample outside its family: " << line;
        const std::string value = line.substr(line.rfind(' ') + 1);
        EXPECT_EQ(value.find_first_not_of("0123456789"), std::string::npos) << line;
        ++samples;
    }
    EXPECT_EQ(typed, helped);
    EXPECT_GT(samples, 10);
    EXPECT_NE(text.find("pb_drops_total{vip=\"global\",reason=\"frag\"} 0"), std::string::npos);
    EXPECT_NE(text.find("pb_real_up{vip=\"198.51.100.1:80/tcp\",real=\"10.0.0.21\"} 1"), std::string::npos);
    // One pb_real_packets_total per real, even when the real serves two VIPs.
    size_t n = 0;
    for (size_t p = 0; (p = text.find("pb_real_packets_total{", p)) != std::string::npos; ++p) ++n;
    EXPECT_EQ(n, 1u);
}

}  // namespace
}  // namespace pb
