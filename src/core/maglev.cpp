// SPDX-License-Identifier: MIT
//
// Maglev and modulo ring construction. See include/packetbalance/maglev.h for
// the contract and Eisenbud et al., "Maglev: A Fast and Reliable Software
// Network Load Balancer", NSDI 2016, section 3.4, for the algorithm.
#include "packetbalance/maglev.h"

#include <algorithm>
#include <stdexcept>

#include "packetbalance/hash.h"
#include "packetbalance/vipspec.h"

namespace pb {

namespace {

// Backends with weight > 0, sorted by address. Sorting first is what makes the
// ring a function of the SET of (addr, weight) pairs: real_id and the order
// the caller listed the backends in cannot influence the fill order.
std::vector<Backend> active_sorted(const std::vector<Backend>& backends) {
    std::vector<Backend> out;
    out.reserve(backends.size());
    for (const Backend& b : backends)
        if (b.weight > 0) out.push_back(b);
    std::sort(out.begin(), out.end(),
              [](const Backend& x, const Backend& y) { return x.addr < y.addr; });
    // Two backends with the same address would have the same permutation and
    // the ring would depend on which real_id sorted first. That is a config
    // error, not something to paper over.
    auto dup = std::adjacent_find(out.begin(), out.end(), [](const Backend& x, const Backend& y) {
        return x.addr == y.addr;
    });
    if (dup != out.end())
        throw std::invalid_argument("build_ring: duplicate backend address " +
                                    ipv4_to_string(dup->addr));
    return out;
}

// Maglev, section 3.4 of the paper.
//
// Each backend i gets a pseudo-random permutation of the M slots:
//
//     offset         = h1(addr) mod M
//     skip           = h2(addr) mod (M - 1) + 1
//     permutation[j] = (offset + j * skip) mod M
//
// Because M is prime and 1 <= skip < M, j -> permutation[j] visits every slot
// exactly once. The table is then filled in rounds: each backend in turn walks
// its permutation from where it last stopped (next[i]) to the first slot that
// is still empty, and claims it. Every backend claims one slot per round, so
// shares differ by at most one slot, and because a backend's preferences do
// not depend on who else is present, removing one backend mostly just hands
// its slots to others (about 1/N of the table moves, not all of it).
//
// Weights. Katran's extension: a backend of weight w takes w turns per round.
// Turns are interleaved rather than taken back to back: a round is w_max
// passes over the backends, and on pass t backend i takes a turn only if
// t < w_i. Over the whole fill backend i therefore claims w_i / sum(w) of the
// slots, up to one partial round. With equal weights this is exactly the
// paper's algorithm.
//
// Cost. permutation[] is never materialised (that would be N * M words);
// each backend keeps only its current position and advances it by `skip`.
// Early turns almost always land on an empty slot; the expected number of
// probes for the whole fill is about M * ln(M), independent of N.
std::vector<uint32_t> build_maglev(const std::vector<Backend>& sorted, uint32_t M) {
    std::vector<uint32_t> entry(M, PB_REAL_NONE);
    if (sorted.empty() || M == 0) return entry;

    const size_t N = sorted.size();
    std::vector<uint32_t> skip(N), next(N);  // next[i]: slot = permutation[i][cursor]
    uint32_t w_max = 0;
    for (size_t i = 0; i < N; ++i) {
        const uint32_t offset = pb_backend_hash_offset(sorted[i].addr) % M;
        skip[i] = M > 1 ? pb_backend_hash_skip(sorted[i].addr) % (M - 1) + 1 : 1;
        next[i] = offset;  // permutation[i][0]
        w_max = std::max(w_max, sorted[i].weight);
    }

    uint32_t filled = 0;
    while (true) {
        for (uint32_t pass = 0; pass < w_max; ++pass) {
            for (size_t i = 0; i < N; ++i) {
                if (pass >= sorted[i].weight) continue;
                // Walk backend i's permutation to its next unclaimed slot.
                uint32_t c = next[i];
                while (entry[c] != PB_REAL_NONE) c = static_cast<uint32_t>((uint64_t{c} + skip[i]) % M);
                entry[c] = sorted[i].real_id;
                next[i] = static_cast<uint32_t>((uint64_t{c} + skip[i]) % M);
                if (++filled == M) return entry;
            }
        }
    }
}

// Modulo: slot i -> list[i mod L], where the list holds each backend (sorted
// by address) repeated `weight` times. Weights are honoured, but adding or
// removing a backend changes L and so renumbers nearly every slot. This is
// the baseline consistent hashing is measured against.
std::vector<uint32_t> build_modulo(const std::vector<Backend>& sorted, uint32_t M) {
    std::vector<uint32_t> entry(M, PB_REAL_NONE);
    std::vector<uint32_t> list;
    for (const Backend& b : sorted) list.insert(list.end(), b.weight, b.real_id);
    if (list.empty()) return entry;
    for (uint32_t i = 0; i < M; ++i) entry[i] = list[i % list.size()];
    return entry;
}

}  // namespace

HashMode parse_hash_mode(const std::string& s) {
    if (s == "maglev") return HashMode::Maglev;
    if (s == "modulo") return HashMode::Modulo;
    throw std::invalid_argument("bad hash mode (maglev|modulo): " + s);
}

const char* hash_mode_name(HashMode m) {
    switch (m) {
        case HashMode::Maglev: return "maglev";
        case HashMode::Modulo: return "modulo";
    }
    return "?";
}

std::vector<uint32_t> build_ring(const std::vector<Backend>& backends, HashMode mode,
                                 uint32_t ring_size) {
    const std::vector<Backend> sorted = active_sorted(backends);
    return mode == HashMode::Maglev ? build_maglev(sorted, ring_size)
                                    : build_modulo(sorted, ring_size);
}

double ring_disruption(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size())
        throw std::invalid_argument("ring_disruption: rings differ in size");
    if (a.empty()) return 0.0;
    size_t changed = 0;
    for (size_t i = 0; i < a.size(); ++i) changed += a[i] != b[i];
    return static_cast<double>(changed) / static_cast<double>(a.size());
}

}  // namespace pb
