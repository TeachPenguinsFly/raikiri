#include <raikiri/observ/histogram.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

namespace raikiri::observ {
namespace {

TEST(Histogram, EmptyReportsZeros) {
    Histogram h;
    const auto snap = h.snapshot();
    EXPECT_EQ(snap.count, 0U);
    EXPECT_EQ(snap.p50_ns, 0U);
    EXPECT_EQ(snap.max_ns, 0U);
}

TEST(Histogram, SmallValuesAreExact) {
    Histogram h;
    for (std::uint64_t v = 0; v < 32; ++v) {
        h.record(v);
    }
    const auto snap = h.snapshot();
    EXPECT_EQ(snap.count, 32U);
    EXPECT_EQ(snap.max_ns, 31U);
    EXPECT_EQ(snap.p50_ns, 16U); // exact zone: bucket == value
}

TEST(Histogram, PercentilesWithinRelativeErrorBound) {
    // Uniform ints in [1000, 1'000'000): compare against true percentiles
    // computed by sorting; histogram error must stay within 1/32 + one
    // bucket of pessimism.
    std::mt19937_64 rng{42};
    std::uniform_int_distribution<std::uint64_t> dist{1000, 999'999};
    Histogram h;
    std::vector<std::uint64_t> values;
    values.reserve(100'000);
    for (int i = 0; i < 100'000; ++i) {
        const auto v = dist(rng);
        values.push_back(v);
        h.record(v);
    }
    std::sort(values.begin(), values.end());
    const auto snap = h.snapshot();

    const auto check = [&](std::uint64_t reported, double q) {
        const auto exact = values[static_cast<std::size_t>(q * (values.size() - 1))];
        EXPECT_GE(reported, exact - exact / 16) << q; // never wildly low
        EXPECT_LE(reported, exact + exact / 8) << q;  // pessimism bounded
    };
    check(snap.p50_ns, 0.50);
    check(snap.p90_ns, 0.90);
    check(snap.p99_ns, 0.99);
    check(snap.p999_ns, 0.999);
    EXPECT_EQ(snap.max_ns, values.back());
}

TEST(Histogram, PercentilesNeverExceedMax) {
    Histogram h;
    h.record(1'000'000);
    h.record(3);
    const auto snap = h.snapshot();
    EXPECT_LE(snap.p999_ns, snap.max_ns);
    EXPECT_EQ(snap.max_ns, 1'000'000U);
}

TEST(Histogram, SnapshotAndResetClears) {
    Histogram h;
    h.record(100);
    const auto first = h.snapshot_and_reset();
    EXPECT_EQ(first.count, 1U);
    const auto second = h.snapshot();
    EXPECT_EQ(second.count, 0U);
    EXPECT_EQ(second.max_ns, 0U);
}

} // namespace
} // namespace raikiri::observ
