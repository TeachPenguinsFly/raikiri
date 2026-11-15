#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesArcus, PerpAsksForOneMinuteCandlesInMicroseconds) {
    const auto venue = kline_venue(VenueId::Arcus, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USD", 29'912'750, 29'912'755);
    EXPECT_EQ(call.host, "api.arcus.xyz");
    EXPECT_EQ(call.target, "/v1/candles?market=BTC-USD&timeframe=1m&from=1794765000000000&to=1794765299999999");
    EXPECT_TRUE(call.body.empty());
    EXPECT_EQ(kline_venue(VenueId::Arcus, MarketType::Spot), nullptr);
}

TEST(KlinesArcus, ParsesNewestFirstRowsIntoVenueMinutesOldestFirst) {
    const auto venue = kline_venue(VenueId::Arcus, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("arcus_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'750);
    EXPECT_EQ(c.open, 64'406'900'000'000LL);
    EXPECT_EQ(c.high, 64'476'700'000'000LL);
    EXPECT_EQ(c.low, 64'382'700'000'000LL);
    EXPECT_EQ(c.close, 64'447'200'000'000LL);
    EXPECT_EQ(c.volume, 5'425'624'930LL);
    EXPECT_EQ(c.trades, 51);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'754);
    EXPECT_FALSE(venue->parse(R"({"error":"Missing 'timeframe' query parameter"})", id).has_value());
}
