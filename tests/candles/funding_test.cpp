#include <raikiri/candles/funding.hpp>

#include <gtest/gtest.h>

#include <vector>

#include "fixture.hpp"

using namespace raikiri;
using namespace raikiri::candles;

namespace {

InstrumentId perp(VenueId venue, std::uint32_t symbol = 1) {
    return InstrumentId::make(venue, MarketType::Perp, SymbolId{symbol});
}

std::vector<MetricSample> rows(VenueId venue, const std::string& fixture) {
    const auto source = funding_venue(venue, MarketType::Perp);
    auto got = source->parse(candle_fixture(fixture), perp(venue));
    EXPECT_TRUE(got.has_value()) << got.error();
    return got.value_or(std::vector<MetricSample>{});
}

// minutes [29'916'000, 29'918'000) are 1794960000000 ms to 1795080000000 ms
constexpr std::int32_t k_from = 29'916'000;
constexpr std::int32_t k_to = 29'918'000;

} // namespace

TEST(FundingVenues, OnlyPerpsOfTheVenuesReadHaveASource) {
    EXPECT_NE(funding_venue(VenueId::Binance, MarketType::Perp), nullptr);
    EXPECT_EQ(funding_venue(VenueId::Binance, MarketType::Spot), nullptr);
    EXPECT_EQ(funding_venue(VenueId::Bybit, MarketType::Future), nullptr);
    EXPECT_EQ(funding_venue(VenueId::Coinbase, MarketType::Perp), nullptr);
}

TEST(FundingVenues, AskTheWindowTheWayEachVenueTakesIt) {
    const auto call = [](VenueId venue, std::string_view symbol, std::int32_t page = 1) {
        return funding_venue(venue, MarketType::Perp)->call(symbol, k_from, k_to, page);
    };
    const auto binance = call(VenueId::Binance, "BTCUSDT");
    EXPECT_EQ(binance.host, "fapi.binance.com");
    EXPECT_EQ(binance.target,
              "/fapi/v1/fundingRate?symbol=BTCUSDT&startTime=1794960000000&endTime=1795079999999&limit=1000");
    EXPECT_TRUE(binance.body.empty());
    const auto bybit = call(VenueId::Bybit, "BTCUSDT");
    EXPECT_EQ(bybit.host, "api.bybit.com");
    EXPECT_EQ(bybit.target,
              "/v5/market/funding/history?category=linear&symbol=BTCUSDT&startTime=1794960000000"
              "&endTime=1795079999999&limit=200");
    const auto okx = call(VenueId::Okx, "BTC-USDT-SWAP");
    EXPECT_EQ(okx.host, "www.okx.com");
    EXPECT_EQ(okx.target,
              "/api/v5/public/funding-rate-history?instId=BTC-USDT-SWAP&after=1795080000000&before=1794959999999"
              "&limit=100");
    const auto hyperliquid = call(VenueId::Hyperliquid, "xyz:TSLA");
    EXPECT_EQ(hyperliquid.host, "api.hyperliquid.xyz");
    EXPECT_EQ(hyperliquid.target, "/info");
    EXPECT_EQ(hyperliquid.body,
              R"({"type":"fundingHistory","coin":"xyz:TSLA","startTime":1794960000000,"endTime":1795079999999})");
    const auto gate = call(VenueId::Gate, "BTC_USDT");
    EXPECT_EQ(gate.host, "api.gateio.ws");
    EXPECT_EQ(gate.target, "/api/v4/futures/usdt/funding_rate?contract=BTC_USDT&limit=34");
    EXPECT_EQ(funding_venue(VenueId::Gate, MarketType::Perp)->call("BTC_USDT", 0, 30 * 24 * 60, 1).target,
              "/api/v4/futures/usdt/funding_rate?contract=BTC_USDT&limit=721");
    const auto bitget = call(VenueId::Bitget, "BTCUSDT", 3);
    EXPECT_EQ(bitget.host, "api.bitget.com");
    EXPECT_EQ(bitget.target,
              "/api/v2/mix/market/history-fund-rate?symbol=BTCUSDT&productType=USDT-FUTURES&pageSize=100&pageNo=3");
    const auto kucoin = call(VenueId::Kucoin, "XBTUSDTM");
    EXPECT_EQ(kucoin.host, "api-futures.kucoin.com");
    EXPECT_EQ(kucoin.target, "/api/v1/contract/funding-rates?symbol=XBTUSDTM&from=1794960000000&to=1795079999999");
}

