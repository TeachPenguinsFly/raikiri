#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBinance, PerpAsksTheFapiForOneMinuteBars) {
    const auto venue = kline_venue(VenueId::Binance, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'760, 29'912'765);
    EXPECT_EQ(call.host, "fapi.binance.com");
    EXPECT_EQ(call.target,
              "/fapi/v1/klines?symbol=BTCUSDT&interval=1m&startTime=1794765600000&endTime=1794765899999&limit=1500");
    EXPECT_EQ(kline_venue(VenueId::Binance, MarketType::Option), nullptr);
}

TEST(KlinesBinance, ParsesRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Binance, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("binance_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 3U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'760);
    EXPECT_EQ(c.open, 64'627'200'000'000LL);
    EXPECT_EQ(c.high, 64'640'000'000'000LL);
    EXPECT_EQ(c.low, 64'590'900'000'000LL);
    EXPECT_EQ(c.close, 64'630'600'000'000LL);
    EXPECT_EQ(c.volume, 59'822'000'000LL);
    EXPECT_EQ(c.trades, 2360);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse("{\"code\":-1121,\"msg\":\"Invalid symbol.\"}", id).has_value());
}
