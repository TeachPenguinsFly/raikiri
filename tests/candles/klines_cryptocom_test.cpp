#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesCryptoCom, AsksCandlesticksForTheStretch) {
    const auto perp = kline_venue(VenueId::CryptoCom, MarketType::Perp);
    ASSERT_NE(perp, nullptr);
    const auto call = perp->call("BTCUSD-PERP", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.crypto.com");
    EXPECT_EQ(call.target,
              "/exchange/v1/public/get-candlestick?instrument_name=BTCUSD-PERP&timeframe=1m&count=300"
              "&start_ts=1794756180000&end_ts=1794756480000");
    EXPECT_NE(kline_venue(VenueId::CryptoCom, MarketType::Spot), nullptr);
    EXPECT_EQ(kline_venue(VenueId::CryptoCom, MarketType::Option), nullptr);
}

TEST(KlinesCryptoCom, ParsesResultData) {
    const auto venue = kline_venue(VenueId::CryptoCom, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("cryptocom_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'681'000'000'000LL);
    EXPECT_EQ(c.high, 64'703'400'000'000LL);
    EXPECT_EQ(c.low, 64'580'800'000'000LL);
    EXPECT_EQ(c.close, 64'629'700'000'000LL);
    EXPECT_EQ(c.volume, 4'142'300'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(
        venue->parse("{\"id\":-1,\"method\":\"public/get-candlestick\",\"code\":40004,\"message\":\"Invalid\"}", id)
            .has_value());
}
