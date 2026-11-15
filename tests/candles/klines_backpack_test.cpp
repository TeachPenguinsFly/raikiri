#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBackpack, AsksKlinesInSeconds) {
    const auto venue = kline_venue(VenueId::Backpack, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC_USDC_PERP", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.backpack.exchange");
    EXPECT_EQ(call.target, "/api/v1/klines?symbol=BTC_USDC_PERP&interval=1m&startTime=1794756180&endTime=1794756480");
    EXPECT_NE(kline_venue(VenueId::Backpack, MarketType::Spot), nullptr);
    EXPECT_EQ(kline_venue(VenueId::Backpack, MarketType::Option), nullptr);
}

TEST(KlinesBackpack, ParsesStartStampsAndTrades) {
    const auto venue = kline_venue(VenueId::Backpack, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("backpack_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'668'200'000'000LL);
    EXPECT_EQ(c.high, 64'668'200'000'000LL);
    EXPECT_EQ(c.low, 64'572'300'000'000LL);
    EXPECT_EQ(c.close, 64'609'100'000'000LL);
    EXPECT_EQ(c.volume, 175'870'000LL);
    EXPECT_EQ(c.trades, 14);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(
        venue->parse("{\"code\":\"INVALID_CLIENT_REQUEST\",\"message\":\"Time range is too long\"}", id).has_value());
}
