#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesToobit, AsksTheQuoteKlinesWithInclusiveBounds) {
    const auto venue = kline_venue(VenueId::Toobit, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-SWAP-USDT", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "api.toobit.com");
    EXPECT_EQ(call.target,
              "/quote/v1/klines?symbol=BTC-SWAP-USDT&interval=1m&startTime=1794765180000&endTime=1794765419999"
              "&limit=1000");
    const auto spot = kline_venue(VenueId::Toobit, MarketType::Spot);
    ASSERT_NE(spot, nullptr);
    EXPECT_EQ(spot->call("BTCUSDT", 29'912'753, 29'912'757).target,
              "/quote/v1/klines?symbol=BTCUSDT&interval=1m&startTime=1794765180000&endTime=1794765419999&limit=1000");
    EXPECT_EQ(kline_venue(VenueId::Toobit, MarketType::Option), nullptr);
}

TEST(KlinesToobit, ParsesPerpRowsInContracts) {
    const auto venue = kline_venue(VenueId::Toobit, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("toobit_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'522'000'000'000LL);
    EXPECT_EQ(c.high, 64'545'700'000'000LL);
    EXPECT_EQ(c.low, 64'508'100'000'000LL);
    EXPECT_EQ(c.close, 64'539'900'000'000LL);
    EXPECT_EQ(c.volume, 20'530'600'000'000LL);
    EXPECT_EQ(c.trades, 217);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse(R"({"code":-100010,"msg":"Invalid Symbols!"})", id).has_value());
}
