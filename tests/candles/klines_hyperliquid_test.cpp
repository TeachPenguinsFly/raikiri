#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesHyperliquid, PostsACandleSnapshotForTheCoin) {
    const auto venue = kline_venue(VenueId::Hyperliquid, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "api.hyperliquid.xyz");
    EXPECT_EQ(call.target, "/info");
    EXPECT_EQ(call.body,
              R"({"type":"candleSnapshot","req":{"coin":"BTC","interval":"1m",)"
              R"("startTime":1794758400000,"endTime":1794758699999}})");
    EXPECT_NE(venue->call("xyz:TSLA", 29'912'640, 29'912'645).body.find(R"("coin":"xyz:TSLA")"), std::string::npos);
    const auto spot = kline_venue(VenueId::Hyperliquid, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_NE(spot->call("@107", 29'912'640, 29'912'645).body.find(R"("coin":"@107")"), std::string::npos);
    EXPECT_EQ(kline_venue(VenueId::Hyperliquid, MarketType::Option), nullptr);
}

TEST(KlinesHyperliquid, ParsesRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Hyperliquid, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Hyperliquid, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("hyperliquid_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'721'000'000'000LL);
    EXPECT_EQ(c.high, 64'721'000'000'000LL);
    EXPECT_EQ(c.low, 64'674'000'000'000LL);
    EXPECT_EQ(c.close, 64'700'000'000'000LL);
    EXPECT_EQ(c.volume, 35'295'570'000LL);
    EXPECT_EQ(c.trades, 264);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    // an unknown coin answers a bare null with a 500
    EXPECT_FALSE(venue->parse("null", id).has_value());
}
