#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBitget, AsksTheMinutesBeforeAnExclusiveEnd) {
    const auto perp = kline_venue(VenueId::Bitget, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTCUSDT", 29'912'600, 29'912'604);
    EXPECT_EQ(call.host, "api.bitget.com");
    EXPECT_EQ(call.target,
              "/api/v2/mix/market/history-candles?symbol=BTCUSDT&productType=USDT-FUTURES&granularity=1m"
              "&endTime=1794756240000&limit=4");
    const auto spot = kline_venue(VenueId::Bitget, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->call("BTCUSDT", 29'912'600, 29'912'604).target,
              "/api/v2/spot/market/history-candles?symbol=BTCUSDT&granularity=1min&endTime=1794756240000&limit=4");
    EXPECT_EQ(kline_venue(VenueId::Bitget, MarketType::Future), nullptr);
}

TEST(KlinesBitget, ParsesRowsInTheBaseCoin) {
    const auto venue = kline_venue(VenueId::Bitget, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Bitget, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("bitget_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'600);
    EXPECT_EQ(c.open, 64'749'200'000'000LL);
    EXPECT_EQ(c.high, 64'749'300'000'000LL);
    EXPECT_EQ(c.low, 64'703'300'000'000LL);
    EXPECT_EQ(c.close, 64'731'700'000'000LL);
    EXPECT_EQ(c.volume, 8'578'700'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'603);
    EXPECT_FALSE(
        venue
            ->parse(R"({"code":"40034","msg":"Parameter NOPE does not exist","requestTime":1794767070199,"data":null})",
                    id)
            .has_value());
}
