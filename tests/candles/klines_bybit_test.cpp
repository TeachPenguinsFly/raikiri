#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBybit, AsksTheLinearOrSpotCategory) {
    const auto perp = kline_venue(VenueId::Bybit, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTCUSDT", 29'912'600, 29'912'604);
    EXPECT_EQ(call.host, "api.bybit.com");
    EXPECT_EQ(call.target,
              "/v5/market/kline?category=linear&symbol=BTCUSDT&interval=1&start=1794756000000&end=1794756239999"
              "&limit=1000");
    const auto spot = kline_venue(VenueId::Bybit, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->call("BTCUSDT", 29'912'600, 29'912'604).target,
              "/v5/market/kline?category=spot&symbol=BTCUSDT&interval=1&start=1794756000000&end=1794756239999"
              "&limit=1000");
    EXPECT_EQ(kline_venue(VenueId::Bybit, MarketType::Option), nullptr);
}

TEST(KlinesBybit, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Bybit, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("bybit_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'757'800'000'000LL);
    EXPECT_EQ(c.high, 64'757'900'000'000LL);
    EXPECT_EQ(c.low, 64'709'800'000'000LL);
    EXPECT_EQ(c.close, 64'734'300'000'000LL);
    EXPECT_EQ(c.volume, 14'757'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'603);
    EXPECT_FALSE(venue
                     ->parse(R"({"retCode":10001,"retMsg":"params error: Symbol Is Invalid","result":{},)"
                             R"("retExtInfo":{},"time":1794767029458})",
                             id)
                     .has_value());
}
