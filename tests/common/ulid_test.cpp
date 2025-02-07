#include <raikiri/common/ulid.hpp>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

namespace raikiri {
namespace {

constexpr UnixNanos k_t0{1'700'000'000'000'000'000ULL};

TEST(Ulid, ShapeAndAlphabet) {
    UlidGen gen{42};
    const auto id = gen.next(k_t0);
    EXPECT_EQ(id.len, 26);
    for (const char c : id.view()) {
        EXPECT_NE(std::string_view{"0123456789ABCDEFGHJKMNPQRSTVWXYZ"}.find(c),
                  std::string_view::npos)
            << c;
    }
}

TEST(Ulid, MonotonicWithinSameMillisecond) {
    UlidGen gen{42};
    auto prev = gen.next(k_t0);
    for (int i = 0; i < 1000; ++i) {
        const auto id = gen.next(k_t0);
        EXPECT_LT(prev.view(), id.view());
        prev = id;
    }
}

TEST(Ulid, TimeOrderedAcrossMilliseconds) {
    UlidGen gen{42};
    const auto early = gen.next(k_t0);
    const auto late = gen.next(k_t0 + 5'000'000ULL); // +5ms
    EXPECT_LT(early.view(), late.view());
}

TEST(Ulid, NoCollisionsInBulk) {
    UlidGen gen; // entropy-seeded
    std::unordered_set<std::string> seen;
    UnixNanos t = k_t0;
    for (int i = 0; i < 100'000; ++i) {
        if (i % 100 == 0) {
            t = t + 1'000'000ULL;
        }
        const auto id = gen.next(t);
        EXPECT_TRUE(seen.emplace(id.view()).second) << "collision at " << i;
    }
}

TEST(Ulid, DeterministicWithSeed) {
    UlidGen a{7};
    UlidGen b{7};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next(k_t0).view(), b.next(k_t0).view());
    }
}

} // namespace
} // namespace raikiri
