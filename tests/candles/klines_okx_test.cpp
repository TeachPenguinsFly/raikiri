#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesOkx, AsksHistoryCandlesBetweenExclusiveBounds) {
    const auto venue = kline_venue(VenueId::Okx, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USDT-SWAP", 29'912'600, 29'912'604);
    EXPECT_EQ(call.host, "www.okx.com");
    EXPECT_EQ(call.target,
              "/api/v5/market/history-candles?instId=BTC-USDT-SWAP&bar=1m&after=1794756240000&before=1794755999999"
              "&limit=100");
    EXPECT_NE(kline_venue(VenueId::Okx, MarketType::Spot), nullptr);
    EXPECT_EQ(kline_venue(VenueId::Okx, MarketType::Future), nullptr);
    EXPECT_EQ(kline_venue(VenueId::Okx, MarketType::Option), nullptr);
}

TEST(KlinesOkx, ParsesConfirmedRowsOldestFirstInContracts) {
    const auto venue = kline_venue(VenueId::Okx, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Okx, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("okx_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'753'900'000'000LL);
    EXPECT_EQ(c.high, 64'753'900'000'000LL);
    EXPECT_EQ(c.low, 64'713'600'000'000LL);
    EXPECT_EQ(c.close, 64'734'000'000'000LL);
    EXPECT_EQ(c.volume, 4'052'460'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'603);

    const auto open = venue->parse(
        R"({"code":"0","data":[["1794756240000","1","1","1","1","1","1","1","0"]],"msg":""})", id);
    ASSERT_TRUE(open.has_value());
    EXPECT_TRUE(open->empty());
    EXPECT_FALSE(venue
                     ->parse(R"({"code":"51001","data":[],)"
                             R"("msg":"Instrument ID, Instrument ID code, or Spread ID doesn't exist."})",
                             id)
                     .has_value());
}
