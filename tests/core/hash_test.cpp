// SPDX-License-Identifier: MIT
//
// Pins the flow hash. lb1 and lb2 (Experiment 4) and the XDP program and the
// control plane (Experiment 5) must agree on pb_flow_hash bit for bit. These
// expected values were computed once from include/packetbalance/hash.h; if an
// edit to the hash changes any of them, this test fails before a failover does.
#include <gtest/gtest.h>

#include "packetbalance/hash.h"
#include "packetbalance/maglev.h"
#include "packetbalance/vipspec.h"

namespace {

using pb::parse_ipv4;
using pb::port_to_be;

struct Pinned {
    const char* saddr;
    const char* daddr;
    uint16_t sport, dport;  // host order, converted below
    uint8_t proto;
    uint32_t expected;
};

TEST(FlowHash, MatchesPinnedValues) {
    const Pinned cases[] = {
        {"10.0.0.1", "198.51.100.1", 12345, 80, 6, 0xde1e4d19u},
        {"192.168.1.10", "198.51.100.1", 40000, 80, 6, 0x79af1f30u},
        {"172.16.5.4", "198.51.100.2", 53, 53, 17, 0xb9eabbabu},
        {"0.0.0.0", "0.0.0.0", 0, 0, 0, 0x56385eb7u},
        {"255.255.255.255", "198.51.100.1", 65535, 443, 6, 0xe1f7b089u},
    };
    for (const Pinned& c : cases) {
        EXPECT_EQ(pb_flow_hash(parse_ipv4(c.saddr), parse_ipv4(c.daddr), port_to_be(c.sport),
                               port_to_be(c.dport), c.proto),
                  c.expected)
            << c.saddr << ":" << c.sport << " -> " << c.daddr << ":" << c.dport << "/"
            << int(c.proto);
    }
}

TEST(FlowHash, BackendHashesArePinned) {
    // Maglev's offset and skip for a real. Changing these reshuffles every ring.
    EXPECT_EQ(pb_backend_hash_offset(parse_ipv4("10.0.0.21")), 0x13814be4u);
    EXPECT_EQ(pb_backend_hash_skip(parse_ipv4("10.0.0.21")), 0x2c509bd8u);
}

TEST(FlowHash, ProtocolAndDirectionMatter) {
    const uint32_t s = parse_ipv4("10.0.0.1"), d = parse_ipv4("198.51.100.1");
    const uint16_t sp = port_to_be(12345), dp = port_to_be(80);
    EXPECT_NE(pb_flow_hash(s, d, sp, dp, 6), pb_flow_hash(s, d, sp, dp, 17));
    EXPECT_NE(pb_flow_hash(s, d, sp, dp, 6), pb_flow_hash(d, s, dp, sp, 6));
}

TEST(RingIndex, IsHashModRingSize) {
    EXPECT_EQ(pb::ring_index(0), 0u);
    EXPECT_EQ(pb::ring_index(PB_RING_SIZE - 1), PB_RING_SIZE - 1);
    EXPECT_EQ(pb::ring_index(PB_RING_SIZE), 0u);
    EXPECT_EQ(pb::ring_index(0xFFFFFFFFu), 0xFFFFFFFFu % PB_RING_SIZE);
    EXPECT_EQ(pb::ring_index(0xde1e4d19u), 0xde1e4d19u % 65537u);
    EXPECT_EQ(pb::ring_index(100, 7), 2u);
}

}  // namespace
