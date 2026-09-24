// SPDX-License-Identifier: MIT
//
// Data-plane tests for the XDP program, run through BPF_PROG_TEST_RUN.
//
// The program is loaded from the libbpf skeleton exactly as the daemon loads
// it, but never attached to an interface. Maps are populated directly, a
// crafted frame is handed to bpf_prog_test_run_opts(), and the test checks
// the verdict, the output frame byte by byte, the counters and the connection
// table. This is the same approach Katran uses to test its data plane.
//
// Needs root (CAP_BPF + CAP_SYS_ADMIN for test runs). Without it every test
// is skipped, not failed.

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <sched.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "packetbalance.skel.h"
#include "packetbalance/abi.h"
#include "packetbalance/hash.h"

namespace {

constexpr int kXdpDrop = 1;
constexpr int kXdpPass = 2;
constexpr int kXdpTx = 3;

constexpr uint8_t kTcpFin = 0x01;
constexpr uint8_t kTcpSyn = 0x02;
constexpr uint8_t kTcpRst = 0x04;
constexpr uint8_t kTcpAck = 0x10;

constexpr const char* kSkipMsg = "dataplane tests need root";

uint32_t ip(const char* s)
{
    in_addr a{};
    inet_pton(AF_INET, s, &a);
    return a.s_addr;  // network order
}

const uint8_t kLbMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
const uint8_t kClientMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x10};

void real_mac(uint32_t real_id, uint8_t out[6])
{
    const uint8_t m[6] = {0x02, 0xaa, 0x00, 0x00, (uint8_t)(real_id >> 8), (uint8_t)real_id};
    memcpy(out, m, 6);
}

// ---- packet building -----------------------------------------------------

void put16(std::vector<uint8_t>& b, size_t off, uint16_t host)
{
    b[off] = host >> 8;
    b[off + 1] = host & 0xff;
}
uint16_t get16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Internet checksum, returned in host order (store with put16).
uint16_t inet_csum(const uint8_t* p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2)
        s += get16(p + i);
    if (n & 1)
        s += p[n - 1] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

std::vector<uint8_t> tcp_hdr(uint16_t sport, uint16_t dport, uint8_t flags)
{
    std::vector<uint8_t> t(20, 0);
    put16(t, 0, sport);
    put16(t, 2, dport);
    t[4] = 0x11;  // seq, arbitrary
    t[12] = 5 << 4;
    t[13] = flags;
    put16(t, 14, 65535);
    return t;
}

std::vector<uint8_t> udp_hdr(uint16_t sport, uint16_t dport, size_t payload)
{
    std::vector<uint8_t> u(8 + payload, 0xab);
    put16(u, 0, sport);
    put16(u, 2, dport);
    put16(u, 4, (uint16_t)(8 + payload));
    put16(u, 6, 0);
    return u;
}

struct IpOpts {
    uint16_t frag = 0;       // host order flags+offset field
    int option_words = 0;    // 32-bit option words (ihl = 5 + this)
    uint8_t tos = 0;
    int claim_extra = 0;     // add to tot_len without adding bytes (truncation)
};

std::vector<uint8_t> ipv4_frame(uint32_t saddr, uint32_t daddr, uint8_t proto,
                                const std::vector<uint8_t>& l4, IpOpts o = {})
{
    size_t ihl = 20 + 4 * o.option_words;
    std::vector<uint8_t> f(14 + ihl, 0);
    memcpy(&f[0], kLbMac, 6);
    memcpy(&f[6], kClientMac, 6);
    put16(f, 12, ETH_P_IP);
    uint8_t* h = &f[14];
    h[0] = 0x40 | (uint8_t)(ihl / 4);
    h[1] = o.tos;
    size_t tot = ihl + l4.size() + o.claim_extra;
    put16(f, 14 + 2, (uint16_t)tot);
    put16(f, 14 + 4, 0x1234);
    put16(f, 14 + 6, o.frag);
    h[8] = 63;
    h[9] = proto;
    memcpy(h + 12, &saddr, 4);
    memcpy(h + 16, &daddr, 4);
    for (int i = 0; i < o.option_words * 4; i++)
        h[20 + i] = 0x01;  // NOP options
    put16(f, 14 + 10, inet_csum(h, ihl));
    f.insert(f.end(), l4.begin(), l4.end());
    return f;
}

std::vector<uint8_t> tcp_frame(uint32_t s, uint16_t sp, uint32_t d, uint16_t dp,
                               uint8_t flags, IpOpts o = {})
{
    return ipv4_frame(s, d, IPPROTO_TCP, tcp_hdr(sp, dp, flags), o);
}

