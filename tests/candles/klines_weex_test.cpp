#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesWeex, AsksTheHistoryKlines) {
    const auto perp = kline_venue(VenueId::Weex, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTCUSDT", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "api-contract.weex.com");
    EXPECT_EQ(call.target,
              "/capi/v3/market/historyKlines?symbol=BTCUSDT&interval=1m&startTime=1794758400000"
              "&endTime=1794758699999&limit=100");
    const auto spot = kline_venue(VenueId::Weex, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto spot_call = spot->call("BTCUSDT", 29'912'640, 29'912'645);
    EXPECT_EQ(spot_call.host, "api-spot.weex.com");
    EXPECT_EQ(spot_call.target,
              "/api/v3/market/historyKlines?symbol=BTCUSDT&interval=1m&startTime=1794758400000"
              "&endTime=1794758699999&limit=100");
    EXPECT_EQ(kline_venue(VenueId::Weex, MarketType::Option), nullptr);
}

TEST(KlinesWeex, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Weex, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Weex, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("weex_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'719'800'000'000LL);
    EXPECT_EQ(c.high, 64'719'800'000'000LL);
    EXPECT_EQ(c.low, 64'666'800'000'000LL);
    EXPECT_EQ(c.close, 64'689'300'000'000LL);
    EXPECT_EQ(c.volume, 26'348'200'000LL);
    EXPECT_EQ(c.trades, 324);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    EXPECT_FALSE(venue->parse(R"({"code":-1142,"msg":"Parameter 'symbol' is invalid."})", id).has_value());
}
