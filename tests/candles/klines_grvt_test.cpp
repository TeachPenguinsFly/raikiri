#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesGrvt, PerpPostsTheKlineQueryForOneMinuteBars) {
    const auto venue = kline_venue(VenueId::Grvt, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC_USDT_Perp", 29'912'750, 29'912'755);
    EXPECT_EQ(call.host, "market-data.grvt.io");
    EXPECT_EQ(call.target, "/full/v1/kline");
    EXPECT_EQ(call.body,
              R"({"instrument":"BTC_USDT_Perp","interval":"CI_1_M","type":"TRADE","start_time":"1794765000000000000",)"
              R"("end_time":"1794765299999999999","limit":1000})");
    EXPECT_EQ(kline_venue(VenueId::Grvt, MarketType::Spot), nullptr);
}

TEST(KlinesGrvt, ParsesNewestFirstRowsIntoVenueMinutesOldestFirst) {
    const auto venue = kline_venue(VenueId::Grvt, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("grvt_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'750);
    EXPECT_EQ(c.open, 64'471'000'000'000LL);
    EXPECT_EQ(c.high, 64'473'100'000'000LL);
    EXPECT_EQ(c.low, 64'433'100'000'000LL);
    EXPECT_EQ(c.close, 64'473'100'000'000LL);
    EXPECT_EQ(c.volume, 12'000'000LL);
    EXPECT_EQ(c.trades, 5);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'754);
    EXPECT_FALSE(venue->parse(R"({"code":2000,"message":"Instrument is invalid","status":400})", id).has_value());
}