std::vector<uint8_t> udp_frame(uint32_t s, uint16_t sp, uint32_t d, uint16_t dp,
                               size_t payload = 18)
{
    return ipv4_frame(s, d, IPPROTO_UDP, udp_hdr(sp, dp, payload));
}

// ---- constants of the fixture topology ------------------------------------

const uint32_t kVip = ip("198.51.100.1");
const uint32_t kClient = ip("10.0.0.10");
const uint32_t kEncapPrefix = ip("10.99.0.0");
const uint32_t kEncapMask = ip("255.255.255.0");
constexpr uint32_t kTcpVipId = 3;
constexpr uint32_t kUdpVipId = 7;
constexpr uint32_t kRealA = 1;   // 10.0.0.21
constexpr uint32_t kRealB = 2;   // 10.0.0.22

uint32_t real_addr(uint32_t real_id)
{
    // 10.0.<id/256>.<20+id%256>, unique per id, never 0.
    uint8_t b[4] = {10, 0, (uint8_t)(real_id >> 8), (uint8_t)(20 + (real_id & 0xff))};
    uint32_t a;
    memcpy(&a, b, 4);
    return a;
}

struct RunResult {
    int ret = -1;
    std::vector<uint8_t> out;
};

class Dataplane : public ::testing::Test {
protected:
    packetbalance_bpf* skel_ = nullptr;
    int ncpu_ = 0;
    cpu_set_t saved_affinity_{};
    bool pinned_ = false;

    void SetUp() override
    {
        if (geteuid() != 0)
            GTEST_SKIP() << kSkipMsg << " (not running as root)";
        ncpu_ = libbpf_num_possible_cpus();
        ASSERT_GT(ncpu_, 0);

        // BPF_PROG_TEST_RUN runs the program on the calling CPU, and the
        // connection table is per-CPU: an entry written while this thread ran
        // on CPU 2 is invisible (a zero-filled value) when the next packet is
        // run on CPU 4. Pin the test to one CPU so "same flow, next packet"
        // means what it means on a NIC RX queue. Without this the conntrack
        // tests flaked whenever the scheduler migrated the thread.
        ASSERT_EQ(sched_getaffinity(0, sizeof(saved_affinity_), &saved_affinity_), 0);
        for (int c = 0; c < CPU_SETSIZE; c++) {
            if (CPU_ISSET(c, &saved_affinity_)) {
                cpu_set_t one;
                CPU_ZERO(&one);
                CPU_SET(c, &one);
                ASSERT_EQ(sched_setaffinity(0, sizeof(one), &one), 0);
                pinned_ = true;
                break;
            }
        }

        skel_ = packetbalance_bpf__open();
        if (!skel_) {
            if (errno == EPERM)
                GTEST_SKIP() << kSkipMsg << " (open: EPERM)";
            FAIL() << "skeleton open failed: " << strerror(errno);
        }
        // The maps are declared LIBBPF_PIN_BY_NAME for the daemon. A test must
        // never reuse or leave behind pins in /sys/fs/bpf/packetbalance, which a
        // running daemon may own, so drop the pin path before load: libbpf then
        // creates fresh, unpinned maps that die with the test.
        bpf_map* m;
        bpf_object__for_each_map(m, skel_->obj) bpf_map__set_pin_path(m, nullptr);
        // 1M entries x per-CPU values is ~100 MB of preallocated memory; the
        // tests need a handful.
        ASSERT_EQ(bpf_map__set_max_entries(skel_->maps.conntrack, 4096), 0);

        int err = packetbalance_bpf__load(skel_);
        if (err == -EPERM)
            GTEST_SKIP() << kSkipMsg << " (load: EPERM)";
        ASSERT_EQ(err, 0) << "load failed (verifier?): " << strerror(-err);

        set_config(0);
        add_vip(kVip, 80, IPPROTO_TCP, kTcpVipId, 0);
        add_vip(kVip, 5000, IPPROTO_UDP, kUdpVipId, 0);
        add_real(kRealA);
        add_real(kRealB);
    }

    void TearDown() override
    {
        if (skel_)
            packetbalance_bpf__destroy(skel_);
        if (pinned_)
            sched_setaffinity(0, sizeof(saved_affinity_), &saved_affinity_);
    }

    int fd(bpf_map* m) { return bpf_map__fd(m); }

