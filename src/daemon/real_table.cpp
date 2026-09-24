// SPDX-License-Identifier: MIT
#include "real_table.h"

#include <bpf/bpf.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "log.h"
#include "map_reader.h"
#include "packetbalance/vipspec.h"

namespace pb {

std::optional<Mac> Mac::parse(const std::string& s) {
    unsigned b[6];
    char tail;
    if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x%c", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
                    &tail) != 6)
        return std::nullopt;
    Mac m;
    for (int i = 0; i < 6; ++i) {
        if (b[i] > 0xff) return std::nullopt;
        m.bytes[i] = static_cast<uint8_t>(b[i]);
    }
    return m;
}

std::string Mac::str() const {
    char buf[18];
    std::snprintf(buf, sizeof buf, "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1], bytes[2],
                  bytes[3], bytes[4], bytes[5]);
    return buf;
}

bool Mac::is_zero() const {
    for (uint8_t b : bytes)
        if (b) return false;
    return true;
}

RealTable::RealTable(int reals_fd, int neigh_fd, const MapReader& reader)
    : reals_fd_(reals_fd), neigh_fd_(neigh_fd), reader_(reader) {}

void RealTable::adopt() {
    uint32_t highest = 0;
    for (uint32_t id = 1; id < PB_MAX_REALS; ++id) {
        pb_real r{};
        if (bpf_map_lookup_elem(reals_fd_, &id, &r) != 0 || r.addr == 0) continue;
        Slot& s = slots_[id];
        s.addr = r.addr;
        pb_mac m{};
        if (bpf_map_lookup_elem(neigh_fd_, &id, &m) == 0) {
            Mac mac;
            std::memcpy(mac.bytes.data(), m.mac, 6);
            if (!mac.is_zero()) s.mac = mac;
        }
        by_addr_[r.addr] = id;
        highest = id;
        log::info("adopted real_id {} = {} (mac {}) from pinned maps", id, ipv4_to_string(r.addr),
                  s.mac ? s.mac->str() : "unresolved");
    }
    cursor_ = highest + 1 < PB_MAX_REALS ? highest + 1 : 1;
}

void RealTable::drop_unreferenced() {
    for (uint32_t id = 1; id < PB_MAX_REALS; ++id) {
        Slot& s = slots_[id];
        if (s.addr == 0 || s.refs > 0) continue;
        log::info("freeing real_id {} ({}): no longer in any VIP", id, ipv4_to_string(s.addr));
        by_addr_.erase(s.addr);
        s = Slot{};
        write_slot(id);
    }
}

uint32_t RealTable::acquire(uint32_t addr) {
    if (auto it = by_addr_.find(addr); it != by_addr_.end()) {
        ++slots_[it->second].refs;
        return it->second;
    }
    for (uint32_t n = 0; n < PB_MAX_REALS - 1; ++n) {
        const uint32_t id = cursor_;
        cursor_ = cursor_ + 1 < PB_MAX_REALS ? cursor_ + 1 : 1;
        if (slots_[id].addr != 0) continue;
        slots_[id] = Slot{.addr = addr, .refs = 1, .mac = std::nullopt};
        by_addr_[addr] = id;
        reader_.reset_real_counters(id);
        write_slot(id);
        return id;
    }
    throw std::runtime_error("too many reals (max " + std::to_string(PB_MAX_REALS - 1) + ")");
}

void RealTable::release(uint32_t addr) {
    auto it = by_addr_.find(addr);
    if (it == by_addr_.end()) return;
    Slot& s = slots_[it->second];
    if (s.refs > 0 && --s.refs > 0) return;
    const uint32_t id = it->second;
    by_addr_.erase(it);
    s = Slot{};
    write_slot(id);
}

std::optional<uint32_t> RealTable::id_of(uint32_t addr) const {
    auto it = by_addr_.find(addr);
    return it == by_addr_.end() ? std::nullopt : std::optional(it->second);
}

std::optional<uint32_t> RealTable::addr_of(uint32_t real_id) const {
    if (real_id == 0 || real_id >= PB_MAX_REALS || slots_[real_id].addr == 0) return std::nullopt;
    return slots_[real_id].addr;
}

std::vector<uint32_t> RealTable::addrs() const {
    std::vector<uint32_t> out;
    for (const auto& [addr, id] : by_addr_) out.push_back(addr);
    return out;
}

std::optional<Mac> RealTable::mac_of(uint32_t addr) const {
    auto id = id_of(addr);
    return id ? slots_[*id].mac : std::nullopt;
}

bool RealTable::set_mac(uint32_t addr, const Mac& mac) {
    auto id = id_of(addr);
    if (!id || slots_[*id].mac == mac) return false;
    slots_[*id].mac = mac;
    write_slot(*id);
    return true;
}

// neigh is written before reals so that the moment the data plane can see an
// address for this id, it also has the MAC to send to.
void RealTable::write_slot(uint32_t id) {
    const Slot& s = slots_[id];
    pb_mac m{};
    if (s.mac) std::memcpy(m.mac, s.mac->bytes.data(), 6);
    if (int err = bpf_map_update_elem(neigh_fd_, &id, &m, BPF_ANY))
        throw std::runtime_error("write neigh[" + std::to_string(id) + "]: " + std::strerror(-err));
    pb_real r{.addr = s.addr, .flags = 0};
    if (int err = bpf_map_update_elem(reals_fd_, &id, &r, BPF_ANY))
        throw std::runtime_error("write reals[" + std::to_string(id) + "]: " + std::strerror(-err));
}

}  // namespace pb
