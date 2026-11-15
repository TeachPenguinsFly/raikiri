#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBitstamp, AsksAStartAndACount) {
    const auto spot = kline_venue(VenueId::Bitstamp, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto call = spot->call("btcusd", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "www.bitstamp.net");
    EXPECT_EQ(call.target, "/api/v2/ohlc/btcusd/?step=60&limit=5&start=1794756180");
    const auto perp = kline_venue(VenueId::Bitstamp, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->call("btcusd-perp", 29'912'603, 29'912'608).target,
              "/api/v2/ohlc/btcusd-perp/?step=60&limit=5&start=1794756180");
    EXPECT_EQ(kline_venue(VenueId::Bitstamp, MarketType::Option), nullptr);
}

TEST(KlinesBitstamp, ParsesOhlcObjects) {
    const auto venue = kline_venue(VenueId::Bitstamp, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bitstamp_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'676'990'000'000LL);
    EXPECT_EQ(c.high, 64'690'060'000'000LL);
    EXPECT_EQ(c.low, 64'584'160'000'000LL);
    EXPECT_EQ(c.close, 64'621'480'000'000LL);
    EXPECT_EQ(c.volume, 112'523'380LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse("{\"code\":\"404\",\"message\":\"Not found\"}", id).has_value());
}
