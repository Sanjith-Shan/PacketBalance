// SPDX-License-Identifier: MIT
//
// Property tests for build_ring. The measured disruption fractions and build
// times are printed so they can be quoted; the assertions are the bounds the
// design promises.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>

#include "packetbalance/maglev.h"
#include "packetbalance/vipspec.h"

namespace {

using pb::Backend;
using pb::build_ring;
using pb::HashMode;
using pb::ring_disruption;

constexpr uint32_t M = PB_RING_SIZE;

// Reals 10.1.x.y, real_id = i. Weights cycle through `weights`.
std::vector<Backend> make_backends(size_t n, const std::vector<uint32_t>& weights = {1}) {
    std::vector<Backend> out;
    for (size_t i = 0; i < n; ++i) {
        const std::string ip = "10.1." + std::to_string(i / 256) + "." + std::to_string(i % 256);
        out.push_back({static_cast<uint32_t>(i), pb::parse_ipv4(ip), weights[i % weights.size()]});
    }
    return out;
}

std::map<uint32_t, size_t> slot_counts(const std::vector<uint32_t>& ring) {
    std::map<uint32_t, size_t> c;
    for (uint32_t r : ring) ++c[r];
    return c;
}

// Every backend's share is within 1% (relative) of weight / sum(weights).
void expect_weighted_shares(const std::vector<Backend>& backends, HashMode mode) {
    const auto ring = build_ring(backends, mode);
    ASSERT_EQ(ring.size(), M);
    const auto counts = slot_counts(ring);
    EXPECT_EQ(counts.count(PB_REAL_NONE), 0u);
    double total_w = 0;
    for (const Backend& b : backends) total_w += b.weight;
    for (const Backend& b : backends) {
        const double expected = b.weight / total_w;
        const double observed = static_cast<double>(counts.at(b.real_id)) / M;
        EXPECT_NEAR(observed, expected, 0.01 * expected)
            << pb::hash_mode_name(mode) << " real " << b.real_id << " weight " << b.weight;
    }
}

class MaglevShare : public ::testing::TestWithParam<size_t> {};

TEST_P(MaglevShare, EqualWeights) {
    expect_weighted_shares(make_backends(GetParam()), HashMode::Maglev);
    expect_weighted_shares(make_backends(GetParam()), HashMode::Modulo);
}

TEST_P(MaglevShare, Weights1124) {
    expect_weighted_shares(make_backends(GetParam(), {1, 1, 2, 4}), HashMode::Maglev);
    expect_weighted_shares(make_backends(GetParam(), {1, 1, 2, 4}), HashMode::Modulo);
}

INSTANTIATE_TEST_SUITE_P(N, MaglevShare, ::testing::Values(2, 4, 16, 64));

TEST(Maglev, DependsOnlyOnTheSetOfAddressesAndWeights) {
    auto a = make_backends(16, {1, 1, 2, 4});
    auto b = a;
    std::shuffle(b.begin(), b.end(), std::mt19937(42));
    // Renumber the reals in b. The ring holds real_ids, so map each back to its
    // address before comparing: the address in every slot must be identical.
    std::map<uint32_t, uint32_t> addr_of_a, addr_of_b;
    for (Backend& x : b) x.real_id += 1000;
    for (const Backend& x : a) addr_of_a[x.real_id] = x.addr;
    for (const Backend& x : b) addr_of_b[x.real_id] = x.addr;

    for (HashMode mode : {HashMode::Maglev, HashMode::Modulo}) {
        const auto ra = build_ring(a, mode), rb = build_ring(b, mode);
        for (uint32_t i = 0; i < M; ++i)
            ASSERT_EQ(addr_of_a.at(ra[i]), addr_of_b.at(rb[i])) << "slot " << i;
        EXPECT_EQ(build_ring(a, mode), ra) << "not deterministic";
    }
}

TEST(Maglev, EmptyOrAllDrainingGivesAllNone) {
    const std::vector<uint32_t> none(M, PB_REAL_NONE);
    for (HashMode mode : {HashMode::Maglev, HashMode::Modulo}) {
        EXPECT_EQ(build_ring({}, mode), none);
        EXPECT_EQ(build_ring(make_backends(4, {0}), mode), none);
    }
}

TEST(Maglev, ZeroWeightTakesNoSlots) {
    auto backends = make_backends(4);
    backends[2].weight = 0;
    const auto counts = slot_counts(build_ring(backends, HashMode::Maglev));
    EXPECT_EQ(counts.count(backends[2].real_id), 0u);
    EXPECT_EQ(counts.size(), 3u);
    // Draining a real is the same ring as removing it.
    auto removed = backends;
    removed.erase(removed.begin() + 2);
    EXPECT_EQ(build_ring(backends, HashMode::Maglev), build_ring(removed, HashMode::Maglev));
}

TEST(Maglev, DuplicateAddressIsRejected) {
    auto backends = make_backends(3);
    backends[2].addr = backends[0].addr;
    EXPECT_THROW(build_ring(backends, HashMode::Maglev), std::invalid_argument);
}

TEST(Maglev, SmallPrimeRingIsComplete) {
    const auto ring = build_ring(make_backends(3, {1, 2, 3}), HashMode::Maglev, 13);
    ASSERT_EQ(ring.size(), 13u);
    EXPECT_EQ(std::count(ring.begin(), ring.end(), PB_REAL_NONE), 0);
}

class MaglevDisruption : public ::testing::TestWithParam<size_t> {};

// Removing one of N reals moves about 1/N of the slots: the removed real's own
// slots must move, and Maglev keeps collateral movement small.
TEST_P(MaglevDisruption, RemoveOne) {
    const size_t n = GetParam();
    const auto all = make_backends(n);
    auto fewer = all;
    fewer.erase(fewer.begin() + static_cast<long>(n / 2));
    const double d = ring_disruption(build_ring(all, HashMode::Maglev),
                                     build_ring(fewer, HashMode::Maglev));
    std::printf("[ measured ] maglev remove 1 of %2zu: %.4f of slots moved (1/N = %.4f)\n", n, d,
                1.0 / n);
    EXPECT_GE(d, 1.0 / n - 0.01);
    EXPECT_LT(d, 1.5 / n + 0.01);
}

TEST_P(MaglevDisruption, AddOne) {
    const size_t n = GetParam();
    const auto more = make_backends(n + 1);
    const auto base = std::vector<Backend>(more.begin(), more.begin() + static_cast<long>(n));
    const double d = ring_disruption(build_ring(base, HashMode::Maglev),
                                     build_ring(more, HashMode::Maglev));
    std::printf("[ measured ] maglev add 1 to %2zu: %.4f of slots moved (1/(N+1) = %.4f)\n", n, d,
                1.0 / (n + 1));
    EXPECT_GE(d, 1.0 / (n + 1) - 0.01);
    EXPECT_LT(d, 1.5 / n + 0.01);
}

INSTANTIATE_TEST_SUITE_P(N, MaglevDisruption, ::testing::Values(4, 16, 64));

TEST(Modulo, RemoveOneOfFourMovesMostSlots) {
    const auto all = make_backends(4);
    auto fewer = all;
    fewer.erase(fewer.begin() + 2);
    const double d = ring_disruption(build_ring(all, HashMode::Modulo),
                                     build_ring(fewer, HashMode::Modulo));
    std::printf("[ measured ] modulo remove 1 of 4: %.4f of slots moved\n", d);
    EXPECT_GT(d, 0.5);
}

// Raising one real's weight from 1 to 2 (N = 4) should grow its share from
// 1/4 to 2/5, i.e. move about 0.15 of the table TO that real, and shuffle
// little among the others.
TEST(Maglev, WeightChangeMovesSlotsTowardTheReweightedReal) {
    const auto before = make_backends(4);
    auto after = before;
    after[1].weight = 2;
    const auto ra = build_ring(before, HashMode::Maglev);
    const auto rb = build_ring(after, HashMode::Maglev);
    const uint32_t target = after[1].real_id;

    size_t to_target = 0, from_target = 0, among_others = 0;
    for (uint32_t i = 0; i < M; ++i) {
        if (ra[i] == rb[i]) continue;
        if (rb[i] == target) ++to_target;
        else if (ra[i] == target) ++from_target;
        else ++among_others;
    }
    const double expected = 2.0 / 5 - 1.0 / 4;
    std::printf("[ measured ] maglev weight 1->2 of 4: %.4f moved to it (expected %.4f), "
                "%.4f away from it, %.4f among others\n",
                double(to_target) / M, expected, double(from_target) / M,
                double(among_others) / M);
    EXPECT_NEAR(double(to_target) / M, expected, 0.02);
    EXPECT_LT(double(from_target) / M, 0.01);
    EXPECT_LT(double(among_others) / M, 0.05);
}

TEST(RingDisruption, IdenticalIsZeroDisjointIsOne) {
    const auto a = build_ring(make_backends(8), HashMode::Maglev);
    EXPECT_DOUBLE_EQ(ring_disruption(a, a), 0.0);
    auto other = make_backends(8);
    for (Backend& b : other) b.real_id += 100;
    EXPECT_DOUBLE_EQ(ring_disruption(a, build_ring(other, HashMode::Maglev)), 1.0);
    EXPECT_THROW(ring_disruption(a, std::vector<uint32_t>(7)), std::invalid_argument);
}

TEST(HashMode, ParseAndName) {
    EXPECT_EQ(pb::parse_hash_mode("maglev"), HashMode::Maglev);
    EXPECT_EQ(pb::parse_hash_mode("modulo"), HashMode::Modulo);
    EXPECT_STREQ(pb::hash_mode_name(HashMode::Maglev), "maglev");
    EXPECT_STREQ(pb::hash_mode_name(HashMode::Modulo), "modulo");
    EXPECT_THROW(pb::parse_hash_mode("Maglev"), std::invalid_argument);
    EXPECT_THROW(pb::parse_hash_mode(""), std::invalid_argument);
}

// The daemon rebuilds a ring on every health transition, so this must be cheap.
TEST(Maglev, BuildTime64Backends) {
    const auto backends = make_backends(64, {1, 1, 2, 4});
    build_ring(backends, HashMode::Maglev);  // warm up allocator and caches
    constexpr int kRuns = 20;
    double best_ms = 1e9, total_ms = 0;
    for (int i = 0; i < kRuns; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto ring = build_ring(backends, HashMode::Maglev);
        const auto t1 = std::chrono::steady_clock::now();
        ASSERT_EQ(ring.size(), M);
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best_ms = std::min(best_ms, ms);
        total_ms += ms;
    }
    std::printf("[ measured ] maglev build, 64 backends, M=%u: best %.2f ms, mean %.2f ms\n", M,
                best_ms, total_ms / kRuns);
#ifdef NDEBUG
    EXPECT_LT(total_ms / kRuns, 200.0);
#endif
}

}  // namespace
