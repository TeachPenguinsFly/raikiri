#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesEdgex, AsksGetKlineByContractId) {
    const auto venue = kline_venue(VenueId::Edgex, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("30000001", 29'912'753, 29'912'757);
    EXPECT_EQ(call.host, "edgex-prod-v2.edgex.exchange");
    EXPECT_EQ(call.target,
              "/api/v2/public/quote/getKline?contractId=30000001&priceType=LAST_PRICE&klineType=MINUTE_1&size=1000"
              "&filterBeginKlineTimeInclusive=1794765180000&filterEndKlineTimeExclusive=1794765420000");
    EXPECT_EQ(kline_venue(VenueId::Edgex, MarketType::Spot), nullptr);
}

TEST(KlinesEdgex, ParsesStringRowsOldestFirst) {
    const auto venue = kline_venue(VenueId::Edgex, MarketType::Perp);
    const InstrumentId id{72339069014638592ULL};
    const auto got = venue->parse(candle_fixture("edgex_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 4U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'753);
    EXPECT_EQ(c.open, 64'509'300'000'000LL);
    EXPECT_EQ(c.high, 64'525'600'000'000LL);
    EXPECT_EQ(c.low, 64'493'800'000'000LL);
    EXPECT_EQ(c.close, 64'520'000'000'000LL);
    EXPECT_EQ(c.volume, 750'000'000LL);
    EXPECT_EQ(c.trades, 58);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'756);
    EXPECT_FALSE(venue
                     ->parse(R"({"code":"GATEWAY_INVALID_REQUEST","data":null,"errorParam":null,)"
                             R"("msg":"invalid enum parameter","requestTime":"1794767394606","traceId":""})",
                             id)
                     .has_value());
}
