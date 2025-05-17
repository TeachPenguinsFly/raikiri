#include <raikiri/portfolio/position_book.hpp>

#include <gtest/gtest.h>

namespace raikiri::portfolio {
namespace {

InstrumentBase btcusdt(VenueId venue) {
    InstrumentBase base{};
    base.id = InstrumentId::make(venue, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from("BTC/USDT");
    base.exchange_symbol = *SymbolText::from("BTCUSDT");
    base.tick_size = *parse_decimal_e9("0.1");
    base.lot_size = *parse_decimal_e9("0.001");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

OrderEvent fill(const InstrumentBase& base,
                std::uint16_t strategy,
                Side side,
                std::int64_t lots,
                std::string_view price) {
    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.side = side;
    event.instrument = base.id;
    event.client_order_id = ClientOrderId::make(strategy, 1);
    event.price = *base.price_to_ticks(*parse_decimal_e9(price));
    event.qty = QtyLots::from_raw(lots);
    event.fee = FixedE9::from_raw(0);
    event.ts_event = UnixNanos{100};
    return event;
}

TEST(PositionBook, TwoStrategiesOnOneInstrumentKeepSeparateBooks) {
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book;

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"),
                             QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(base, fill(base, 2, Side::Sell, 400, "50000"),
                             QtyLots::from_raw(400)));

    EXPECT_EQ(book.size(), 2U);
    EXPECT_EQ(book.find({1, base.id})->qty().raw, 1000);
    EXPECT_EQ(book.find({2, base.id})->qty().raw, -400);
}

TEST(PositionBook, SameSymbolOnTwoVenuesIsTwoPositions) {
    // The venue is the top byte of InstrumentId, so a basis trade holding both
    // legs never nets them by accident.
    const auto binance = btcusdt(VenueId::Binance);
    const auto bybit = btcusdt(VenueId::Bybit);
    PositionBook book;

    ASSERT_TRUE(book.on_fill(binance, fill(binance, 1, Side::Buy, 1000, "50000"),
                             QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(bybit, fill(bybit, 1, Side::Sell, 1000, "50010"),
                             QtyLots::from_raw(1000)));

    EXPECT_EQ(book.size(), 2U);
    EXPECT_EQ(book.find({1, binance.id})->qty().raw, 1000);
    EXPECT_EQ(book.find({1, bybit.id})->qty().raw, -1000);
}

TEST(PositionBook, RepeatedFillsLandInOnePosition) {
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book;

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 100, "50000"),
                                 QtyLots::from_raw(100)));
    }
    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.find({1, base.id})->qty().raw, 500);
    EXPECT_EQ(book.find({1, base.id})->fill_count(), 5U);
}

TEST(PositionBook, ClosedPositionStaysInTheBook) {
    // Flat is not gone: the realized PnL and the fill count are the record of
    // what happened, and dropping the row would drop them.
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book;

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"),
                             QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Sell, 1000, "51000"),
                             QtyLots::from_raw(1000)));

    const auto* p = book.find({1, base.id});
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->flat());
    EXPECT_EQ(p->realized().raw, 1000 * k_e9);
    EXPECT_EQ(book.size(), 1U);
}

TEST(PositionBook, PointersSurviveLaterInserts) {
    // Strategies hold what find() returns across callbacks, so the book must
    // never reallocate under them.
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book{4};

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"),
                             QtyLots::from_raw(1000)));
    const auto* first = book.find({1, base.id});

    for (std::uint16_t s = 2; s <= 4; ++s) {
        ASSERT_TRUE(book.on_fill(base, fill(base, s, Side::Buy, 100, "50000"),
                                 QtyLots::from_raw(100)));
    }
    EXPECT_EQ(first, book.find({1, base.id}));
    EXPECT_EQ(first->qty().raw, 1000);
}

TEST(PositionBook, RefusesToGrowPastCapacity) {
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book{2};

    ASSERT_TRUE(book.open({1, base.id}, UnixNanos{1}));
    ASSERT_TRUE(book.open({2, base.id}, UnixNanos{1}));

    const auto third = book.open({3, base.id}, UnixNanos{1});
    ASSERT_FALSE(third.has_value());
    EXPECT_EQ(third.error(), BookError::Full);
    // An existing key still resolves once full.
    EXPECT_TRUE(book.open({1, base.id}, UnixNanos{1}).has_value());
}

TEST(PositionBook, UnknownKeyIsNullNotAnEmptyPosition) {
    const auto base = btcusdt(VenueId::Binance);
    const PositionBook book;
    EXPECT_EQ(book.find({1, base.id}), nullptr);
}

} // namespace
} // namespace raikiri::portfolio