    void set_config(uint32_t flags)
    {
        pb_config c{};
        memcpy(c.lb_mac, kLbMac, 6);
        c.encap_src_prefix = kEncapPrefix;
        c.encap_src_mask = kEncapMask;
        c.flags = flags;
        uint32_t k = 0;
        ASSERT_EQ(bpf_map_update_elem(fd(skel_->maps.config), &k, &c, BPF_ANY), 0);
    }

    void add_vip(uint32_t addr, uint16_t port_host, uint8_t proto, uint32_t id, uint32_t flags)
    {
        pb_vip_key k{};
        k.addr = addr;
        k.port = htons(port_host);
        k.proto = proto;
        pb_vip_value v{id, flags};
        ASSERT_EQ(bpf_map_update_elem(fd(skel_->maps.vip_map), &k, &v, BPF_ANY), 0);
    }

    void add_real(uint32_t id)
    {
        pb_real r{real_addr(id), 0};
        pb_mac m{};
        real_mac(id, m.mac);
        ASSERT_EQ(bpf_map_update_elem(fd(skel_->maps.reals), &id, &r, BPF_ANY), 0);
        ASSERT_EQ(bpf_map_update_elem(fd(skel_->maps.neigh), &id, &m, BPF_ANY), 0);
    }

    // Build a complete inner ring and swap it into rings[vip_id], the way
    // the control plane does.
    template <typename F>
    void set_ring(uint32_t vip_id, F slot_to_real)
    {
        int inner = bpf_map_create(BPF_MAP_TYPE_ARRAY, "pbtest_ring", sizeof(uint32_t),
                                   sizeof(uint32_t), PB_RING_SIZE, nullptr);
        ASSERT_GE(inner, 0) << strerror(errno);
        std::vector<uint32_t> keys(PB_RING_SIZE), vals(PB_RING_SIZE);
        for (uint32_t i = 0; i < PB_RING_SIZE; i++) {
            keys[i] = i;
            vals[i] = slot_to_real(i);
        }
        uint32_t count = PB_RING_SIZE;
        if (bpf_map_update_batch(inner, keys.data(), vals.data(), &count, nullptr) != 0) {
            for (uint32_t i = 0; i < PB_RING_SIZE; i++)
                ASSERT_EQ(bpf_map_update_elem(inner, &keys[i], &vals[i], BPF_ANY), 0);
        }
        int err = bpf_map_update_elem(fd(skel_->maps.rings), &vip_id, &inner, BPF_ANY);
        close(inner);  // the outer map holds its own reference
        ASSERT_EQ(err, 0) << strerror(errno);
    }

    void set_ring_all(uint32_t vip_id, uint32_t real_id)
    {
        set_ring(vip_id, [real_id](uint32_t) { return real_id; });
    }

    RunResult run(const std::vector<uint8_t>& in)
    {
        RunResult r;
        r.out.assign(in.size() + 512, 0);
        bpf_test_run_opts o{};
        o.sz = sizeof(o);
        o.data_in = in.data();
        o.data_size_in = (uint32_t)in.size();
        o.data_out = r.out.data();
        o.data_size_out = (uint32_t)r.out.size();
        o.repeat = 1;
        int err = bpf_prog_test_run_opts(bpf_program__fd(skel_->progs.xdp_packetbalance), &o);
        EXPECT_EQ(err, 0) << "test_run: " << strerror(errno);
        r.ret = (int)o.retval;
        r.out.resize(o.data_size_out);
        return r;
    }

    uint64_t counter(uint32_t slot, pb_counter c)
    {
        std::vector<pb_stats> v(ncpu_);
        EXPECT_EQ(bpf_map_lookup_elem(fd(skel_->maps.stats), &slot, v.data()), 0);
        uint64_t s = 0;
        for (auto& x : v)
            s += x.c[c];
        return s;
    }

    pb_real_stats real_counter(uint32_t real_id)
    {
        std::vector<pb_real_stats> v(ncpu_);
        EXPECT_EQ(bpf_map_lookup_elem(fd(skel_->maps.real_stats), &real_id, v.data()), 0);
        pb_real_stats s{};
        for (auto& x : v) {
            s.packets += x.packets;
            s.bytes += x.bytes;
        }
        return s;
    }

