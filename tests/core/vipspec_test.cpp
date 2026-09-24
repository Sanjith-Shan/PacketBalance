// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <stdexcept>

#include "packetbalance/vipspec.h"

namespace {

using pb::VipSpec;

TEST(VipSpec, ParseAndStrRoundTrip) {
    for (const char* s : {"198.51.100.1:80/tcp", "10.0.0.1:53/udp", "0.0.0.0:1/tcp",
                          "255.255.255.255:65535/udp"}) {
        EXPECT_EQ(VipSpec::parse(s).str(), s);
        EXPECT_EQ(VipSpec::parse(VipSpec::parse(s).str()), VipSpec::parse(s));
    }
}

TEST(VipSpec, FieldsAreNetworkOrder) {
    const VipSpec v = VipSpec::parse("198.51.100.1:80/tcp");
    const auto* a = reinterpret_cast<const unsigned char*>(&v.addr_be);
    EXPECT_EQ(a[0], 198);
    EXPECT_EQ(a[3], 1);
    const auto* p = reinterpret_cast<const unsigned char*>(&v.port_be);
    EXPECT_EQ(p[0], 0);
    EXPECT_EQ(p[1], 80);
    EXPECT_EQ(v.proto, 6);

    const pb_vip_key k = v.key();
    EXPECT_EQ(k.addr, v.addr_be);
    EXPECT_EQ(k.port, v.port_be);
    EXPECT_EQ(k.proto, 6);
    EXPECT_EQ(k.pad, 0);
}

TEST(VipSpec, RejectsBadInput) {
    for (const char* s : {"", "198.51.100.1", "198.51.100.1:80", "198.51.100.1/tcp:80",
                          "198.51.100.1:0/tcp", "198.51.100.1:65536/tcp", "198.51.100.1:80/icmp",
                          "198.51.100.256:80/tcp", "198.51.100:80/tcp", "198.51.100.1.1:80/tcp",
                          "host:80/tcp", "198.51.100.1:x/tcp"}) {
        EXPECT_ANY_THROW(VipSpec::parse(s)) << s;
    }
}

TEST(VipSpec, Ipv4RoundTrip) {
    for (const char* s : {"0.0.0.0", "10.0.0.21", "255.255.255.255"})
        EXPECT_EQ(pb::ipv4_to_string(pb::parse_ipv4(s)), s);
    EXPECT_THROW(pb::parse_ipv4("10.0.0.1 "), std::invalid_argument);
    EXPECT_EQ(pb::port_from_be(pb::port_to_be(8080)), 8080);
}

}  // namespace
