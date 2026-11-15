#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesDerive, PerpPostsTheChartQueryInSeconds) {
    const auto venue = kline_venue(VenueId::Derive, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-PERP", 29'912'750, 29'912'755);
    EXPECT_EQ(call.host, "api.lyra.finance");
    EXPECT_EQ(call.target, "/public/get_tradingview_chart_data");
    EXPECT_EQ(call.body,
              R"({"instrument_name":"BTC-PERP","period":60,"start_timestamp":1794765000,"end_timestamp":1794765299})");
    EXPECT_EQ(kline_venue(VenueId::Derive, MarketType::Option), nullptr);
}

// the fixture follows the documented schema: this network gets a 503 from derive
TEST(KlinesDerive, ParsesBucketsIntoVenueMinutesInContracts) {
    const auto venue = kline_venue(VenueId::Derive, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("derive_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 3U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'750);
    EXPECT_EQ(c.open, 64'410'500'000'000LL);
    EXPECT_EQ(c.high, 64'472'000'000'000LL);
    EXPECT_EQ(c.low, 64'388'250'000'000LL);
    EXPECT_EQ(c.close, 64'455'100'000'000LL);
    EXPECT_EQ(c.volume, 1'264'000'000LL);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse(R"({"error":{"code":-32602,"message":"Invalid params"},"id":"0"})", id).has_value());
}
