#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesPhemex, PerpAsksTheKlineList) {
    const auto venue = kline_venue(VenueId::Phemex, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTCUSDT", 29'912'603, 29'912'608);
    EXPECT_EQ(call.host, "api.phemex.com");
    EXPECT_EQ(call.target,
              "/exchange/public/md/v2/kline/list?symbol=BTCUSDT&resolution=60&from=1794756180&to=1794756480");
    EXPECT_EQ(kline_venue(VenueId::Phemex, MarketType::Spot), nullptr);
}

TEST(KlinesPhemex, ParsesRealValuedRows) {
    const auto venue = kline_venue(VenueId::Phemex, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("phemex_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'698'600'000'000LL);
    EXPECT_EQ(c.high, 64'698'600'000'000LL);
    EXPECT_EQ(c.low, 64'600'000'000'000LL);
    EXPECT_EQ(c.close, 64'600'000'000'000LL);
    EXPECT_EQ(c.volume, 335'000'000LL);
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_FALSE(venue->parse("{\"code\":30018,\"msg\":\"phemex.data.size.uplimt\",\"data\":null}", id).has_value());
}

TEST(KlinesPhemex, UsdContractsAreScaledAndCountContracts) {
    const auto venue = kline_venue(VenueId::Phemex, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse("{\"code\":0,\"msg\":\"OK\",\"data\":{\"total\":-1,\"rows\":[[1794756180,60,"
                                  "\"646737000\",\"646564000\",\"646647000\",\"645609000\",\"645609000\",\"116394\","
                                  "\"180126234\",\"BTCUSD\"]]}}",
                                  id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 1U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'603);
    EXPECT_EQ(c.open, 64'656'400'000'000LL);
    EXPECT_EQ(c.high, 64'664'700'000'000LL);
    EXPECT_EQ(c.low, 64'560'900'000'000LL);
    EXPECT_EQ(c.close, 64'560'900'000'000LL);
    EXPECT_EQ(c.volume, 116'394'000'000'000LL);
}
