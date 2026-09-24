#include <raikiri/common/time.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri {
namespace {

struct Stamper {
    const Clock* clock;

    [[nodiscard]] UnixNanos record() const noexcept { return clock->wall(); }
};

TEST(Clock, TheSeamIsWhatMakesAReplayReproducible) {
    const auto run = [] {
        TestClock clock{UnixNanos{1'700'000'000'000'000'000ULL}};
        const Stamper stamper{&clock};
        std::vector<std::uint64_t> stamps;
        for (int i = 0; i < 5; ++i) {
            stamps.push_back(stamper.record().ns);
            clock.advance(1'000'000);
        }
        return stamps;
    };
    EXPECT_EQ(run(), run());
    EXPECT_EQ(run().front(), 1'700'000'000'000'000'000ULL);
}

TEST(Clock, LiveTimeMovesForwardAndTheTwoScalesAreIndependent) {
    const LiveClock clock;
    const auto wall = clock.wall();
    const auto mono = clock.mono();

    EXPECT_GT(wall.ns, 1'600'000'000'000'000'000ULL);
    EXPECT_GE(clock.mono().ns, mono.ns);
}

TEST(Clock, TestClockDoesNotDriftUnderItsOwnReads) {
    TestClock clock{UnixNanos{5}, MonoNanos{7}};
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(clock.wall().ns, 5U);
        EXPECT_EQ(clock.mono().ns, 7U);
    }
    clock.advance(3);
    EXPECT_EQ(clock.wall().ns, 8U);
    EXPECT_EQ(clock.mono().ns, 10U);
}

} // namespace
} // namespace raikiri
