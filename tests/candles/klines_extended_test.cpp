#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesExtended, PagesBackFromTheEndForTheStretch) {
    const auto venue = kline_venue(VenueId::Extended, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USD", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "api.starknet.extended.exchange");
    EXPECT_EQ(call.target, "/api/v1/info/candles/BTC-USD/trades?interval=PT1M&limit=4&endTime=1794765419999");
    EXPECT_EQ(kline_venue(VenueId::Extended, MarketType::Spot), nullptr);
}

TEST(KlinesExtended, ParsesRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Extended, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("extended_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'497'500'000'000LL);
    EXPECT_EQ(c.high, 64'516'000'000'000LL);
    EXPECT_EQ(c.low, 64'497'500'000'000LL);
    EXPECT_EQ(c.close, 64'515'500'000'000LL);
    EXPECT_EQ(c.volume, 253'700'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'756);
    EXPECT_FALSE(
        venue->parse(R"({"status":"ERROR","error":{"code":1001,"message":"Market not found"}})", id).has_value());
}
