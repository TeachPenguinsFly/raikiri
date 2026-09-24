#include <raikiri/domain/balance.hpp>
#include <raikiri/domain/order_events.hpp>

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {

using namespace raikiri;

constexpr auto k_order = ClientOrderId::make(7, 42);

TEST(FillIdentity, TheVenuesOwnIdIsUsedVerbatim) {
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), "83956769");
    EXPECT_EQ(id.view(), "83956769");
}

TEST(FillIdentity, WithoutOneTheAnswerSaysSo) {
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), "");
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(id.view().front(), '~');
}

TEST(FillIdentity, AResendDerivesTheIdentityItDerivedBefore) {
    // bingx perp pushes carry no trade id, so the fallback must be deterministic
    const auto first = fill_identity(k_order, QtyLots::from_raw(18), "");
    const auto again = fill_identity(k_order, QtyLots::from_raw(18), "");
    EXPECT_EQ(first.view(), again.view());
}

TEST(FillIdentity, EveryFillOfOneOrderGetsItsOwn) {
    std::set<std::string> seen;
    for (const std::int64_t leaves : {30, 18, 11, 4, 0}) {
        seen.insert(std::string{fill_identity(k_order, QtyLots::from_raw(leaves), "").view()});
    }
    EXPECT_EQ(seen.size(), 5U);
}

TEST(FillIdentity, TwoOrdersAtTheSameLeavesDoNotCollide) {
    const auto a = fill_identity(ClientOrderId::make(7, 42), QtyLots::from_raw(0), "");
    const auto b = fill_identity(ClientOrderId::make(7, 43), QtyLots::from_raw(0), "");
    EXPECT_NE(a.view(), b.view());
}

TEST(FillIdentity, FieldsAreFixedWidthSoTheyCannotBeReadTwoWays) {
    const auto id = fill_identity(ClientOrderId{0x1A}, QtyLots::from_raw(0xB), "");
    EXPECT_EQ(id.view(), "~000000000000001a.000000000000000b");
}

TEST(FillIdentity, ANegativeLeavesStillRoundTripsDistinctly) {
    const auto negative = fill_identity(k_order, QtyLots::from_raw(-1), "");
    const auto positive = fill_identity(k_order, QtyLots::from_raw(1), "");
    EXPECT_NE(negative.view(), positive.view());
}

TEST(FillIdentity, AnOversizeVenueIdFallsBackRatherThanTruncating) {
    const std::string too_long(64, 'x');
    const auto id = fill_identity(k_order, QtyLots::from_raw(18), too_long);
    ASSERT_FALSE(id.empty());
    EXPECT_EQ(id.view().front(), '~');
}

TEST(AssetCode, IsFoldedSoOneHoldingIsNotCountedTwice) {
    ASSERT_TRUE(to_asset_code("usdt").has_value());
    EXPECT_EQ(to_asset_code("usdt")->view(), "USDT");
    EXPECT_EQ(to_asset_code("USDT")->view(), "USDT");
    EXPECT_EQ(to_asset_code("uSdT")->view(), "USDT");
    EXPECT_EQ(to_asset_code("1000pepe")->view(), "1000PEPE");
}

TEST(AssetCode, RefusesWhatWouldBeANumberWithNoUnit) {
    EXPECT_FALSE(to_asset_code("").has_value());
    // bingx lists the 14-char NCCOCOFFEE2USD
    EXPECT_TRUE(to_asset_code("NCCOCOFFEE2USD").has_value());
    EXPECT_FALSE(to_asset_code("TWENTYONECHARACTERSXX").has_value());
}

} // namespace
