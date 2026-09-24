// SPDX-License-Identifier: MIT
//
// "ADDR:PORT/PROTO" parsing shared by the daemon, pbctl and the tools.
// Header only, no dependencies beyond the standard library.
#pragma once

#include <cstdint>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>

#include "packetbalance/abi.h"

namespace pb {

// Parses dotted-quad IPv4 to network byte order. Throws std::invalid_argument.
inline uint32_t parse_ipv4(const std::string& s) {
    // Strict: exactly four decimal octets, digits only, no sign, no whitespace.
    unsigned char be[4];
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (pos >= s.size() || !std::isdigit((unsigned char)s[pos]))
            throw std::invalid_argument("bad IPv4 address: " + s);
        unsigned v = 0;
        size_t start = pos;
        while (pos < s.size() && std::isdigit((unsigned char)s[pos])) {
            v = v * 10 + (unsigned)(s[pos] - '0');
            if (v > 255 || pos - start >= 3) throw std::invalid_argument("bad IPv4 address: " + s);
            ++pos;
        }
        be[i] = (unsigned char)v;
        if (i < 3) {
            if (pos >= s.size() || s[pos] != '.') throw std::invalid_argument("bad IPv4 address: " + s);
            ++pos;
        }
    }
    if (pos != s.size()) throw std::invalid_argument("bad IPv4 address: " + s);
    uint32_t out;
    std::memcpy(&out, be, 4);
    return out;
}

// Strict decimal port 1..65535, digits only.
inline uint16_t parse_port(const std::string& s) {
    if (s.empty() || s.size() > 5) throw std::invalid_argument("bad port: " + s);
    unsigned v = 0;
    for (char ch : s) {
        if (!std::isdigit((unsigned char)ch)) throw std::invalid_argument("bad port: " + s);
        v = v * 10 + (unsigned)(ch - '0');
    }
    if (v < 1 || v > 65535) throw std::invalid_argument("bad port: " + s);
    return (uint16_t)v;
}

inline std::string ipv4_to_string(uint32_t addr_be) {
    unsigned char b[4];
    std::memcpy(b, &addr_be, 4);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." +
           std::to_string(b[2]) + "." + std::to_string(b[3]);
}

inline uint16_t port_to_be(uint16_t host_port) {
    unsigned char be[2] = {(unsigned char)(host_port >> 8), (unsigned char)(host_port & 0xff)};
    uint16_t out;
    std::memcpy(&out, be, 2);
    return out;
}
inline uint16_t port_from_be(uint16_t port_be) {
    unsigned char b[2];
    std::memcpy(b, &port_be, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
}

struct VipSpec {
    uint32_t addr_be = 0;   // network order
    uint16_t port_be = 0;   // network order
    uint8_t  proto = 0;     // 6 (tcp) or 17 (udp)

    // "198.51.100.1:80/tcp". Throws std::invalid_argument.
    static VipSpec parse(const std::string& s) {
        auto colon = s.rfind(':');
        auto slash = s.rfind('/');
        if (colon == std::string::npos || slash == std::string::npos || slash < colon)
            throw std::invalid_argument("bad VIP, want ADDR:PORT/PROTO: " + s);
        VipSpec v;
        v.addr_be = parse_ipv4(s.substr(0, colon));
        v.port_be = port_to_be(parse_port(s.substr(colon + 1, slash - colon - 1)));
        std::string proto = s.substr(slash + 1);
        if (proto == "tcp") v.proto = 6;
        else if (proto == "udp") v.proto = 17;
        else throw std::invalid_argument("bad proto in VIP (tcp|udp): " + s);
        return v;
    }

    std::string str() const {
        return ipv4_to_string(addr_be) + ":" + std::to_string(port_from_be(port_be)) +
               (proto == 6 ? "/tcp" : proto == 17 ? "/udp" : "/?");
    }

    pb_vip_key key() const {
        pb_vip_key k{};
        k.addr = addr_be;
        k.port = port_be;
        k.proto = proto;
        return k;
    }

    bool operator==(const VipSpec& o) const {
        return addr_be == o.addr_be && port_be == o.port_be && proto == o.proto;
    }
    bool operator<(const VipSpec& o) const {
        if (addr_be != o.addr_be) return addr_be < o.addr_be;
        if (port_be != o.port_be) return port_be < o.port_be;
        return proto < o.proto;
    }
};

}  // namespace pb
