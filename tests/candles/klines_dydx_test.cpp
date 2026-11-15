#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesDydx, AsksTheIndexerInIsoTimes) {
    const auto venue = kline_venue(VenueId::Dydx, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USD", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "indexer.dydx.trade");
    EXPECT_EQ(call.target,
              "/v4/candles/perpetualMarkets/BTC-USD?resolution=1MIN&fromISO=2026-11-15T17:53:00Z"
              "&toISO=2026-11-15T17:56:59Z&limit=1000");
    EXPECT_EQ(kline_venue(VenueId::Dydx, MarketType::Spot), nullptr);
}

TEST(KlinesDydx, ParsesCandlesOldestFirst) {
    const auto venue = kline_venue(VenueId::Dydx, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("dydx_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'750);
    EXPECT_EQ(c.open, 64'401'000'000'000LL);
    EXPECT_EQ(c.high, 64'457'000'000'000LL);
    EXPECT_EQ(c.low, 64'401'000'000'000LL);
    EXPECT_EQ(c.close, 64'457'000'000'000LL);
    EXPECT_EQ(c.volume, 100'200'000LL);
    EXPECT_EQ(c.trades, 2);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'753);
    EXPECT_FALSE(venue
                     ->parse(R"j({"errors":[{"value":"NOPE-USD","msg":"ticker must be a valid ticker (BTC-USD, etc)",)j"
                             R"j("param":"ticker","location":"params"}]})j",
                             id)
                     .has_value());
}
