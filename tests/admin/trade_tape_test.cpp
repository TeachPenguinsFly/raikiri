#include <raikiri/admin/trade_tape.hpp>

#include <gtest/gtest.h>

namespace raikiri::admin {
namespace {

const InstrumentId k_btc = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{3});
const InstrumentId k_eth = InstrumentId::make(VenueId::Deribit, MarketType::Perp, SymbolId{4});

TradePrint print(std::int64_t ticks) {
    return {.price = PriceTicks{ticks}, .qty = QtyLots{1}, .aggressor = Side::Sell, .ts_event = UnixNanos{1}};
}

TEST(TradeTape, ANewSubscriberGetsWhatIsHeldOldestFirst) {
    TradeRing ring;
    EXPECT_TRUE(ring.since(0).empty());

    for (std::int64_t i = 1; i <= 3; ++i) {
        ring.push(print(i));
    }
    const auto held = ring.since(0);
    ASSERT_EQ(held.size(), 3U);
    EXPECT_EQ(held.front().price.raw, 1);
    EXPECT_EQ(held.back().price.raw, 3);
    EXPECT_EQ(ring.head(), 3U);
}

TEST(TradeTape, TheRingKeepsTheLastHundredAndForgetsTheRest) {
    TradeRing ring;
    for (std::int64_t i = 1; i <= 250; ++i) {
        ring.push(print(i));
    }
    const auto held = ring.since(0);
    ASSERT_EQ(held.size(), k_trade_rows);
    EXPECT_EQ(held.front().price.raw, 151);
    EXPECT_EQ(held.back().price.raw, 250);
}

TEST(TradeTape, PrintsBetweenTwoFramesLeaveAsOneBatch) {
    TradeRing ring;
    ring.push(print(1));
    auto cursor = ring.head();
    EXPECT_TRUE(ring.since(cursor).empty());

    for (std::int64_t i = 2; i <= 6; ++i) {
        ring.push(print(i));
    }
    const auto batch = ring.since(cursor);
    ASSERT_EQ(batch.size(), 5U);
    EXPECT_EQ(batch.front().price.raw, 2);
    EXPECT_EQ(batch.back().price.raw, 6);
    cursor = ring.head();
    EXPECT_TRUE(ring.since(cursor).empty());
}

TEST(TradeTape, AReaderLeftBehindGetsTheNewestAndNoMoreThanTheRing) {
    TradeRing ring;
    ring.push(print(1));
    const auto cursor = ring.head();
    for (std::int64_t i = 2; i <= 400; ++i) {
        ring.push(print(i));
    }
    const auto batch = ring.since(cursor);
    ASSERT_EQ(batch.size(), k_trade_rows);
    EXPECT_EQ(batch.front().price.raw, 301);
    EXPECT_EQ(batch.back().price.raw, 400);
}

TEST(TradeTape, APrintOfAnInstrumentNobodyWatchesIsNotKept) {
    TradeTape tape;
    EXPECT_FALSE(tape.push(k_btc, print(1)));

    const auto ring = tape.watch(k_btc);
    EXPECT_TRUE(tape.watched(k_btc));
    EXPECT_FALSE(tape.watched(k_eth));
    EXPECT_TRUE(tape.push(k_btc, print(2)));
    EXPECT_FALSE(tape.push(k_eth, print(3)));
    ASSERT_EQ(ring->since(0).size(), 1U);
    EXPECT_EQ(ring->since(0).front().price.raw, 2);
}

TEST(TradeTape, TwoSubscribersShareOneRingAndTheLastToLeaveFreesIt) {
    TradeTape tape;
    auto first = tape.watch(k_btc);
    auto second = tape.watch(k_btc);
    EXPECT_EQ(first, second);
    ASSERT_EQ(tape.instruments().size(), 1U);
    EXPECT_EQ(tape.instruments().front(), k_btc);

    first.reset();
    EXPECT_EQ(tape.prune(), 0U);
    EXPECT_TRUE(tape.watched(k_btc));

    second.reset();
    EXPECT_EQ(tape.prune(), 1U);
    EXPECT_FALSE(tape.watched(k_btc));
    EXPECT_FALSE(tape.push(k_btc, print(1)));
}

} // namespace
} // namespace raikiri::admin
