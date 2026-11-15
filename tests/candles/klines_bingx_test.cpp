#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBingx, AsksTheSwapAndSpotKlines) {
    const auto perp = kline_venue(VenueId::Bingx, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTC-USDT", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "open-api.bingx.com");
    EXPECT_EQ(call.target,
              "/openApi/swap/v3/quote/klines?symbol=BTC-USDT&interval=1m&startTime=1794758400000"
              "&endTime=1794758699999&limit=1440");
    const auto spot = kline_venue(VenueId::Bingx, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->call("BTC-USDT", 29'912'640, 29'912'645).target,
              "/openApi/spot/v2/market/kline?symbol=BTC-USDT&interval=1m&startTime=1794758400000"
              "&endTime=1794758699999&limit=1440");
    EXPECT_EQ(kline_venue(VenueId::Bingx, MarketType::Option), nullptr);
}

TEST(KlinesBingx, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Bingx, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Bingx, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("bingx_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'719'600'000'000LL);
    EXPECT_EQ(c.high, 64'719'600'000'000LL);
    EXPECT_EQ(c.low, 64'665'300'000'000LL);
    EXPECT_EQ(c.close, 64'688'600'000'000LL);
    EXPECT_EQ(c.volume, 909'200'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    EXPECT_FALSE(venue
                     ->parse(R"({"code":109425,"msg":"NOPE-USDT not exist, please verify it in api: )"
                             R"(/openApi/swap/v2/quote/contracts","data":{}})",
                             id)
                     .has_value());
}

TEST(KlinesBingx, ParsesSpotRows) {
    const auto venue = kline_venue(VenueId::Bingx, MarketType::Spot);
    const auto id = InstrumentId::make(VenueId::Bingx, MarketType::Spot, SymbolId{1});
    const auto got = venue->parse(
        R"({"code":0,"timestamp":1794767041190,"data":[)"
        R"([1794758460000,64727.48,64727.48,64680.59,64713.59,1.235361,1794758519999,79926.09],)"
        R"([1794758400000,64748.29,64753.37,64700.04,64727.48,3.896961,1794758459999,252195.27]]})",
        id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 2U);
    EXPECT_EQ(got->front().minute, 29'912'640);
    EXPECT_EQ(got->front().open, 64'748'290'000'000LL);
    EXPECT_EQ(got->front().volume, 3'896'961'000LL);
    EXPECT_EQ(got->back().minute, 29'912'641);
}
