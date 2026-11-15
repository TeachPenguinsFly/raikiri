#include <raikiri/candles/candle.hpp>
#include <raikiri/candles/history.hpp>

#include <gtest/gtest.h>

using namespace raikiri;
using namespace raikiri::candles;

namespace {

constexpr std::uint64_t k_min = 60'000'000'000ULL;
const InstrumentId k_btc{72339069014638592ULL}; // binance perp

UnixNanos at(std::uint64_t minute, std::uint64_t second = 0) {
    return UnixNanos{minute * k_min + second * 1'000'000'000ULL};
}

} // namespace

TEST(MinuteBuilder, FoldsPrintsAndClosesAMinuteWhenTheNextBegins) {
    MinuteBuilder b;
    b.trade(k_btc, 100, 2, at(10, 1));
    b.trade(k_btc, 105, 1, at(10, 20));
    b.trade(k_btc, 98, 3, at(10, 40));
    b.trade(k_btc, 101, 1, at(10, 59));
    EXPECT_TRUE(b.take().empty());
    b.trade(k_btc, 102, 1, at(11, 0));
    const auto closed = b.take();
    ASSERT_EQ(closed.size(), 1U);
    const auto& c = closed[0];
    EXPECT_EQ(c.minute, 10);
    EXPECT_EQ(c.open, 100);
    EXPECT_EQ(c.high, 105);
    EXPECT_EQ(c.low, 98);
    EXPECT_EQ(c.close, 101);
    EXPECT_EQ(c.volume, 7);
    EXPECT_EQ(c.trades, 4);
    EXPECT_EQ(c.source, Source::Live);
}

TEST(MinuteBuilder, ClosesQuietMinutesAfterTheGraceAndDropsLatePrints) {
    MinuteBuilder b;
    b.trade(k_btc, 100, 1, at(10, 30));
    b.close_through(at(11, 2), 3'000'000'000ULL); // two seconds into the next minute: still open
    EXPECT_TRUE(b.take().empty());
    b.close_through(at(11, 4), 3'000'000'000ULL);
    EXPECT_EQ(b.take().size(), 1U);
    b.trade(k_btc, 101, 1, at(12, 5));
    b.trade(k_btc, 99, 1, at(11, 50)); // its minute was handed back already
    EXPECT_EQ(b.late(), 1U);
    b.close_all();
    const auto rest = b.take();
    ASSERT_EQ(rest.size(), 1U);
    EXPECT_EQ(rest[0].minute, 12);
    EXPECT_EQ(rest[0].close, 101);
}

TEST(Backfill, CutsAGapNewestFirstAndPacesEachVenue) {
    Backfill bf;
    // 3,000 minutes on binance perps: two calls of 1,500, the newer asked first
    bf.add({.instrument = k_btc, .symbol = "BTCUSDT", .from = 1'000, .to = 4'000});
    EXPECT_EQ(bf.queued(), 2U);
    const auto first = bf.due(0, 5'000);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first[0].part.from, 2'500);
    EXPECT_EQ(first[0].part.to, 4'000);
    // thirty calls a minute: the next one waits two seconds
    EXPECT_TRUE(bf.due(1'000'000'000ULL, 5'000).empty());
    const auto second = bf.due(2'000'000'000ULL, 5'000);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].part.from, 1'000);
    EXPECT_EQ(bf.queued(), 0U);
}

TEST(Backfill, WaitsForTheLastMinuteToCloseAndRetriesOnce) {
    Backfill bf;
    bf.add({.instrument = k_btc, .symbol = "BTCUSDT", .from = 4'990, .to = 5'001});
    EXPECT_TRUE(bf.due(0, 5'000).empty()); // minute 5,000 is still open
    const auto due = bf.due(0, 5'001);
    ASSERT_EQ(due.size(), 1U);
    bf.retry(due[0]);
    EXPECT_EQ(bf.queued(), 1U);
    const auto again = bf.due(10'000'000'000ULL, 5'001);
    ASSERT_EQ(again.size(), 1U);
    bf.retry(again[0]);
    EXPECT_EQ(bf.queued(), 0U);
    EXPECT_EQ(bf.dropped(), 1U);
}

TEST(Backfill, SkipsAVenueWithoutHistory) {
    Backfill bf;
    const auto option = InstrumentId::make(VenueId::Binance, MarketType::Option, SymbolId{7});
    bf.add({.instrument = option, .symbol = "BTC-261225-100000-C", .from = 0, .to = 100});
    EXPECT_EQ(bf.queued(), 0U);
}
