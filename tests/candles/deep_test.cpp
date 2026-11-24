#include <raikiri/candles/deep.hpp>

#include <gtest/gtest.h>

using namespace raikiri;
using namespace raikiri::candles;

namespace {

InstrumentId perp(VenueId venue, std::uint32_t symbol = 1) {
    return InstrumentId::make(venue, MarketType::Perp, SymbolId{symbol});
}

constexpr std::int32_t k_floor = 29'916'000;
constexpr std::int32_t k_day = 24 * 60;

} // namespace

TEST(DeepBackfill, SkipsWhatAlreadyReachesTheFloorAndVenuesWithoutHistory) {
    DeepBackfill deep;
    const auto id = perp(VenueId::Binance);
    EXPECT_FALSE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor}));
    EXPECT_FALSE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + k_day}));
    EXPECT_FALSE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor - 10}));
    EXPECT_FALSE(deep.add({.instrument = InstrumentId::make(VenueId::Binance, MarketType::Option, SymbolId{1}),
                           .symbol = "BTC-C",
                           .from = k_floor,
                           .to = k_floor + 30 * k_day}));
    EXPECT_EQ(deep.queued(), 0U);
    EXPECT_TRUE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + k_day + 1}));
    EXPECT_EQ(deep.queued(), 1U);
}

TEST(DeepBackfill, WalksBackFromTheOldestStoredMinuteInTheVenuesLargestPages) {
    DeepBackfill deep;
    const auto id = perp(VenueId::Binance);
    const auto oldest = k_floor + 21 * k_day;
    ASSERT_TRUE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = oldest}));
    // binance perps answer 1500 minutes a call: three weeks are 21 calls, the oldest one short
    EXPECT_EQ(deep.queued(), 21U);
    const Backfill klines{};
    const FundingBackfill funding{};
    std::vector<DeepBackfill::Due> asked;
    for (std::uint64_t t = 1; asked.size() < 21; t += 60'000'000'000ULL) {
        for (auto& due : deep.due(t, klines, funding)) {
            asked.push_back(std::move(due));
        }
    }
    EXPECT_EQ(asked.front().part.to, oldest);
    EXPECT_EQ(asked.front().part.from, oldest - 1500);
    EXPECT_EQ(asked.front().call.host, "fapi.binance.com");
    for (std::size_t i = 1; i < asked.size(); ++i) {
        EXPECT_EQ(asked[i].part.to, asked[i - 1].part.from);
    }
    EXPECT_EQ(asked.back().part.from, k_floor);
    EXPECT_EQ(asked.back().part.to - asked.back().part.from, 21 * k_day - 20 * 1500);
    EXPECT_EQ(deep.queued(), 0U);
}

TEST(DeepBackfill, WaitsForTheVenuesCandleAndFundingBackfills) {
    DeepBackfill deep;
    const auto id = perp(VenueId::Bybit);
    ASSERT_TRUE(deep.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + 2 * k_day}));
    Backfill klines;
    klines.add({.instrument = perp(VenueId::Bybit, 2), .symbol = "ETHUSDT", .from = k_floor, .to = k_floor + 5});
    FundingBackfill funding;
    EXPECT_TRUE(deep.due(1, klines, funding).empty());
    ASSERT_EQ(klines.due(1, k_floor + 2 * k_day).size(), 1U);
    ASSERT_TRUE(funding.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + k_day}));
    EXPECT_TRUE(deep.due(1, klines, funding).empty());
    ASSERT_EQ(funding.due(1, klines).size(), 1U);
    const auto due = deep.due(1, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due.front().part.instrument, id);
    // another venue's backfill does not hold this one back
    DeepBackfill other;
    ASSERT_TRUE(other.add({.instrument = id, .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + 2 * k_day}));
    Backfill okx;
    okx.add({.instrument = perp(VenueId::Okx), .symbol = "BTC-USDT-SWAP", .from = k_floor, .to = k_floor + 5});
    EXPECT_EQ(other.due(1, okx, funding).size(), 1U);
}

TEST(DeepBackfill, PacesAVenueAtItsCallsAMinute) {
    DeepBackfill deep;
    ASSERT_TRUE(deep.add(
        {.instrument = perp(VenueId::Binance), .symbol = "BTCUSDT", .from = k_floor, .to = k_floor + 3 * k_day}));
    const Backfill klines{};
    const FundingBackfill funding{};
    EXPECT_EQ(deep.due(1'000'000'000ULL, klines, funding).size(), 1U);
    EXPECT_TRUE(deep.due(2'000'000'000ULL, klines, funding).empty());
    // binance perps spend 30 a minute, one every two seconds
    EXPECT_EQ(deep.due(3'000'000'000ULL, klines, funding).size(), 1U);
    const auto load = deep.load();
    ASSERT_EQ(load.size(), 1U);
    EXPECT_EQ(load.front().venue, VenueId::Binance);
    EXPECT_EQ(load.front().market, MarketType::Perp);
    EXPECT_EQ(load.front().per_minute, 30U);
    EXPECT_EQ(load.front().calls, deep.queued());
}

TEST(DeepBackfill, AnEmptyAnswerEndsTheInstrumentButNotTheNext) {
    DeepBackfill deep;
    const auto first = perp(VenueId::Binance, 1);
    const auto second = perp(VenueId::Binance, 2);
    ASSERT_TRUE(deep.add({.instrument = first, .symbol = "AUSDT", .from = k_floor, .to = k_floor + 3 * k_day}));
    ASSERT_TRUE(deep.add({.instrument = second, .symbol = "BUSDT", .from = k_floor, .to = k_floor + 3 * k_day}));
    EXPECT_EQ(deep.queued(), 6U);
    const Backfill klines{};
    const FundingBackfill funding{};
    auto due = deep.due(1, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    deep.answered(due.front(), 1500);
    EXPECT_EQ(deep.queued(), 5U);
    due = deep.due(10'000'000'000ULL, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    deep.answered(due.front(), 0);
    EXPECT_EQ(deep.queued(), 3U);
    due = deep.due(20'000'000'000ULL, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    EXPECT_EQ(due.front().part.instrument, second);
}

TEST(DeepBackfill, AFailedCallIsAskedOnceMore) {
    DeepBackfill deep;
    ASSERT_TRUE(deep.add(
        {.instrument = perp(VenueId::Okx), .symbol = "BTC-USDT-SWAP", .from = k_floor, .to = k_floor + 2 * k_day}));
    const Backfill klines{};
    const FundingBackfill funding{};
    const auto queued = deep.queued();
    auto due = deep.due(1, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    deep.retry(due.front());
    EXPECT_EQ(deep.queued(), queued);
    due = deep.due(10'000'000'000ULL, klines, funding);
    ASSERT_EQ(due.size(), 1U);
    deep.retry(due.front());
    EXPECT_EQ(deep.queued(), queued - 1);
    EXPECT_EQ(deep.dropped(), 1U);
}
