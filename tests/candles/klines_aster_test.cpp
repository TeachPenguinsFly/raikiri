#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesAster, AsksTheFapiAndTheSpotApiForOneMinuteBars) {
    const auto venue = kline_venue(VenueId::Aster, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "fapi.asterdex.com");
    EXPECT_EQ(call.target,
              "/fapi/v1/klines?symbol=BTCUSDT&interval=1m&startTime=1794765180000&endTime=1794765419999&limit=1500");
    const auto spot = kline_venue(VenueId::Aster, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    const auto spot_call = spot->call("BTCUSDT", 29'912'753, 29'912'757);
    EXPECT_EQ(spot_call.host, "sapi.asterdex.com");
    EXPECT_EQ(spot_call.target,
              "/api/v1/klines?symbol=BTCUSDT&interval=1m&startTime=1794765180000&endTime=1794765419999&limit=1000");
    EXPECT_EQ(kline_venue(VenueId::Aster, MarketType::Option), nullptr);
}

TEST(KlinesAster, ParsesRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Aster, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("aster_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'527'300'000'000LL);
    EXPECT_EQ(c.high, 64'543'200'000'000LL);
    EXPECT_EQ(c.low, 64'524'000'000'000LL);
    EXPECT_EQ(c.close, 64'543'200'000'000LL);
    EXPECT_EQ(c.volume, 3'339'000'000LL);
    EXPECT_EQ(c.trades, 18);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse(R"({"code":-1121,"msg":"Invalid symbol."})", id).has_value());
}
