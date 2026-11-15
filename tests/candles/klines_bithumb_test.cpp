#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBithumb, AsksMinuteCandlesBeforeAKoreanEnd) {
    const auto venue = kline_venue(VenueId::Bithumb, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("KRW-BTC", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.bithumb.com");
    EXPECT_EQ(call.target, "/v1/candles/minutes/1?market=KRW-BTC&to=2026-11-16T00:28:00&count=5");
    EXPECT_EQ(kline_venue(VenueId::Bithumb, MarketType::Perp), nullptr);
}

TEST(KlinesBithumb, ParsesUtcStampsOldestFirst) {
    const auto venue = kline_venue(VenueId::Bithumb, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bithumb_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 89'251'000'000'000'000LL);
    EXPECT_EQ(c.high, 89'268'000'000'000'000LL);
    EXPECT_EQ(c.low, 89'125'000'000'000'000LL);
    EXPECT_EQ(c.close, 89'172'000'000'000'000LL);
    EXPECT_EQ(c.volume, 262'211'100LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'607);
    EXPECT_FALSE(
        venue->parse("{\"error\":{\"name\":400,\"message\":\"Invalid parameter. Check the given value!\"}}", id)
            .has_value());
}
