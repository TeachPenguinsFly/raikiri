#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesLbank, SpotAsksForAsManyBarsAsTheStretchFromItsStart) {
    const auto venue = kline_venue(VenueId::Lbank, MarketType::Spot);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("btc_usdt", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "api.lbank.info");
    EXPECT_EQ(call.target, "/v2/kline.do?symbol=btc_usdt&size=4&type=minute1&time=1794765180");
    EXPECT_TRUE(call.body.empty());
    EXPECT_EQ(kline_venue(VenueId::Lbank, MarketType::Perp), nullptr);
    EXPECT_EQ(kline_venue(VenueId::Lbank, MarketType::Option), nullptr);
}

TEST(KlinesLbank, ParsesNumberRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Lbank, MarketType::Spot);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("lbank_spot.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'543'830'000'000LL);
    EXPECT_EQ(c.high, 64'574'570'000'000LL);
    EXPECT_EQ(c.low, 64'535'260'000'000LL);
    EXPECT_EQ(c.close, 64'564'450'000'000LL);
    EXPECT_EQ(c.volume, 1'717'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'756);
    EXPECT_FALSE(
        venue->parse(R"({"result":"false","msg":"currency pair nonsupport","error_code":10008,"ts":1794767024251})", id)
            .has_value());
}
