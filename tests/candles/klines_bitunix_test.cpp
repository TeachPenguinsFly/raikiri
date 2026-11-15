#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBitunix, PerpAsksTheFuturesKlinesBetweenBothBounds) {
    const auto venue = kline_venue(VenueId::Bitunix, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "fapi.bitunix.com");
    EXPECT_EQ(call.target,
              "/api/v1/futures/market/kline?symbol=BTCUSDT&interval=1m&startTime=1794765180000&endTime=1794765420000"
              "&limit=200");
    EXPECT_EQ(kline_venue(VenueId::Bitunix, MarketType::Option), nullptr);
}

TEST(KlinesBitunix, SpotPagesBackFromTheEndForTheStretch) {
    const auto venue = kline_venue(VenueId::Bitunix, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "openapi.bitunix.com");
    EXPECT_EQ(call.target, "/api/spot/v1/market/kline/history?symbol=BTCUSDT&interval=1&endTime=1794765420&limit=4");
}

TEST(KlinesBitunix, ParsesPerpRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Bitunix, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bitunix_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'521'900'000'000LL);
    EXPECT_EQ(c.high, 64'545'700'000'000LL);
    EXPECT_EQ(c.low, 64'508'100'000'000LL);
    EXPECT_EQ(c.close, 64'539'900'000'000LL);
    EXPECT_EQ(c.volume, 9'994'500'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'756);
    EXPECT_FALSE(
        venue->parse(R"({"code":20015,"data":null,"msg":"this Futures is not allowed to trade."})", id).has_value());
}

TEST(KlinesBitunix, ParsesSpotRowsWithIsoTimes) {
    const auto venue = kline_venue(VenueId::Bitunix, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("bitunix_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'546'050'000'000LL);
    EXPECT_EQ(c.high, 64'575'850'000'000LL);
    EXPECT_EQ(c.low, 64'538'760'000'000LL);
    EXPECT_EQ(c.close, 64'565'660'000'000LL);
    EXPECT_EQ(c.volume, 3'215'680'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(got->back().minute, 29'912'756);
    EXPECT_FALSE(
        venue->parse(R"({"code":"2","msg":"result.parameter.error","data":null,"success":false})", id).has_value());
}
