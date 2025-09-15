#include <raikiri/oms/funding_split.hpp>

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

namespace raikiri::oms {
namespace {

constexpr std::uint16_t k_manual = 0;

std::int64_t total(const std::vector<FundingShare>& shares) {
    return std::accumulate(
        shares.begin(), shares.end(), std::int64_t{0}, [](std::int64_t sum, const FundingShare& s) {
            return sum + s.amount.raw;
        });
}

TEST(FundingSplit, TheSharesSumToTheChargeExactly) {
    // The invariant the whole function exists for: rounding each share on its
    // own loses units into nobody's book, and the ledger then disagrees with
    // the venue's balance forever.
    const std::vector<FundingHolder> holders{{.strategy_id = 1, .lots = 1},
                                             {.strategy_id = 2, .lots = 1},
                                             {.strategy_id = 3, .lots = 1}};
    const auto shares = split_funding(holders, FixedE9::from_raw(-100), k_manual);

    ASSERT_EQ(shares.size(), 3U);
    EXPECT_EQ(total(shares), -100) << "three equal shares of a hundred do not divide";
    // Two get the rounded share; the last carries the remainder.
    EXPECT_EQ(shares[0].amount.raw, -33);
    EXPECT_EQ(shares[1].amount.raw, -33);
    EXPECT_EQ(shares[2].amount.raw, -34);
}

TEST(FundingSplit, ProRatedByTheSizeEachCarries) {
    const std::vector<FundingHolder> holders{{.strategy_id = 7, .lots = 30},
                                             {.strategy_id = 9, .lots = 10}};
    const auto shares = split_funding(holders, FixedE9::from_raw(-400), k_manual);

    ASSERT_EQ(shares.size(), 2U);
    EXPECT_EQ(shares[0].amount.raw, -300);
    EXPECT_EQ(shares[1].amount.raw, -100);
    EXPECT_EQ(total(shares), -400);
}

TEST(FundingSplit, AShortIsChargedExactlyAsALongIs) {
    // The magnitude, not the sign: funding is paid on the size carried, and a
    // negative lot count is a direction, not a smaller position.
    const std::vector<FundingHolder> holders{{.strategy_id = 1, .lots = -10},
                                             {.strategy_id = 2, .lots = 10}};
    const auto shares = split_funding(holders, FixedE9::from_raw(-200), k_manual);

    ASSERT_EQ(shares.size(), 2U);
    EXPECT_EQ(shares[0].amount.raw, -100);
    EXPECT_EQ(shares[1].amount.raw, -100);
}

TEST(FundingSplit, AChargeNobodyIsHoldingLandsOnTheManualBookRatherThanBeingDropped) {
    // Dropping it would put the ledger permanently at odds with the venue's
    // balance: the money moved whether or not a strategy claims it.
    const std::vector<FundingHolder> holders{{.strategy_id = 4, .lots = 0}};
    const auto shares = split_funding(holders, FixedE9::from_raw(-55), k_manual);

    ASSERT_EQ(shares.size(), 1U);
    EXPECT_EQ(shares[0].strategy_id, k_manual);
    EXPECT_EQ(shares[0].amount.raw, -55);
}

TEST(FundingSplit, NoHoldersAtAllIsTheSameAnswer) {
    const auto shares = split_funding({}, FixedE9::from_raw(-55), k_manual);
    ASSERT_EQ(shares.size(), 1U);
    EXPECT_EQ(shares[0].strategy_id, k_manual);
}

TEST(FundingSplit, AFlatStrategyIsNotGivenAShareOfNothing) {
    const std::vector<FundingHolder> holders{{.strategy_id = 1, .lots = 5},
                                             {.strategy_id = 2, .lots = 0},
                                             {.strategy_id = 3, .lots = 5}};
    const auto shares = split_funding(holders, FixedE9::from_raw(-100), k_manual);

    ASSERT_EQ(shares.size(), 2U) << "a flat book carried none of it";
    EXPECT_EQ(shares[0].strategy_id, 1);
    EXPECT_EQ(shares[1].strategy_id, 3);
    EXPECT_EQ(total(shares), -100);
}

TEST(FundingSplit, NothingToBookIsNoRowsAtAll) {
    const std::vector<FundingHolder> holders{{.strategy_id = 1, .lots = 5}};
    EXPECT_TRUE(split_funding(holders, FixedE9::from_raw(0), k_manual).empty());
    EXPECT_TRUE(split_funding(holders, FixedE9{}, k_manual).empty())
        << "an undefined charge is not a zero one, and neither is bookable";
}

TEST(FundingSplit, FundingReceivedSplitsTheSameWay) {
    // A positive charge is funding the account was PAID. Nothing about the
    // arithmetic changes, and the remainder still lands whole.
    const std::vector<FundingHolder> holders{{.strategy_id = 1, .lots = 1},
                                             {.strategy_id = 2, .lots = 2}};
    const auto shares = split_funding(holders, FixedE9::from_raw(10), k_manual);

    ASSERT_EQ(shares.size(), 2U);
    EXPECT_EQ(shares[0].amount.raw, 3);
    EXPECT_EQ(shares[1].amount.raw, 7);
    EXPECT_EQ(total(shares), 10);
}

} // namespace
} // namespace raikiri::oms
