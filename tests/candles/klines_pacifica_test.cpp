#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesPacifica, AsksTheKlineBetweenBothBounds) {
    const auto venue = kline_venue(VenueId::Pacifica, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "api.pacifica.fi");
    EXPECT_EQ(call.target, "/api/v1/kline?symbol=BTC&interval=1m&start_time=1794765180000&end_time=1794765419999");
    EXPECT_EQ(kline_venue(VenueId::Pacifica, MarketType::Spot), nullptr);
}

TEST(KlinesPacifica, ParsesRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Pacifica, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("pacifica_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'525'000'000'000LL);
    EXPECT_EQ(c.high, 64'525'000'000'000LL);
    EXPECT_EQ(c.low, 64'525'000'000'000LL);
    EXPECT_EQ(c.close, 64'525'000'000'000LL);
    EXPECT_EQ(c.volume, 20'000LL);
    EXPECT_EQ(c.trades, 2);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue
                     ->parse(R"({"success":false,"data":null,"error":"`interval`: Must be one of 1m, 3m, 5m.",)"
                             R"("code":400,"error_id":"malformed_request"})",
                             id)
                     .has_value());
}