    // Returns the real_id stored for the flow on any CPU that wrote it, or
    // PB_REAL_NONE when the key is absent or only zero-filled values exist.
    uint32_t ct_real(uint32_t s, uint16_t sp, uint32_t d, uint16_t dp, uint8_t proto)
    {
        pb_ct_key k{};
        k.saddr = s;
        k.daddr = d;
        k.sport = htons(sp);
        k.dport = htons(dp);
        k.proto = proto;
        std::vector<pb_ct_value> v(ncpu_);
        if (bpf_map_lookup_elem(fd(skel_->maps.conntrack), &k, v.data()) != 0)
            return PB_REAL_NONE;
        for (auto& x : v)
            if (x.last_seen_ns != 0)
                return x.real_id;
        return PB_REAL_NONE;
    }

    // Asserts that `out` is `in` encapsulated toward real_id and returns the
    // outer source address.
    void expect_encap(const std::vector<uint8_t>& in, const RunResult& r, uint32_t real_id)
    {
        ASSERT_EQ(r.ret, kXdpTx);
        ASSERT_EQ(r.out.size(), in.size() + 20);
        const uint8_t* o = r.out.data();
        uint8_t dmac[6];
        real_mac(real_id, dmac);
        EXPECT_EQ(memcmp(o, dmac, 6), 0) << "eth dst is not the real's MAC";
        EXPECT_EQ(memcmp(o + 6, kLbMac, 6), 0) << "eth src is not the LB MAC";
        EXPECT_EQ(get16(o + 12), ETH_P_IP);

        const uint8_t* h = o + 14;
        EXPECT_EQ(h[0], 0x45);
        EXPECT_EQ(h[1], in[14 + 1]) << "tos not copied from inner";
        EXPECT_EQ(get16(h + 2), get16(&in[14 + 2]) + 20) << "outer tot_len";
        EXPECT_EQ(get16(h + 6), 0) << "outer frag_off (DF=0, no offset)";
        EXPECT_EQ(h[8], 64) << "outer ttl";
        EXPECT_EQ(h[9], IPPROTO_IPIP);
        uint32_t saddr, daddr;
        memcpy(&saddr, h + 12, 4);
        memcpy(&daddr, h + 16, 4);
        EXPECT_EQ(daddr, real_addr(real_id)) << "outer daddr is not the real";
        EXPECT_EQ(saddr & kEncapMask, kEncapPrefix) << "outer saddr outside encap prefix";
        // A header whose checksum is right sums to 0 including the check field.
        EXPECT_EQ(inet_csum(h, 20), 0) << "outer IPv4 checksum wrong";
        EXPECT_EQ(memcmp(o + 34, &in[14], in.size() - 14), 0) << "inner packet modified";
    }

    uint32_t outer_daddr(const RunResult& r)
    {
        uint32_t d = 0;
        if (r.out.size() >= 34)
            memcpy(&d, &r.out[14 + 16], 4);
        return d;
    }
    uint32_t outer_saddr(const RunResult& r)
    {
        uint32_t s = 0;
        if (r.out.size() >= 34)
            memcpy(&s, &r.out[14 + 12], 4);
        return s;
    }
};

// ---- parse and pass/drop ---------------------------------------------------

TEST_F(Dataplane, ArpPasses)
{
    std::vector<uint8_t> f(60, 0);
    memset(&f[0], 0xff, 6);
    memcpy(&f[6], kClientMac, 6);
    put16(f, 12, ETH_P_ARP);
    auto r = run(f);
    EXPECT_EQ(r.ret, kXdpPass);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_PASS), 1u);
}

TEST_F(Dataplane, NonVipPassesAndCountsGlobal)
{
    auto before = counter(PB_STATS_GLOBAL, PB_CNT_PASS);
    // Right address, wrong port; and a different address altogether.
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 22, kTcpSyn)).ret, kXdpPass);
    EXPECT_EQ(run(tcp_frame(kClient, 40000, ip("10.0.0.2"), 80, kTcpSyn)).ret, kXdpPass);
    // Right address and port, wrong protocol.
    EXPECT_EQ(run(udp_frame(kClient, 40000, kVip, 80)).ret, kXdpPass);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_PASS), before + 3);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_PACKETS), 0u);
}

TEST_F(Dataplane, FragmentDropped)
{
    set_ring_all(kTcpVipId, kRealA);
    IpOpts mf;
    mf.frag = 0x2000;  // MF, offset 0: first fragment
    IpOpts off;
    off.frag = 185;    // offset 1480 bytes, last fragment
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 80, kTcpAck, mf)).ret, kXdpDrop);
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 80, kTcpAck, off)).ret, kXdpDrop);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_DROP_FRAG), 2u);
    // DF alone is not a fragment.
    IpOpts df;
    df.frag = 0x4000;
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 80, kTcpSyn, df)).ret, kXdpTx);
}

