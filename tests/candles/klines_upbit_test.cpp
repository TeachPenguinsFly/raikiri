#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesUpbit, AsksMinuteCandlesBeforeAnUtcEnd) {
    const auto venue = kline_venue(VenueId::Upbit, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("KRW-BTC", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.upbit.com");
    EXPECT_EQ(call.target, "/v1/candles/minutes/1?market=KRW-BTC&to=2026-11-15T15:28:00Z&count=5");
    EXPECT_EQ(kline_venue(VenueId::Upbit, MarketType::Perp), nullptr);
}

TEST(KlinesUpbit, ParsesNewestFirstRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Upbit, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("upbit_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 89'277'000'000'000'000LL);
    EXPECT_EQ(c.high, 89'285'000'000'000'000LL);
    EXPECT_EQ(c.low, 89'175'000'000'000'000LL);
    EXPECT_EQ(c.close, 89'175'000'000'000'000LL);
    EXPECT_EQ(c.volume, 1'485'932'930LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'607);
    EXPECT_FALSE(venue->parse("{\"error\":{\"name\":400,\"message\":\"Invalid parameter.\"}}", id).has_value());
}
