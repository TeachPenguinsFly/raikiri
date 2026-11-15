#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesApex, AsksTheV3KlinesInInclusiveSeconds) {
    const auto venue = kline_venue(VenueId::Apex, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "omni.apex.exchange");
    EXPECT_EQ(call.target, "/api/v3/klines?symbol=BTCUSDT&interval=1&start=1794765180&end=1794765419&limit=200");
    EXPECT_EQ(kline_venue(VenueId::Apex, MarketType::Spot), nullptr);
}

TEST(KlinesApex, ParsesTheSymbolsRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Apex, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("apex_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'532'600'000'000LL);
    EXPECT_EQ(c.high, 64'549'200'000'000LL);
    EXPECT_EQ(c.low, 64'521'000'000'000LL);
    EXPECT_EQ(c.close, 64'546'600'000'000LL);
    EXPECT_EQ(c.volume, 4'339'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    const auto unknown = venue->parse(R"({"data":{},"timeCost":671})", id);
    ASSERT_TRUE(unknown.has_value());
    EXPECT_TRUE(unknown->empty());
    EXPECT_FALSE(venue->parse(R"({"code":2,"msg":"internal server error","timeCost":1883})", id).has_value());
}