TEST_F(Dataplane, IpOptionsDropped)
{
    set_ring_all(kTcpVipId, kRealA);
    IpOpts o;
    o.option_words = 1;
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 80, kTcpSyn, o)).ret, kXdpDrop);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_DROP_OPTS), 1u);
}

TEST_F(Dataplane, TruncatedDropped)
{
    IpOpts o;
    o.claim_extra = 100;  // tot_len says 100 bytes more than the frame has
    EXPECT_EQ(run(tcp_frame(kClient, 40000, kVip, 80, kTcpSyn, o)).ret, kXdpDrop);
    // IPv4 header cut short.
    auto f = tcp_frame(kClient, 40000, kVip, 80, kTcpSyn);
    f.resize(14 + 12);
    EXPECT_EQ(run(f).ret, kXdpDrop);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_DROP_SHORT), 2u);
}

// ---- forwarding ---------------------------------------------------------------

TEST_F(Dataplane, TcpSynEncapsulatedToReal)
{
    set_ring_all(kTcpVipId, kRealA);
    IpOpts o;
    o.tos = 0x28;
    auto in = tcp_frame(kClient, 40000, kVip, 80, kTcpSyn, o);
    auto r = run(in);
    expect_encap(in, r, kRealA);

    uint32_t h = pb_flow_hash(kClient, kVip, htons(40000), htons(80), IPPROTO_TCP);
    EXPECT_EQ(outer_saddr(r), (kEncapPrefix & kEncapMask) | (h & ~kEncapMask))
        << "outer saddr must be prefix | flow_hash host bits";

    EXPECT_EQ(ct_real(kClient, 40000, kVip, 80, IPPROTO_TCP), kRealA);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_PACKETS), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_BYTES), in.size() - 14);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_SYN), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_HASH), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_TX), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 0u) << "a bare SYN does not look up";
    auto rs = real_counter(kRealA);
    EXPECT_EQ(rs.packets, 1u);
    EXPECT_EQ(rs.bytes, in.size() - 14);
}

TEST_F(Dataplane, OuterSourceVariesWithFlow)
{
    set_ring_all(kTcpVipId, kRealA);
    std::vector<uint32_t> seen;
    for (uint16_t p = 40000; p < 40064; p++) {
        auto r = run(tcp_frame(kClient, p, kVip, 80, kTcpSyn));
        ASSERT_EQ(r.ret, kXdpTx);
        seen.push_back(outer_saddr(r));
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    EXPECT_GT(seen.size(), 32u) << "RSS-friendly source should spread 64 flows";
}

// THE conntrack property: an established flow keeps its real when the ring
// changes under it.
TEST_F(Dataplane, ConntrackPinsFlowAcrossRingSwap)
{
    set_ring_all(kTcpVipId, kRealA);
    auto syn = tcp_frame(kClient, 40001, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);

    set_ring_all(kTcpVipId, kRealB);   // e.g. real A's weight changed

    auto ack = tcp_frame(kClient, 40001, kVip, 80, kTcpAck);
    auto r = run(ack);
    expect_encap(ack, r, kRealA);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 0u);

    // FIN does not remove the entry: the final ACK still reaches A.
    auto fin = tcp_frame(kClient, 40001, kVip, 80, kTcpFin | kTcpAck);
    expect_encap(fin, run(fin), kRealA);
    auto last = tcp_frame(kClient, 40001, kVip, 80, kTcpAck);
    expect_encap(last, run(last), kRealA);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 3u);

    // A new flow follows the new ring.
    auto syn2 = tcp_frame(kClient, 40002, kVip, 80, kTcpSyn);
    expect_encap(syn2, run(syn2), kRealB);
}

TEST_F(Dataplane, NoConntrackFollowsRing)
{
    set_config(PB_CFG_F_NO_CONNTRACK);
    set_ring_all(kTcpVipId, kRealA);
    auto syn = tcp_frame(kClient, 40003, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);
    set_ring_all(kTcpVipId, kRealB);
    auto ack = tcp_frame(kClient, 40003, kVip, 80, kTcpAck);
    expect_encap(ack, run(ack), kRealB);
    EXPECT_EQ(ct_real(kClient, 40003, kVip, 80, IPPROTO_TCP), PB_REAL_NONE)
        << "no-conntrack must not insert";
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT) + counter(kTcpVipId, PB_CNT_CT_MISS), 0u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_HASH), 2u);
}

