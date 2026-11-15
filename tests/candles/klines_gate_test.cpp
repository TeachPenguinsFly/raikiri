#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesGate, AsksFuturesAndSpotCandlesticksInSeconds) {
    const auto perp = kline_venue(VenueId::Gate, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTC_USDT", 29'912'600, 29'912'604);
    EXPECT_EQ(call.host, "api.gateio.ws");
    EXPECT_EQ(call.target,
              "/api/v4/futures/usdt/candlesticks?contract=BTC_USDT&interval=1m&from=1794756000&to=1794756239");
    const auto spot = kline_venue(VenueId::Gate, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->call("BTC_USDT", 29'912'600, 29'912'604).target,
              "/api/v4/spot/candlesticks?currency_pair=BTC_USDT&interval=1m&from=1794756000&to=1794756239");
    EXPECT_EQ(kline_venue(VenueId::Gate, MarketType::Future), nullptr);
}

TEST(KlinesGate, ParsesPerpRowsInContracts) {
    const auto venue = kline_venue(VenueId::Gate, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Gate, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("gate_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'750'200'000'000LL);
    EXPECT_EQ(c.high, 64'752'100'000'000LL);
    EXPECT_EQ(c.low, 64'706'500'000'000LL);
    EXPECT_EQ(c.close, 64'736'300'000'000LL);
    EXPECT_EQ(c.volume, 247'676'000'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'603);
    EXPECT_FALSE(venue->parse(R"({"label":"CONTRACT_NOT_FOUND"})", id).has_value());
}

TEST(KlinesGate, ParsesSpotRowsInTheBaseCoin) {
    const auto venue = kline_venue(VenueId::Gate, MarketType::Spot);
    const auto id = InstrumentId::make(VenueId::Gate, MarketType::Spot, SymbolId{1});
    const auto got = venue->parse(candle_fixture("gate_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'789'100'000'000LL);
    EXPECT_EQ(c.high, 64'789'100'000'000LL);
    EXPECT_EQ(c.low, 64'742'100'000'000LL);
    EXPECT_EQ(c.close, 64'764'100'000'000LL);
    EXPECT_EQ(c.volume, 10'389'965'000LL);
    EXPECT_EQ(c.trades, 0);

    const auto open = venue->parse(R"([["1794756240","1","1","1","1","1","1","false"]])", id);
    ASSERT_TRUE(open.has_value());
    EXPECT_TRUE(open->empty());
}
