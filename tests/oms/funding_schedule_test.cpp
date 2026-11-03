#include <raikiri/oms/funding_schedule.hpp>
#include <raikiri/oms/funding_split.hpp>

#include <gtest/gtest.h>

#include <array>

namespace raikiri::oms {
namespace {

constexpr std::uint64_t k_second = 1'000'000'000ULL;
constexpr std::uint16_t k_manual = 1;

FundingUpdate update(std::uint64_t next_s, std::int64_t rate = 100'000) {
    FundingUpdate out{};
    out.instrument = InstrumentId{7};
    out.rate = FixedE9{rate};
    out.mark_price = PriceTicks{5};
    out.index_price = PriceTicks::undef();
    out.next_funding_time = UnixNanos{next_s * k_second};
    out.accrual = FundingAccrual::Discrete;
    return out;
}

TEST(FundingSchedule, ASettlementComesDueOnceAtItsPublishedTime) {
    FundingSchedule schedule;
    schedule.on_update(update(100));
    EXPECT_TRUE(schedule.due(UnixNanos{99 * k_second}).empty());
    const auto due = schedule.due(UnixNanos{100 * k_second});
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].rate.raw, 100'000);
    EXPECT_EQ(due[0].settled_at.ns, 100 * k_second);
    EXPECT_TRUE(schedule.due(UnixNanos{101 * k_second}).empty());
    // the same schedule published again is not another settlement
    schedule.on_update(update(100));
    EXPECT_TRUE(schedule.due(UnixNanos{102 * k_second}).empty());
}

TEST(FundingSchedule, TheLastRateBeforeTheInstantIsTheOneOwed) {
    FundingSchedule schedule;
    schedule.on_update(update(100, 100'000));
    schedule.on_update(update(100, 200'000));
    const auto due = schedule.due(UnixNanos{100 * k_second});
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].rate.raw, 200'000);
}

TEST(FundingSchedule, AVenueMovingOnFirstStillSettlesTheOldInstantAtItsOldRate) {
    FundingSchedule schedule;
    schedule.on_update(update(100, 100'000));
    schedule.on_update(update(200, 300'000));
    const auto due = schedule.due(UnixNanos{100 * k_second});
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due[0].settled_at.ns, 100 * k_second);
    EXPECT_EQ(due[0].rate.raw, 100'000);
    const auto next = schedule.due(UnixNanos{200 * k_second});
    ASSERT_EQ(next.size(), 1U);
    EXPECT_EQ(next[0].settled_at.ns, 200 * k_second);
    EXPECT_EQ(next[0].rate.raw, 300'000);
}

TEST(FundingSchedule, ASettlementFoundLongAfterIsCountedNotBooked) {
    FundingSchedule schedule;
    schedule.on_update(update(100));
    EXPECT_TRUE(schedule.due(UnixNanos{200 * k_second}).empty());
    EXPECT_EQ(schedule.missed(), 1U);
}

TEST(FundingSchedule, ContinuousFundingAndAnUnknownTimeHaveNoInstant) {
    FundingSchedule schedule;
    auto continuous = update(100);
    continuous.accrual = FundingAccrual::Continuous;
    schedule.on_update(continuous);
    schedule.on_update(update(0));
    EXPECT_TRUE(schedule.due(UnixNanos{100 * k_second}).empty());
}

TEST(FundingFlat, TheLongsPayTheShortsAndTheAccountStaysWhole) {
    const std::array holders{FundingHolder{.strategy_id = 2, .lots = 30},
                             FundingHolder{.strategy_id = 3, .lots = -10},
                             FundingHolder{.strategy_id = 4, .lots = 0},
                             FundingHolder{.strategy_id = 5, .lots = -20}};
    const auto shares = split_funding_flat(holders, FixedE9{-301});
    ASSERT_EQ(shares.size(), 3U);
    EXPECT_EQ(shares[0].strategy_id, 2);
    EXPECT_EQ(shares[0].amount.raw, -301);
    EXPECT_EQ(shares[1].strategy_id, 3);
    EXPECT_EQ(shares[1].amount.raw, 100);
    EXPECT_EQ(shares[2].strategy_id, 5);
    EXPECT_EQ(shares[2].amount.raw, 201);
}

TEST(FundingFlat, BooksThatDoNotNetToZeroWaitForTheVenuesCharge) {
    const std::array holders{FundingHolder{.strategy_id = 2, .lots = 30}, FundingHolder{.strategy_id = 3, .lots = -10}};
    EXPECT_TRUE(split_funding_flat(holders, FixedE9{-300}).empty());
    EXPECT_TRUE(split_funding_flat({}, FixedE9{-300}).empty());
}

TEST(FundingNet, AChargeOnBooksNettingToZeroGoesToTheUnheldBookNotToThem) {
    const std::array holders{FundingHolder{.strategy_id = 2, .lots = 10}, FundingHolder{.strategy_id = 3, .lots = -10}};
    const auto shares = split_funding_net(holders, FixedE9{-55}, k_manual);
    ASSERT_EQ(shares.size(), 1U);
    EXPECT_EQ(shares[0].strategy_id, k_manual);
    EXPECT_EQ(shares[0].amount.raw, -55);
}

} // namespace
} // namespace raikiri::oms
