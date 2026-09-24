// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "packetbalance/config.h"

namespace {

using pb::Config;
using pb::parse_config;

TEST(Cidr, ParsesAndMasks) {
    const pb::Cidr c = pb::parse_cidr("10.99.0.7/24");
    EXPECT_EQ(pb::ipv4_to_string(c.prefix_be), "10.99.0.0");
    EXPECT_EQ(pb::ipv4_to_string(c.mask_be), "255.255.255.0");
    EXPECT_EQ(c.len, 24);
    EXPECT_EQ(c.str(), "10.99.0.0/24");
    EXPECT_EQ(pb::ipv4_to_string(pb::parse_cidr("1.2.3.4/32").mask_be), "255.255.255.255");
    EXPECT_EQ(pb::parse_cidr("1.2.3.4/0").mask_be, 0u);
}

TEST(Cidr, RejectsGarbage) {
    for (const char* s : {"10.0.0.0", "10.0.0.0/33", "10.0.0.0/", "10.0.0/24", "10.0.0.0/2x"})
        EXPECT_THROW(pb::parse_cidr(s), std::invalid_argument) << s;
}

TEST(XdpMode, RoundTrip) {
    for (const char* s : {"native", "generic", "auto"})
        EXPECT_STREQ(pb::xdp_mode_name(pb::parse_xdp_mode(s)), s);
    EXPECT_THROW(pb::parse_xdp_mode("offload"), std::invalid_argument);
}

#ifdef PB_HAVE_YAML

const char* kMinimal = R"(
interface: eth1
encap_src_prefix: 10.99.0.0/24
vips:
  - address: 198.51.100.9
    port: 80
    proto: tcp
    reals:
      - { address: 10.77.0.2, weight: 3 }
      - { address: 10.77.0.3 }
)";

TEST(Config, MinimalUsesDefaults) {
    const Config c = parse_config(kMinimal);
    EXPECT_EQ(c.interface, "eth1");
    EXPECT_EQ(c.xdp_mode, pb::XdpMode::Auto);
    EXPECT_EQ(c.hash, pb::HashMode::Maglev);
    EXPECT_TRUE(c.conntrack_enabled);
    EXPECT_EQ(c.conntrack_size, PB_CT_DEFAULT_SIZE);
    EXPECT_EQ(c.socket, "/run/packetbalance.sock");
    EXPECT_EQ(c.pin_path, PB_PIN_DIR);
    EXPECT_EQ(c.metrics_listen, "127.0.0.1:9101");
    EXPECT_FALSE(c.next_hop_be.has_value());
    EXPECT_EQ(c.health.fall, 3u);
    EXPECT_EQ(c.health.rise, 2u);
    ASSERT_EQ(c.vips.size(), 1u);
    EXPECT_EQ(c.vips[0].spec.str(), "198.51.100.9:80/tcp");
    ASSERT_EQ(c.vips[0].reals.size(), 2u);
    EXPECT_EQ(c.vips[0].reals[0].weight, 3u);
    EXPECT_EQ(c.vips[0].reals[1].weight, 1u);  // default
}

TEST(Config, ParsesTheShippedExample) {
    // deploy/packetbalance.yaml is documentation; it must stay parseable.
    const auto path =
        std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "deploy" / "packetbalance.yaml";
    if (!std::filesystem::exists(path)) GTEST_SKIP() << "no " << path;
    const Config c = pb::load_config_file(path.string());
    EXPECT_EQ(c.interface, "veth0");
    EXPECT_EQ(c.encap_src.str(), "10.99.0.0/24");
    EXPECT_EQ(c.health.interval_ms, 1000u);
    EXPECT_EQ(c.health.timeout_ms, 500u);
    ASSERT_EQ(c.vips.size(), 3u);
    EXPECT_EQ(c.vips[1].spec.str(), "198.51.100.1:5000/udp");
    EXPECT_EQ(c.vips[2].reals.size(), 4u);
}

TEST(Config, FullSchema) {
    const Config c = parse_config(R"(
interface: veth0
xdp_mode: generic
hash: modulo
encap_src_prefix: 10.99.0.0/16
next_hop: 10.0.0.1
conntrack: { enabled: false, size: 65536 }
socket: /tmp/pb.sock
pin_path: /sys/fs/bpf/pbtest
metrics: { listen: 127.0.0.1:9200 }
health_check: { interval_ms: 200, timeout_ms: 100, fall: 5, rise: 1 }
vips: []
)");
    EXPECT_EQ(c.xdp_mode, pb::XdpMode::Generic);
    EXPECT_EQ(c.hash, pb::HashMode::Modulo);
    EXPECT_EQ(pb::ipv4_to_string(c.encap_src.mask_be), "255.255.0.0");
    ASSERT_TRUE(c.next_hop_be.has_value());
    EXPECT_EQ(pb::ipv4_to_string(*c.next_hop_be), "10.0.0.1");
    EXPECT_FALSE(c.conntrack_enabled);
    EXPECT_EQ(c.conntrack_size, 65536u);
    EXPECT_EQ(c.socket, "/tmp/pb.sock");
    EXPECT_EQ(c.pin_path, "/sys/fs/bpf/pbtest");
    EXPECT_EQ(c.metrics_listen, "127.0.0.1:9200");
    EXPECT_EQ(c.health.fall, 5u);
    EXPECT_TRUE(c.vips.empty());
}

void expect_error(const std::string& yaml, const std::string& needle) {
    try {
        parse_config(yaml);
        ADD_FAILURE() << "expected an error containing '" << needle << "'";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
    }
}

TEST(Config, RejectsSchemaErrors) {
    const std::string base = "interface: eth0\nencap_src_prefix: 10.99.0.0/24\n";
    expect_error("encap_src_prefix: 10.99.0.0/24\n", "`interface` is required");
    expect_error("interface: eth0\n", "`encap_src_prefix` is required");
    expect_error(base + "conntrak: { size: 5 }\n", "unknown key `conntrak`");
    expect_error(base + "xdp_mode: offload\n", "xdp_mode");
    expect_error(base + "hash: rendezvous\n", "hash");
    expect_error(base + "vips: [{address: 1.2.3.4, port: 80, proto: sctp}]\n", "tcp or udp");
    expect_error(base + "vips: [{address: 1.2.3.4, port: 0, proto: tcp}]\n", "port` out of range");
    expect_error(base + "vips: [{address: 1.2.3, port: 80, proto: tcp}]\n", "vips[0].address");
    expect_error(base + "vips: [{address: 1.2.3.4, port: 80, proto: tcp},"
                        " {address: 1.2.3.4, port: 80, proto: tcp}]\n",
                 "duplicate vip");
    expect_error(base + "vips: [{address: 1.2.3.4, port: 80, proto: tcp,"
                        " reals: [{address: 10.0.0.1}, {address: 10.0.0.1}]}]\n",
                 "duplicate real 10.0.0.1");
    expect_error(base + "vips: [{address: 1.2.3.4, port: 80, proto: tcp,"
                        " reals: [{address: 10.0.0.1, wieght: 2}]}]\n",
                 "unknown key `vips[0].reals[0].wieght`");
    expect_error(base + "health_check: { interval_ms: 100, timeout_ms: 500 }\n", "timeout_ms");
    expect_error("interface: [unclosed\n", "YAML syntax");
}

#else

TEST(Config, NeedsYamlCpp) { GTEST_SKIP() << "built without yaml-cpp"; }

#endif

}  // namespace