TEST_F(Dataplane, PerVipNoConntrackFlag)
{
    add_vip(kVip, 80, IPPROTO_TCP, kTcpVipId, PB_VIP_F_NO_CONNTRACK);
    set_ring_all(kTcpVipId, kRealA);
    auto syn = tcp_frame(kClient, 40004, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);
    set_ring_all(kTcpVipId, kRealB);
    auto ack = tcp_frame(kClient, 40004, kVip, 80, kTcpAck);
    expect_encap(ack, run(ack), kRealB);
}

// A flow pinned to a real that is then deleted (reals[id].addr = 0, id
// freed) must move to the live ring owner at once, counted as a miss, and
// the entry must be rewritten, not black-holed until LRU eviction.
TEST_F(Dataplane, ConntrackEntryForDeletedRealRehashes)
{
    set_ring_all(kTcpVipId, kRealA);
    auto syn = tcp_frame(kClient, 40011, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);

    // Control plane removes real A: clears its slot and rebuilds the ring.
    pb_real gone{0, 0};
    uint32_t id = kRealA;
    ASSERT_EQ(bpf_map_update_elem(fd(skel_->maps.reals), &id, &gone, BPF_ANY), 0);
    set_ring_all(kTcpVipId, kRealB);

    auto ack = tcp_frame(kClient, 40011, kVip, 80, kTcpAck);
    expect_encap(ack, run(ack), kRealB);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 0u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_DROP_NO_REAL), 0u);
    EXPECT_EQ(ct_real(kClient, 40011, kVip, 80, IPPROTO_TCP), kRealB) << "entry overwritten";
    // And from now on it is an ordinary hit on B.
    expect_encap(ack, run(ack), kRealB);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 1u);
}

TEST_F(Dataplane, TcpMidFlowMissHashesAndInserts)
{
    // An ACK for a flow this LB never saw (e.g. after failover from another
    // LB): miss, hash, insert, and from then on a hit.
    set_ring_all(kTcpVipId, kRealB);
    auto ack = tcp_frame(kClient, 40005, kVip, 80, kTcpAck);
    expect_encap(ack, run(ack), kRealB);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_HASH), 1u);
    EXPECT_EQ(ct_real(kClient, 40005, kVip, 80, IPPROTO_TCP), kRealB);
    expect_encap(ack, run(ack), kRealB);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 1u);
}

TEST_F(Dataplane, RstForUnknownFlowForwardedWithoutState)
{
    set_ring_all(kTcpVipId, kRealA);
    auto rst = tcp_frame(kClient, 40009, kVip, 80, kTcpRst | kTcpAck);
    expect_encap(rst, run(rst), kRealA);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_HASH), 1u);
    EXPECT_EQ(ct_real(kClient, 40009, kVip, 80, IPPROTO_TCP), PB_REAL_NONE)
        << "a RST that misses must not create a conntrack entry";

    // A RST on a known flow still follows the entry (and does not delete it).
    auto syn = tcp_frame(kClient, 40010, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);
    set_ring_all(kTcpVipId, kRealB);
    auto rst2 = tcp_frame(kClient, 40010, kVip, 80, kTcpRst);
    expect_encap(rst2, run(rst2), kRealA);
    EXPECT_EQ(ct_real(kClient, 40010, kVip, 80, IPPROTO_TCP), kRealA);
}

TEST_F(Dataplane, UdpFlowEncapsulatedAndTracked)
{
    set_ring_all(kUdpVipId, kRealB);
    auto in = udp_frame(kClient, 5353, kVip, 5000);
    expect_encap(in, run(in), kRealB);
    EXPECT_EQ(ct_real(kClient, 5353, kVip, 5000, IPPROTO_UDP), kRealB);
    EXPECT_EQ(counter(kUdpVipId, PB_CNT_CT_MISS), 1u);
    set_ring_all(kUdpVipId, kRealA);
    expect_encap(in, run(in), kRealB);
    EXPECT_EQ(counter(kUdpVipId, PB_CNT_CT_HIT), 1u);
    EXPECT_EQ(counter(kUdpVipId, PB_CNT_SYN), 0u);
}

TEST_F(Dataplane, RingSlotNoneDrops)
{
    set_ring_all(kTcpVipId, PB_REAL_NONE);
    EXPECT_EQ(run(tcp_frame(kClient, 40006, kVip, 80, kTcpSyn)).ret, kXdpDrop);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_DROP_NO_REAL), 1u);
    EXPECT_EQ(ct_real(kClient, 40006, kVip, 80, IPPROTO_TCP), PB_REAL_NONE);
}

