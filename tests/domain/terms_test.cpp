#include <raikiri/domain/terms.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri {
namespace {

// binance's published ETHUSDT brackets
const std::vector<MaintenanceTier> k_tiers{
    {.floor = 0, .cap = 10'000, .rate = 0.0065, .amount = 0, .max_leverage = 75},
    {.floor = 10'000, .cap = 50'000, .rate = 0.01, .amount = 35, .max_leverage = 50},
    {.floor = 50'000, .cap = 250'000, .rate = 0.02, .amount = 535, .max_leverage = 25},
};

TEST(MaintenanceMargin, IsTheRateOfTheTierLessItsAmount) {
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, 5'000), 32.5);
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, 20'000), 165.0);
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, 100'000), 1'465.0);
}

TEST(MaintenanceMargin, IsContinuousAcrossAFloor) {
    EXPECT_NEAR(*maintenance_margin(k_tiers, 9'999.999999), *maintenance_margin(k_tiers, 10'000), 1e-6);
    EXPECT_NEAR(*maintenance_margin(k_tiers, 49'999.999999), *maintenance_margin(k_tiers, 50'000), 1e-6);
}

TEST(MaintenanceMargin, AShortCountsAsItsSizeAndTheLastTierHasNoCeiling) {
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, -20'000), 165.0);
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, 1'000'000), 1'000'000 * 0.02 - 535);
    EXPECT_DOUBLE_EQ(*maintenance_margin(k_tiers, 0), 0.0);
}

TEST(MaintenanceMargin, NoTiersIsNoAnswer) {
    EXPECT_FALSE(maintenance_margin({}, 1'000).has_value());
    EXPECT_EQ(tier_for({}, 1'000), nullptr);
}

TEST(MaintenanceMargin, AmountsFilledFromRatesKeepItContinuous) {
    std::vector<MaintenanceTier> tiers{
        {.floor = 0, .cap = 150'000'000, .rate = 0.0125, .amount = 0, .max_leverage = 40},
        {.floor = 150'000'000, .cap = 0, .rate = 0.025, .amount = 0, .max_leverage = 20},
    };
    fill_maintenance_amounts(tiers);
    EXPECT_DOUBLE_EQ(tiers[0].amount, 0.0);
    EXPECT_DOUBLE_EQ(tiers[1].amount, 1'875'000.0);
    EXPECT_DOUBLE_EQ(*maintenance_margin(tiers, 150'000'000), 1'875'000.0);
}

} // namespace
} // namespace raikiri
