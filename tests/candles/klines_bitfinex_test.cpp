#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBitfinex, AsksTradeCandlesOldestFirst) {
    const auto spot = kline_venue(VenueId::Bitfinex, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto call = spot->call("tBTCUSD", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api-pub.bitfinex.com");
    EXPECT_EQ(call.target,
              "/v2/candles/trade:1m:tBTCUSD/hist?start=1794756180000&end=1794756479999&limit=10000&sort=1");
    const auto perp = kline_venue(VenueId::Bitfinex, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    EXPECT_EQ(perp->call("tBTCF0:USTF0", 29'912'603, 29'912'608).target,
              "/v2/candles/trade:1m:tBTCF0:USTF0/hist?start=1794756180000&end=1794756479999&limit=10000&sort=1");
    EXPECT_EQ(kline_venue(VenueId::Bitfinex, MarketType::Option), nullptr);
}

TEST(KlinesBitfinex, ParsesOpenCloseHighLowRows) {
    const auto venue = kline_venue(VenueId::Bitfinex, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bitfinex_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'754'000'000'000LL);
    EXPECT_EQ(c.high, 64'772'000'000'000LL);
    EXPECT_EQ(c.low, 64'658'000'000'000LL);
    EXPECT_EQ(c.close, 64'698'000'000'000LL);
    EXPECT_EQ(c.volume, 791'627'790LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse("[\"error\",10020,\"limit: invalid\"]", id).has_value());
}