TEST_F(Dataplane, MissingRingDrops)
{
    // VIP configured but no ring installed yet.
    EXPECT_EQ(run(tcp_frame(kClient, 40007, kVip, 80, kTcpSyn)).ret, kXdpDrop);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_DROP_NO_REAL), 1u);
}

TEST_F(Dataplane, DeletedRealDrops)
{
    set_ring_all(kTcpVipId, 9);   // real 9 has no address
    EXPECT_EQ(run(tcp_frame(kClient, 40008, kVip, 80, kTcpSyn)).ret, kXdpDrop);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_DROP_NO_REAL), 1u);
}

TEST_F(Dataplane, OversizeDropsMtu)
{
    set_ring_all(kUdpVipId, kRealA);
    // IPv4 tot_len such that tot_len + 20 == PB_MAX_PACKET_LEN: allowed.
    size_t ok_payload = PB_MAX_PACKET_LEN - 20 - 20 - 8;
    auto ok = udp_frame(kClient, 6000, kVip, 5000, ok_payload);
    ASSERT_EQ(get16(&ok[16]) + 20u, PB_MAX_PACKET_LEN);
    expect_encap(ok, run(ok), kRealA);
    // One byte more: dropped.
    auto big = udp_frame(kClient, 6001, kVip, 5000, ok_payload + 1);
    EXPECT_EQ(run(big).ret, kXdpDrop);
    EXPECT_EQ(counter(kUdpVipId, PB_CNT_DROP_MTU), 1u);
    EXPECT_EQ(ct_real(kClient, 6001, kVip, 5000, IPPROTO_UDP), PB_REAL_NONE)
        << "a dropped packet must not create a flow";
}

// ---- ICMP ---------------------------------------------------------------------

std::vector<uint8_t> icmp_frame(uint8_t type, uint8_t code, const std::vector<uint8_t>& body)
{
    std::vector<uint8_t> m(8, 0);
    m[0] = type;
    m[1] = code;
    if (type == 3 && code == 4)
        put16(m, 6, 1400);   // next-hop MTU
    m.insert(m.end(), body.begin(), body.end());
    put16(m, 2, inet_csum(m.data(), m.size()));
    return ipv4_frame(ip("10.0.0.1"), kVip, IPPROTO_ICMP, m);
}

// The quoted datagram of a frag-needed about a DSR reply: the real's
// VIP:80 -> client:port packet, IP header + first 8 bytes of TCP.
std::vector<uint8_t> quoted_reply(uint16_t client_port)
{
    auto f = ipv4_frame(kVip, kClient, IPPROTO_TCP, tcp_hdr(80, client_port, kTcpAck));
    std::vector<uint8_t> q(f.begin() + 14, f.begin() + 14 + 20 + 8);
    put16(q, 2, 1500);   // the original was a full-size segment
    return q;
}

TEST_F(Dataplane, IcmpEchoPasses)
{
    set_config(PB_CFG_F_ICMP_PMTU);
    std::vector<uint8_t> payload(32, 0x5a);
    EXPECT_EQ(run(icmp_frame(8, 0, payload)).ret, kXdpPass);
    EXPECT_EQ(counter(PB_STATS_GLOBAL, PB_CNT_PASS), 1u);
}

TEST_F(Dataplane, IcmpFragNeededForwardedToFlowOwner)
{
    set_config(PB_CFG_F_ICMP_PMTU);
    set_ring_all(kTcpVipId, kRealA);
    auto syn = tcp_frame(kClient, 41000, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);
    // Ring moves to B; the flow is pinned to A by conntrack, so the ICMP
    // must go to A, the real that sent the too-big reply.
    set_ring_all(kTcpVipId, kRealB);

    auto icmp = icmp_frame(3, 4, quoted_reply(41000));
    auto r = run(icmp);
    expect_encap(icmp, r, kRealA);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_ICMP_PMTU_FWD), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 1u);

    // A frag-needed for a flow the table does not know goes where the hash
    // points, and does not create state.
    auto icmp2 = icmp_frame(3, 4, quoted_reply(41001));
    expect_encap(icmp2, run(icmp2), kRealB);
    EXPECT_EQ(ct_real(kClient, 41001, kVip, 80, IPPROTO_TCP), PB_REAL_NONE);
}

