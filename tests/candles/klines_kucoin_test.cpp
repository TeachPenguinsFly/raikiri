#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesKucoin, AsksTheFuturesHostInMsAndSpotInSeconds) {
    const auto perp = kline_venue(VenueId::Kucoin, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("XBTUSDTM", 29'912'600, 29'912'604);
    EXPECT_EQ(call.host, "api-futures.kucoin.com");
    EXPECT_EQ(call.target, "/api/v1/kline/query?symbol=XBTUSDTM&granularity=1&from=1794756000000&to=1794756239999");
    const auto spot = kline_venue(VenueId::Kucoin, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto spot_call = spot->call("BTC-USDT", 29'912'600, 29'912'604);
    EXPECT_EQ(spot_call.host, "api.kucoin.com");
    EXPECT_EQ(spot_call.target, "/api/v1/market/candles?type=1min&symbol=BTC-USDT&startAt=1794756000&endAt=1794756239");
    EXPECT_EQ(kline_venue(VenueId::Kucoin, MarketType::Future), nullptr);
}

TEST(KlinesKucoin, ParsesFuturesRowsInLots) {
    const auto venue = kline_venue(VenueId::Kucoin, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Kucoin, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("kucoin_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'737'300'000'000LL);
    EXPECT_EQ(c.high, 64'737'300'000'000LL);
    EXPECT_EQ(c.low, 64'707'000'000'000LL);
    EXPECT_EQ(c.close, 64'720'500'000'000LL);
    EXPECT_EQ(c.volume, 1'567'000'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'603);
    EXPECT_FALSE(venue->parse(R"({"msg":"Unsupported trading pair.","code":"400100"})", id).has_value());
}

TEST(KlinesKucoin, ParsesSpotRowsOldestFirstWithTheVolumeCutAtE9) {
    const auto venue = kline_venue(VenueId::Kucoin, MarketType::Spot);
    const auto id = InstrumentId::make(VenueId::Kucoin, MarketType::Spot, SymbolId{1});
    const auto got = venue->parse(candle_fixture("kucoin_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'791'300'000'000LL);
    EXPECT_EQ(c.high, 64'791'300'000'000LL);
    EXPECT_EQ(c.low, 64'753'200'000'000LL);
    EXPECT_EQ(c.close, 64'771'900'000'000LL);
    // 0.43256722593653905943 as sent
    EXPECT_EQ(c.volume, 432'567'225LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(got->back().minute, 29'912'603);
}
