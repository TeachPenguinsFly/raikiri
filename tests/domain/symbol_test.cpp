#include <raikiri/domain/symbol.hpp>

#include <gtest/gtest.h>

namespace {

using namespace raikiri;

TEST(CanonicalSymbol, TheFourSpellingsOfOneMarketBecomeOneName) {
    EXPECT_EQ(canonical_symbol("BTC", "USDT")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("btc", "usdt")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("XBT", "USDT")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("xbt", "USDT")->view(), "BTC/USDT");
}

TEST(CanonicalSymbol, TheSeparatorIsWhatKeepsTwoMarketsApart) {
    EXPECT_NE(canonical_symbol("BTC", "USDT")->view(), canonical_symbol("BTCU", "SDT")->view());
}

TEST(CanonicalSymbol, AMultiplierIsNotAnAlias) {
    EXPECT_EQ(canonical_symbol("1000PEPE", "USDT")->view(), "1000PEPE/USDT");
    EXPECT_NE(canonical_symbol("1000PEPE", "USDT")->view(),
              canonical_symbol("PEPE", "USDT")->view());
}

TEST(CanonicalSymbol, AHalfNamedMarketIsRefused) {
    EXPECT_FALSE(canonical_symbol("BTC", "").has_value());
    EXPECT_FALSE(canonical_symbol("", "USDT").has_value());
}

TEST(SplitSymbol, ReadsThePairOutOfTheVenuesOwnSpelling) {
    const auto gate = split_symbol("BTC_USDT", '_');
    ASSERT_TRUE(gate.has_value());
    EXPECT_EQ(gate->first.view(), "BTC");
    EXPECT_EQ(gate->second.view(), "USDT");

    const auto bingx = split_symbol("BTC-USDT", '-');
    ASSERT_TRUE(bingx.has_value());
    EXPECT_EQ(bingx->second.view(), "USDT");

    const auto xt = split_symbol("xbt_usdt", '_');
    ASSERT_TRUE(xt.has_value());
    EXPECT_EQ(xt->first.view(), "BTC");
}

TEST(SplitSymbol, RefusesASymbolCarryingMoreThanThePair) {
    EXPECT_FALSE(split_symbol("BTC-USDT-SWAP", '-').has_value());
    EXPECT_FALSE(split_symbol("BTCUSDT", '_').has_value());
}

TEST(CanonicalSymbol, MatchesTheFormOneAdapterAlreadyProduced) {
    EXPECT_EQ(canonical_symbol("HYPE", "USDC")->view(), "HYPE/USDC");
}

} // namespace
