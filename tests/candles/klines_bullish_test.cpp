#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBullish, AsksForOneMinuteCandlesBetweenInclusiveDatetimes) {
    const auto venue = kline_venue(VenueId::Bullish, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USDC-PERP", 29'912'750, 29'912'755);
    EXPECT_EQ(call.host, "api.exchange.bullish.com");
    EXPECT_EQ(call.target,
              "/trading-api/v1/markets/BTC-USDC-PERP/candle?createdAtDatetime%5Bgte%5D=2026-11-15T17:50:00.000Z"
              "&createdAtDatetime%5Blte%5D=2026-11-15T17:54:59.000Z&timeBucket=1m&_pageSize=100");
    const auto spot = kline_venue(VenueId::Bullish, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_TRUE(spot->call("BTCUSDC", 29'912'750, 29'912'751).target.starts_with("/trading-api/v1/markets/BTCUSDC/"));
    EXPECT_EQ(kline_venue(VenueId::Bullish, MarketType::Option), nullptr);
}

TEST(KlinesBullish, ParsesNewestFirstRowsIntoVenueMinutesOldestFirst) {
    const auto venue = kline_venue(VenueId::Bullish, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bullish_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 3U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'751);
    EXPECT_EQ(c.open, 64'474'000'000'000LL);
    EXPECT_EQ(c.high, 64'517'352'400'000LL);
    EXPECT_EQ(c.low, 64'474'000'000'000LL);
    EXPECT_EQ(c.close, 64'517'352'400'000LL);
    EXPECT_EQ(c.volume, 1'301'804'130LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'753);
    EXPECT_FALSE(venue->parse(R"({"message":"HTTP_INVALID_PARAMETER: Start should be before end"})", id).has_value());
}
