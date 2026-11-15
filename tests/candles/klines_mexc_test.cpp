#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesMexc, AsksTheContractAndSpotKlines) {
    const auto perp = kline_venue(VenueId::Mexc, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTC_USDT", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "contract.mexc.com");
    EXPECT_EQ(call.target, "/api/v1/contract/kline/BTC_USDT?interval=Min1&start=1794758400&end=1794758640");
    EXPECT_TRUE(call.body.empty());
    const auto spot = kline_venue(VenueId::Mexc, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto spot_call = spot->call("BTCUSDT", 29'912'640, 29'912'645);
    EXPECT_EQ(spot_call.host, "api.mexc.com");
    EXPECT_EQ(spot_call.target,
              "/api/v3/klines?symbol=BTCUSDT&interval=1m&startTime=1794758400000&endTime=1794758699999&limit=1000");
    EXPECT_EQ(kline_venue(VenueId::Mexc, MarketType::Option), nullptr);
}

TEST(KlinesMexc, ParsesPerpColumnsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Mexc, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Mexc, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("mexc_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'720'000'000'000LL);
    EXPECT_EQ(c.high, 64'720'000'000'000LL);
    EXPECT_EQ(c.low, 64'666'900'000'000LL);
    EXPECT_EQ(c.close, 64'689'300'000'000LL);
    EXPECT_EQ(c.volume, 78'646'000'000'000LL); // contracts
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    EXPECT_FALSE(venue->parse(R"({"success":false,"code":1001,"message":"Contract does not exist"})", id).has_value());
}

TEST(KlinesMexc, ParsesSpotRowsInTheBaseAsset) {
    const auto venue = kline_venue(VenueId::Mexc, MarketType::Spot);
    const auto id = InstrumentId::make(VenueId::Mexc, MarketType::Spot, SymbolId{1});
    const auto got = venue->parse(
        R"([[1794758400000,"64756.51","64778.28","64702.65","64731.32","1.32118633",1794758460000,"85516.31"]])", id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 1U);
    EXPECT_EQ(got->front().minute, 29'912'640);
    EXPECT_EQ(got->front().close, 64'731'320'000'000LL);
    EXPECT_EQ(got->front().volume, 1'321'186'330LL);
    EXPECT_FALSE(venue->parse(R"({"msg":"Invalid symbol.","code":-1121,"_extend":null})", id).has_value());
}
