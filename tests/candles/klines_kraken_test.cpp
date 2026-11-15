#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesKraken, SpotAsksOhlcSinceTheFirstMinute) {
    const auto venue = kline_venue(VenueId::Kraken, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC/USD", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.kraken.com");
    EXPECT_EQ(call.target, "/0/public/OHLC?pair=BTC/USD&interval=1&since=1794756180");
    EXPECT_EQ(kline_venue(VenueId::Kraken, MarketType::Option), nullptr);
}

TEST(KlinesKraken, PerpAsksTheFuturesCharts) {
    const auto venue = kline_venue(VenueId::Kraken, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("PF_XBTUSD", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "futures.kraken.com");
    EXPECT_EQ(call.target, "/api/charts/v1/trade/PF_XBTUSD/1m?from=1794756180&to=1794756420");
}

TEST(KlinesKraken, SpotDropsTheOpenMinute) {
    const auto venue = kline_venue(VenueId::Kraken, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("kraken_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'680'500'000'000LL);
    EXPECT_EQ(c.high, 64'697'500'000'000LL);
    EXPECT_EQ(c.low, 64'594'900'000'000LL);
    EXPECT_EQ(c.close, 64'622'500'000'000LL);
    EXPECT_EQ(c.volume, 2'458'373'570LL);
    EXPECT_EQ(c.trades, 243);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'606);
    EXPECT_FALSE(venue->parse("{\"error\":[\"EQuery:Unknown asset pair\"]}", id).has_value());
}

TEST(KlinesKraken, PerpParsesChartCandles) {
    const auto venue = kline_venue(VenueId::Kraken, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("kraken_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'699'000'000'000LL);
    EXPECT_EQ(c.high, 64'699'000'000'000LL);
    EXPECT_EQ(c.low, 64'585'000'000'000LL);
    EXPECT_EQ(c.close, 64'633'000'000'000LL);
    EXPECT_EQ(c.volume, 10'973'900'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse("{\"result\":\"error\",\"error\":\"marketUnavailable\"}", id).has_value());
}
