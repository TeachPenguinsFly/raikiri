#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesBlofin, PerpAsksBetweenExclusiveBoundsForOneMinuteBars) {
    const auto venue = kline_venue(VenueId::Blofin, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USDT", 29'912'750, 29'912'755);
    EXPECT_EQ(call.host, "openapi.blofin.com");
    EXPECT_EQ(call.target,
              "/api/v1/market/candles?instId=BTC-USDT&bar=1m&after=1794765300000&before=1794764999999&limit=1440");
    EXPECT_EQ(kline_venue(VenueId::Blofin, MarketType::Spot), nullptr);
}

TEST(KlinesBlofin, ParsesNewestFirstRowsIntoVenueMinutesInContracts) {
    const auto venue = kline_venue(VenueId::Blofin, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("blofin_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'750);
    EXPECT_EQ(c.open, 64'403'400'000'000LL);
    EXPECT_EQ(c.high, 64'464'400'000'000LL);
    EXPECT_EQ(c.low, 64'364'500'000'000LL);
    EXPECT_EQ(c.close, 64'462'200'000'000LL);
    EXPECT_EQ(c.volume, 2'695'000'000'000LL);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'753);
    EXPECT_FALSE(venue->parse(R"({"code":"152002","msg":"Parameter instId error.","data":[]})", id).has_value());
}