TEST_F(Dataplane, IcmpFragNeededPassesWhenDisabled)
{
    set_ring_all(kTcpVipId, kRealA);
    EXPECT_EQ(run(icmp_frame(3, 4, quoted_reply(41002))).ret, kXdpPass);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_ICMP_PMTU_FWD), 0u);
}

TEST_F(Dataplane, IcmpFragNeededForNonVipPasses)
{
    set_config(PB_CFG_F_ICMP_PMTU);
    auto f = ipv4_frame(ip("10.0.0.2"), kClient, IPPROTO_TCP, tcp_hdr(22, 41003, kTcpAck));
    std::vector<uint8_t> q(f.begin() + 14, f.begin() + 14 + 28);
    EXPECT_EQ(run(icmp_frame(3, 4, q)).ret, kXdpPass);
}

// ---- the hash contract ------------------------------------------------------

// The control plane (Maglev, hash-quality experiment, failover) assumes the
// data plane indexes the ring with exactly pb_flow_hash() % PB_RING_SIZE.
// Fill the ring with a real per region of slots and check, for many flows,
// that the real the program chose is the one userspace predicts.
TEST_F(Dataplane, UserspaceHashPredictsRingSlot)
{
    constexpr uint32_t kRegions = 16;
    for (uint32_t id = 1; id <= kRegions; id++)
        add_real(id);
    auto region_real = [](uint32_t slot) {
        return 1 + (uint32_t)((uint64_t)slot * kRegions / PB_RING_SIZE);
    };
    set_ring(kUdpVipId, region_real);

    std::mt19937 rng(12345);
    for (int i = 0; i < 300; i++) {
        uint32_t src = htonl(0x0a000000u | (rng() & 0x00ffffffu));
        uint16_t sport = (uint16_t)(1024 + rng() % 60000);
        auto r = run(udp_frame(src, sport, kVip, 5000));
        ASSERT_EQ(r.ret, kXdpTx);
        uint32_t h = pb_flow_hash(src, kVip, htons(sport), htons(5000), IPPROTO_UDP);
        uint32_t want = region_real(h % PB_RING_SIZE);
        ASSERT_EQ(outer_daddr(r), real_addr(want)) << "flow " << i << " slot " << h % PB_RING_SIZE;
    }
}

// ---- per-CPU conntrack semantics ---------------------------------------------

// LRU_PERCPU_HASH: an insert from CPU A creates the key for every CPU but
// zero-fills the other CPUs' values. A packet of the same flow processed on
// CPU B must treat that as a miss (and hash), not as a hit on real_id 0.
TEST_F(Dataplane, OtherCpuZeroValueIsAMissNotRealZero)
{
    cpu_set_t online = saved_affinity_;
    if (CPU_COUNT(&online) < 2)
        GTEST_SKIP() << "needs two CPUs";
    int cpus[2], n = 0;
    for (int c = 0; c < CPU_SETSIZE && n < 2; c++)
        if (CPU_ISSET(c, &online))
            cpus[n++] = c;
    auto pin = [](int c) {
        cpu_set_t s;
        CPU_ZERO(&s);
        CPU_SET(c, &s);
        return sched_setaffinity(0, sizeof(s), &s);
    };

    add_real(0);   // real 0 exists: a false "hit" would silently go there
    set_ring_all(kTcpVipId, kRealA);
    ASSERT_EQ(pin(cpus[0]), 0);
    auto syn = tcp_frame(kClient, 42000, kVip, 80, kTcpSyn);
    expect_encap(syn, run(syn), kRealA);

    ASSERT_EQ(pin(cpus[1]), 0);
    auto ack = tcp_frame(kClient, 42000, kVip, 80, kTcpAck);
    auto r = run(ack);
    expect_encap(ack, r, kRealA);   // hash, same ring: same real
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_MISS), 1u);
    EXPECT_EQ(counter(kTcpVipId, PB_CNT_CT_HIT), 0u);
}

// ---- verifier -----------------------------------------------------------------

TEST_F(Dataplane, ReportsVerifierStats)
{
    bpf_prog_info info{};
    uint32_t len = sizeof(info);
    ASSERT_EQ(bpf_prog_get_info_by_fd(bpf_program__fd(skel_->progs.xdp_packetbalance), &info, &len), 0);
    std::printf("[ verifier ] %s: verified_insns=%u xlated_bytes=%u (%u insns) jited_bytes=%u\n",
                info.name, info.verified_insns, info.xlated_prog_len,
                info.xlated_prog_len / 8, info.jited_prog_len);
    EXPECT_GT(info.verified_insns, 0u);
}

}  // namespace
