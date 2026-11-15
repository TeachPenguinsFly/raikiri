#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesHtx, AsksTheLinearSwapHistory) {
    const auto venue = kline_venue(VenueId::Htx, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-USDT", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "api.hbdm.vn");
    EXPECT_EQ(call.target,
              "/linear-swap-ex/market/history/kline?contract_code=BTC-USDT&period=1min&from=1794758400&to=1794758640");
    EXPECT_TRUE(call.body.empty());
    EXPECT_EQ(kline_venue(VenueId::Htx, MarketType::Spot), nullptr);
}

TEST(KlinesHtx, ParsesRowsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Htx, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Htx, MarketType::Perp, SymbolId{1});
    const auto got = venue->parse(candle_fixture("htx_perp.json"), id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'721'600'000'000LL);
    EXPECT_EQ(c.high, 64'723'800'000'000LL);
    EXPECT_EQ(c.low, 64'677'300'000'000LL);
    EXPECT_EQ(c.close, 64'701'200'000'000LL);
    EXPECT_EQ(c.volume, 1'942'000'000'000LL); // contracts
    EXPECT_EQ(c.trades, 45);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);
    EXPECT_FALSE(venue
                     ->parse(R"({"ts":1794767225517,"status":"error","err-code":"invalid-parameter",)"
                             R"("err-msg":"invalid symbol"})",
                             id)
                     .has_value());
}
