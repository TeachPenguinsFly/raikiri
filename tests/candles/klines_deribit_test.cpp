#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

TEST(KlinesDeribit, PostsChartDataWithTheVolumeColumnAsId) {
    const auto venue = kline_venue(VenueId::Deribit, MarketType::Perp);
    ASSERT_NE(venue, nullptr);
    const auto call = venue->call("BTC-PERPETUAL", 29'912'640, 29'912'645);
    EXPECT_EQ(call.host, "www.deribit.com");
    EXPECT_EQ(call.target, "/api/v2");
    EXPECT_EQ(call.body,
              R"({"jsonrpc":"2.0","id":"cost","method":"public/get_tradingview_chart_data",)"
              R"("params":{"instrument_name":"BTC-PERPETUAL","start_timestamp":1794758400000,)"
              R"("end_timestamp":1794758699999,"resolution":"1"}})");
    EXPECT_NE(venue->call("BTC_USDC-PERPETUAL", 29'912'640, 29'912'645).body.find(R"("id":"volume")"),
              std::string::npos);
    EXPECT_NE(kline_venue(VenueId::Deribit, MarketType::Future), nullptr);
    EXPECT_NE(kline_venue(VenueId::Deribit, MarketType::Spot), nullptr);
    EXPECT_EQ(kline_venue(VenueId::Deribit, MarketType::Option), nullptr);
}

TEST(KlinesDeribit, ParsesColumnsIntoVenueMinutes) {
    const auto venue = kline_venue(VenueId::Deribit, MarketType::Perp);
    const auto id = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{1});
    const auto body = candle_fixture("deribit_perp.json");
    const auto got = venue->parse(body, id);
    ASSERT_TRUE(got.has_value()) << got.error();
    ASSERT_EQ(got->size(), 5U);
    const auto& c = got->front();
    EXPECT_EQ(c.minute, 29'912'640);
    EXPECT_EQ(c.open, 64'711'000'000'000LL);
    EXPECT_EQ(c.high, 64'711'000'000'000LL);
    EXPECT_EQ(c.low, 64'653'000'000'000LL);
    EXPECT_EQ(c.close, 64'683'000'000'000LL);
    EXPECT_EQ(c.volume, 1'087'610'000'000'000LL); // dollars, as BTC-PERPETUAL prints count
    EXPECT_EQ(c.trades, 0);
    EXPECT_EQ(c.source, Source::Venue);
    EXPECT_EQ(got->back().minute, 29'912'644);

    // the same answer asked for a linear contract reads the coin
    auto linear = body;
    const std::string cost = R"("id":"cost")";
    linear.replace(linear.find(cost), cost.size(), R"("id":"volume")");
    const auto coins = venue->parse(linear, id);
    ASSERT_TRUE(coins.has_value()) << coins.error();
    EXPECT_EQ(coins->front().volume, 16'815'689'470LL);

    EXPECT_FALSE(venue
                     ->parse(R"({"jsonrpc":"2.0","error":{"code":-32602,"data":{"reason":"wrong format",)"
                             R"("param":"instrument_name"},"message":"Invalid params"},"testnet":false})",
                             id)
                     .has_value());
}
