// The form that makes one market one row.
//
// Every test here is about the boundary between "the same thing spelled two
// ways" and "two things that look alike". Getting the first wrong loses a venue
// from a comparison; getting the second wrong invents an arbitrage.

#include <raikiri/domain/symbol.hpp>

#include <gtest/gtest.h>

namespace {

using namespace raikiri;

TEST(CanonicalSymbol, TheFourSpellingsOfOneMarketBecomeOneName) {
    // What bybit, gate, kucoin and okx respectively call the same perpetual.
    EXPECT_EQ(canonical_symbol("BTC", "USDT")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("btc", "usdt")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("XBT", "USDT")->view(), "BTC/USDT");
    EXPECT_EQ(canonical_symbol("xbt", "USDT")->view(), "BTC/USDT");
}

TEST(CanonicalSymbol, TheSeparatorIsWhatKeepsTwoMarketsApart) {
    // Concatenated, these two are the same eight characters. The slash is not
    // decoration: it is the only thing that says where the base ends.
    EXPECT_NE(canonical_symbol("BTC", "USDT")->view(), canonical_symbol("BTCU", "SDT")->view());
}

TEST(CanonicalSymbol, AMultiplierIsNotAnAlias) {
    // A contract on a thousand of a coin is not that coin. Folding them makes a
    // thousandfold price gap read as an arbitrage, which is the one failure
    // this whole form exists to avoid.
    EXPECT_EQ(canonical_symbol("1000PEPE", "USDT")->view(), "1000PEPE/USDT");
    EXPECT_NE(canonical_symbol("1000PEPE", "USDT")->view(),
              canonical_symbol("PEPE", "USDT")->view());
}

TEST(CanonicalSymbol, AHalfNamedMarketIsRefused) {
    // A price with no unit. Guessing the quote would group two markets that are
    // not the same one.
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

    // Lowercase and aliases travel through the same door.
    const auto xt = split_symbol("xbt_usdt", '_');
    ASSERT_TRUE(xt.has_value());
    EXPECT_EQ(xt->first.view(), "BTC");
}

TEST(SplitSymbol, RefusesASymbolCarryingMoreThanThePair) {
    // "BTC-USDT-SWAP" must not yield a quote currency of "USDT-SWAP". Which
    // part the trailing word is belongs to the venue, which strips it first.
    EXPECT_FALSE(split_symbol("BTC-USDT-SWAP", '-').has_value());
    EXPECT_FALSE(split_symbol("BTCUSDT", '_').has_value());
}

TEST(CanonicalSymbol, MatchesTheFormOneAdapterAlreadyProduced) {
    // hyperliquid has been emitting this shape since before there was a
    // function for it. The form is not new; what is new is that every venue
    // reduces to it.
    EXPECT_EQ(canonical_symbol("HYPE", "USDC")->view(), "HYPE/USDC");
}

} // namespace