TEST(FundingVenues, BinanceRowsAreOldestFirstWithATimeAFewMillisecondsLate) {
    const auto got = rows(VenueId::Binance, "funding_binance.json");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got.front().instrument, perp(VenueId::Binance));
    EXPECT_EQ(got.front().minute, 29'916'960);
    EXPECT_EQ(got.front().funding_rate, 59'630);
    EXPECT_FALSE(got.front().mark.has_value());
    EXPECT_FALSE(got.front().index.has_value());
    EXPECT_FALSE(got.front().open_interest.has_value());
    // "fundingTime":1795104000002
    EXPECT_EQ(got.back().minute, 29'918'400);
    EXPECT_EQ(got.back().funding_rate, 17'850);
    // an error is an object, not a list
    const auto source = funding_venue(VenueId::Binance, MarketType::Perp);
    EXPECT_FALSE(source->parse(R"({"code":-1121,"msg":"Invalid symbol."})", perp(VenueId::Binance)).has_value());
    EXPECT_FALSE(source->parse("<html>", perp(VenueId::Binance)).has_value());
}

TEST(FundingVenues, BybitRowsSitUnderResultListNewestFirst) {
    const auto got = rows(VenueId::Bybit, "funding_bybit.json");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got.front().minute, 29'917'920);
    EXPECT_EQ(got.front().funding_rate, 54'250);
    EXPECT_EQ(got.back().minute, 29'916'480);
    const auto source = funding_venue(VenueId::Bybit, MarketType::Perp);
    EXPECT_FALSE(
        source->parse(R"({"retCode":10001,"retMsg":"params error","result":{}})", perp(VenueId::Bybit)).has_value());
}

TEST(FundingVenues, OkxGivesTheRealizedRateCutAtNinePlaces) {
    const auto got = rows(VenueId::Okx, "funding_okx.json");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got[0].minute, 29'917'920);
    // "-0.0000055330692592"
    EXPECT_EQ(got[0].funding_rate, -5'533);
    // "0.0000634843947535"
    EXPECT_EQ(got[2].funding_rate, 63'484);
    EXPECT_EQ(got.back().minute, 29'916'480);
}

TEST(FundingVenues, HyperliquidRowsAreHourlyOldestFirst) {
    const auto got = rows(VenueId::Hyperliquid, "funding_hyperliquid.json");
    ASSERT_EQ(got.size(), 3U);
    // "time":1795003200077
    EXPECT_EQ(got[0].minute, 29'916'720);
    EXPECT_EQ(got[0].funding_rate, 5'160);
    EXPECT_EQ(got[1].minute, 29'916'780);
    EXPECT_EQ(got[2].minute, 29'916'840);
}

TEST(FundingVenues, GateTimesAreSeconds) {
    const auto got = rows(VenueId::Gate, "funding_gate.json");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got.front().minute, 29'925'120);
    EXPECT_EQ(got.front().funding_rate, -13'900);
    EXPECT_EQ(got.back().minute, 29'923'680);
}

TEST(FundingVenues, BitgetRowsSitUnderData) {
    const auto got = rows(VenueId::Bitget, "funding_bitget.json");
    ASSERT_EQ(got.size(), 4U);
    EXPECT_EQ(got.front().minute, 29'925'120);
    EXPECT_EQ(got.front().funding_rate, -37'000);
    EXPECT_EQ(got.back().minute, 29'923'680);
}

TEST(FundingVenues, KucoinRatesAreNumbers) {
    const auto got = rows(VenueId::Kucoin, "funding_kucoin.json");
    ASSERT_EQ(got.size(), 3U);
    EXPECT_EQ(got.front().minute, 29'917'920);
    // 5.1E-5
    EXPECT_EQ(got.front().funding_rate, 51'000);
    EXPECT_EQ(got.back().minute, 29'916'960);
}

TEST(FundingBackfill, WaitsForTheVenuesCandleBackfill) {
    const auto id = perp(VenueId::Bybit);
    FundingBackfill funding;
    EXPECT_TRUE(funding.add({.instrument = id, .symbol = "BTCUSDT", .from = k_from, .to = k_to}));
    EXPECT_FALSE(
        funding.add({.instrument = perp(VenueId::Coinbase), .symbol = "BTC-PERP", .from = k_from, .to = k_to}));
    EXPECT_FALSE(funding.add({.instrument = id, .symbol = "BTCUSDT", .from = k_to, .to = k_to}));
    EXPECT_EQ(funding.queued(), 1U);
    Backfill klines;
    klines.add({.instrument = perp(VenueId::Bybit, 2), .symbol = "ETHUSDT", .from = k_from, .to = k_from + 5});
    EXPECT_TRUE(klines.busy(id));
    EXPECT_FALSE(klines.busy(perp(VenueId::Okx)));
    EXPECT_TRUE(funding.due(1, klines).empty());
    ASSERT_EQ(klines.due(1, k_to).size(), 1U);
    EXPECT_FALSE(klines.busy(id));
    const auto due = funding.due(1, klines);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due.front().part.instrument, id);
    EXPECT_EQ(due.front().call.host, "api.bybit.com");
    EXPECT_EQ(funding.queued(), 0U);
}

TEST(FundingBackfill, PacesAVenueAtItsCallsAMinute) {
    FundingBackfill funding;
    const Backfill klines{};
    for (std::uint32_t i = 1; i <= 2; ++i) {
        (void)funding.add({.instrument = perp(VenueId::Bybit, i), .symbol = "BTCUSDT", .from = k_from, .to = k_to});
    }
    EXPECT_EQ(funding.due(1'000'000'000ULL, klines).size(), 1U);
    EXPECT_TRUE(funding.due(3'000'000'000ULL, klines).empty());
    // 20 a minute is one every three seconds
    EXPECT_EQ(funding.due(4'000'000'000ULL, klines).size(), 1U);
}

TEST(FundingBackfill, AFullAnswerQueuesTheRestOfItsWindow) {
    const Backfill klines{};
    const auto ask = [&](VenueId venue, std::size_t answered, std::int32_t first, std::int32_t step) {
        FundingBackfill funding;
        (void)funding.add({.instrument = perp(venue), .symbol = "X", .from = k_from, .to = k_to});
        const auto due = funding.due(1, klines);
        std::vector<MetricSample> got;
        for (std::size_t i = 0; i < answered; ++i) {
            got.push_back(
                {.instrument = perp(venue), .minute = first + static_cast<std::int32_t>(i) * step, .funding_rate = 1});
        }
        funding.answered(due.front(), got);
        auto rest = funding.due(1'000'000'000'000ULL, klines);
        return rest.empty() ? FundingBackfill::Part{} : rest.front().part;
    };
    // bybit's newest 200 of the window end at its minute 1000: the older ones are still to ask
    const auto older = ask(VenueId::Bybit, 200, k_from + 1000, 5);
    EXPECT_EQ(older.from, k_from);
    EXPECT_EQ(older.to, k_from + 1000);
    EXPECT_EQ(ask(VenueId::Bybit, 199, k_from + 1000, 5).to, 0);
    // hyperliquid's oldest 500 start at the window's first minute
    const auto newer = ask(VenueId::Hyperliquid, 500, k_from, 2);
    EXPECT_EQ(newer.from, k_from + 999);
    EXPECT_EQ(newer.to, k_to);
    // bitget pages on while a page stays inside the window, and stops at one reaching before it
    const auto page = ask(VenueId::Bitget, 100, k_from + 500, 10);
    EXPECT_EQ(page.page, 2);
    EXPECT_EQ(page.from, k_from);
    EXPECT_EQ(page.to, k_to);
    EXPECT_EQ(ask(VenueId::Bitget, 100, k_from - 10, 10).to, 0);
    // gate's one call is all there is
    EXPECT_EQ(ask(VenueId::Gate, 1000, k_from, 1).to, 0);
}

TEST(FundingBackfill, AFailedCallIsAskedOnceMore) {
    const Backfill klines{};
    FundingBackfill funding;
    (void)funding.add({.instrument = perp(VenueId::Okx), .symbol = "BTC-USDT-SWAP", .from = k_from, .to = k_to});
    const auto first = funding.due(1, klines);
    ASSERT_EQ(first.size(), 1U);
    funding.retry(first.front());
    EXPECT_EQ(funding.queued(), 1U);
    const auto second = funding.due(1'000'000'000'000ULL, klines);
    ASSERT_EQ(second.size(), 1U);
    funding.retry(second.front());
    EXPECT_EQ(funding.queued(), 0U);
    EXPECT_EQ(funding.dropped(), 1U);
    funding.defer(second.front());
    EXPECT_EQ(funding.queued(), 1U);
}
