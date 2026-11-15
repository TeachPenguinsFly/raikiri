#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesXt, AsksBarsClosingByTheEnd) {
    const auto perp = kline_venue(VenueId::Xt, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("btc_usdt", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "fapi.xt.com");
    EXPECT_EQ(call.target,
              "/future/market/v1/public/q/kline?symbol=btc_usdt&interval=1m&startTime=1794758400000"
              "&endTime=1794758700000&limit=5");
    const auto spot = kline_venue(VenueId::Xt, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto spot_call = spot->call("btc_usdt", 29'912'640, 29'912'645);
    EXPECT_EQ(spot_call.host, "sapi.xt.com");
    EXPECT_EQ(spot_call.target,
              "/v4/public/kline?symbol=btc_usdt&interval=1m&startTime=1794758400000&endTime=1794758700000&limit=5");
    EXPECT_EQ(kline_venue(VenueId::Xt, MarketType::Option), nullptr);
}

TEST(KlinesXt, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Xt, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Xt, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("xt_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'714'500'000'000LL);
    EXPECT_EQ(c.high, 64'714'700'000'000LL);
    EXPECT_EQ(c.low, 64'666'900'000'000LL);
    EXPECT_EQ(c.close, 64'699'700'000'000LL);
    EXPECT_EQ(c.volume, 55'967'000'000'000LL); // contracts
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    EXPECT_FALSE(venue
                     ->parse(R"({"returnCode":1,"msgInfo":"failure","error":{"code":"invalid_symbol",)"
                             R"("msg":"invalid symbol","args":[]},"result":null})",
                             id)
                     .has_value());
}

TEST(KlinesXt, ParsesSpotRowsInTheBaseAsset) {
    const auto venue = kline_venue(VenueId::Xt, MarketType::Spot);
    const auto id = InstrumentId::make(VenueId::Xt, MarketType::Spot, SymbolId{1});
    const auto got = venue->parse(
        R"({"rc":0,"mc":"SUCCESS","ma":[],"result":[{"t":1794758400000,"o":"64748.30",)"
        R"("c":"64727.50","h":"64748.30","l":"64700.00","q":"2.64435","v":"171143.6513229"}]})",
        id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 1U);
    EXPECT_EQ(got->front().minute, 29'912'640);
    EXPECT_EQ(got->front().close, 64'727'500'000'000LL);
    EXPECT_EQ(got->front().volume, 2'644'350'000LL);
}
