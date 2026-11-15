#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesCoinbase, SpotAsksTheExchangeForOneMinuteBars) {
    const auto venue = kline_venue(VenueId::Coinbase, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USD", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.exchange.coinbase.com");
    EXPECT_EQ(call.target, "/products/BTC-USD/candles?granularity=60&start=1794756180&end=1794756420");
    EXPECT_EQ(kline_venue(VenueId::Coinbase, MarketType::Perp), nullptr);
}

TEST(KlinesCoinbase, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Coinbase, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("coinbase_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'681'060'000'000LL);
    EXPECT_EQ(c.high, 64'703'670'000'000LL);
    EXPECT_EQ(c.low, 64'578'100'000'000LL);
    EXPECT_EQ(c.close, 64'631'840'000'000LL);
    EXPECT_EQ(c.volume, 16'313'861'710LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'607);
    EXPECT_FALSE(venue->parse("{\"message\":\"NotFound\"}", id).has_value());
}
