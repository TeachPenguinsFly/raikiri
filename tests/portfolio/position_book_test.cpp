#include <raikiri/domain/fill_report.hpp>
#include <raikiri/portfolio/position_book.hpp>

#include <gtest/gtest.h>

#include <cstddef>

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

OrderEvent
fill(const InstrumentBase& base, std::uint16_t strategy, Side side, std::int64_t lots, std::string_view price) {
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

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"), QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(base, fill(base, 2, Side::Sell, 400, "50000"), QtyLots::from_raw(400)));

    EXPECT_EQ(book.size(), 2U);
    EXPECT_EQ(book.find({1, base.id})->qty().raw, 1000);
    EXPECT_EQ(book.find({2, base.id})->qty().raw, -400);
}

TEST(PositionBook, SameSymbolOnTwoVenuesIsTwoPositions) {
    // the venue is the top byte of InstrumentId
    const auto binance = btcusdt(VenueId::Binance);
    const auto bybit = btcusdt(VenueId::Bybit);
    PositionBook book;

    ASSERT_TRUE(book.on_fill(binance, fill(binance, 1, Side::Buy, 1000, "50000"), QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(bybit, fill(bybit, 1, Side::Sell, 1000, "50010"), QtyLots::from_raw(1000)));

    EXPECT_EQ(book.size(), 2U);
    EXPECT_EQ(book.find({1, binance.id})->qty().raw, 1000);
    EXPECT_EQ(book.find({1, bybit.id})->qty().raw, -1000);
}

TEST(PositionBook, RepeatedFillsLandInOnePosition) {
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book;

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 100, "50000"), QtyLots::from_raw(100)));
    }
    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.find({1, base.id})->qty().raw, 500);
    EXPECT_EQ(book.find({1, base.id})->fill_count(), 5U);
}

TEST(PositionBook, ClosedPositionStaysInTheBook) {
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book;

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"), QtyLots::from_raw(1000)));
    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Sell, 1000, "51000"), QtyLots::from_raw(1000)));

    const auto* p = book.find({1, base.id});
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->flat());
    EXPECT_EQ(p->realized().raw, 1000 * k_e9);
    EXPECT_EQ(book.size(), 1U);
}

TEST(PositionBook, PointersSurviveLaterInserts) {
    // strategies hold find() pointers across callbacks, so the book never reallocates
    const auto base = btcusdt(VenueId::Binance);
    PositionBook book{4};

    ASSERT_TRUE(book.on_fill(base, fill(base, 1, Side::Buy, 1000, "50000"), QtyLots::from_raw(1000)));
    const auto* first = book.find({1, base.id});

    for (std::uint16_t s = 2; s <= 4; ++s) {
        ASSERT_TRUE(book.on_fill(base, fill(base, s, Side::Buy, 100, "50000"), QtyLots::from_raw(100)));
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
    EXPECT_TRUE(book.open({1, base.id}, UnixNanos{1}).has_value());
}

TEST(PositionBook, UnknownKeyIsNullNotAnEmptyPosition) {
    const auto base = btcusdt(VenueId::Binance);
    const PositionBook book;
    EXPECT_EQ(book.find({1, base.id}), nullptr);
}

TEST(PositionBook, ARestoredPositionIsIndistinguishableFromATradedOne) {
    const auto base = btcusdt(VenueId::Bybit);

    PositionBook traded;
    ASSERT_TRUE(traded.on_fill(base, fill(base, 1, Side::Buy, 4, "60000"), QtyLots::from_raw(4)).has_value());
    ASSERT_TRUE(traded.on_fill(base, fill(base, 1, Side::Sell, 1, "61000"), QtyLots::from_raw(1)).has_value());
    const auto* original = traded.find({1, base.id});
    ASSERT_NE(original, nullptr);

    PositionBook rebuilt;
    ASSERT_TRUE(rebuilt.restore({1, base.id},
                                {.qty = original->qty(),
                                 .cost = original->cost(),
                                 .realized = original->realized(),
                                 .fees = original->fees(),
                                 .fill_count = original->fill_count(),
                                 .ts_opened = original->ts_opened(),
                                 .ts_updated = original->ts_updated()}));

    const auto* restored = rebuilt.find({1, base.id});
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->qty().raw, original->qty().raw);
    EXPECT_EQ(restored->cost().raw, original->cost().raw);
    EXPECT_EQ(restored->realized().raw, original->realized().raw);
    EXPECT_EQ(restored->fill_count(), original->fill_count());
    EXPECT_EQ(restored->avg_entry_price(base).raw, original->avg_entry_price(base).raw);

    ASSERT_TRUE(rebuilt.on_fill(base, fill(base, 1, Side::Sell, 3, "62000"), QtyLots::from_raw(3)).has_value());
    EXPECT_TRUE(rebuilt.find({1, base.id})->flat());
}

TEST(PositionBook, RestoringTwiceIsRefusedRatherThanDoubled) {
    const auto base = btcusdt(VenueId::Bybit);
    PositionBook book;
    const Position::Restored state{.qty = QtyLots::from_raw(5)};

    EXPECT_TRUE(book.restore({1, base.id}, state));
    EXPECT_FALSE(book.restore({1, base.id}, state));
    EXPECT_EQ(book.size(), 1U);
    EXPECT_EQ(book.find({1, base.id})->qty().raw, 5);
}

TEST(PositionBook, RestoreRespectsTheCapacityTheBookWasGiven) {
    const auto base = btcusdt(VenueId::Bybit);
    PositionBook book{1};
    EXPECT_TRUE(book.restore({1, base.id}, {}));
    EXPECT_FALSE(book.restore({2, base.id}, {}));
}

OptionInstrument coin_option(ContractKind kind) {
    OptionInstrument option{};
    option.base.id = InstrumentId::make(VenueId::Okx, MarketType::Option, SymbolId{7});
    option.base.underlying = *UnderlyingText::from("BTC");
    option.base.kind = kind;
    option.base.tick_size = FixedE9::from_raw(100'000);         // 0.0001 btc
    option.base.lot_size = FixedE9::from_raw(k_e9);             // one contract
    option.base.contract_value = FixedE9::from_raw(k_e9 / 100); // 0.01 btc
    option.premium = OptionUnit::Base;
    option.greeks = OptionUnit::Quote;
    return option;
}

OrderEvent option_fill(const OptionInstrument& option, Side side, std::int64_t ticks, std::string_view fee_asset) {
    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.side = side;
    event.instrument = option.base.id;
    event.client_order_id = ClientOrderId::make(1, 1);
    event.price = PriceTicks::from_raw(ticks);
    event.qty = QtyLots::from_raw(10);
    event.fee = FixedE9::from_raw(2'000); // 0.000002 of whatever fee_asset says
    event.fee_asset = to_fee_asset(fee_asset);
    event.ts_event = UnixNanos{5};
    return event;
}

TEST(PositionBook, ACoinFeeJoinsTheSumsOfAnOptionCountedInThatCoin) {
    // the inverse kind is okx's spelling and the linear one deribit's; both are premium in the coin
    for (const auto kind : {ContractKind::Inverse, ContractKind::Linear}) {
        PositionBook book{4};
        const Instrument instrument{coin_option(kind)};
        const auto& option = std::get<OptionInstrument>(instrument);
        EXPECT_EQ(settle_side(instrument), SettleSide::Base);

        // bought at 0.05 btc, sold at 0.06 btc: ten contracts of 0.01 btc make 0.001 btc
        const auto bought = book.on_fill(instrument, option_fill(option, Side::Buy, 500, "BTC"), QtyLots::from_raw(10));
        ASSERT_TRUE(bought.has_value());
        EXPECT_EQ(bought->unbooked_fee.raw, 0);
        const auto sold = book.on_fill(instrument, option_fill(option, Side::Sell, 600, "BTC"), QtyLots::from_raw(10));
        ASSERT_TRUE(sold.has_value());
        EXPECT_EQ(sold->realized, FixedE9::from_raw(1'000'000));

        const auto* held = book.find({1, option.base.id});
        ASSERT_NE(held, nullptr);
        EXPECT_EQ(held->realized(), FixedE9::from_raw(1'000'000));
        EXPECT_EQ(held->fees(), FixedE9::from_raw(4'000));
        EXPECT_EQ(held->net_realized(), FixedE9::from_raw(996'000));
    }
}

TEST(PositionBook, AFeeInAnotherCurrencyIsLeftOutAndReported) {
    PositionBook book{4};
    const Instrument instrument{coin_option(ContractKind::Inverse)};
    const auto& option = std::get<OptionInstrument>(instrument);
    const auto applied = book.on_fill(instrument, option_fill(option, Side::Buy, 500, "BNB"), QtyLots::from_raw(10));
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied->unbooked_fee, FixedE9::from_raw(2'000));
    EXPECT_EQ(applied->open_fee.raw, 0);
    EXPECT_EQ(book.find({1, option.base.id})->fees().raw, 0);
    EXPECT_EQ(book.find({1, option.base.id})->qty(), QtyLots::from_raw(10));
}

TEST(PositionBook, AQuoteOptionTakesAUsdFeeAndRefusesTheCoin) {
    auto quoted = coin_option(ContractKind::Linear);
    quoted.premium = OptionUnit::Quote;
    const Instrument instrument{quoted};
    EXPECT_EQ(settle_side(instrument), SettleSide::Quote);
    EXPECT_TRUE(fee_joins_sums(quoted.base, SettleSide::Quote, to_fee_asset("usdt")));
    EXPECT_TRUE(fee_joins_sums(quoted.base, SettleSide::Quote, to_fee_asset("USDC")));
    EXPECT_FALSE(fee_joins_sums(quoted.base, SettleSide::Quote, to_fee_asset("BTC")));
    EXPECT_FALSE(fee_joins_sums(quoted.base, SettleSide::Base, to_fee_asset("USDT")));
}

TEST(PositionBook, AFillThatStatesNoCurrencyBooksAsItAlwaysDid) {
    EXPECT_TRUE(to_fee_asset("").empty());
    InstrumentBase perp{};
    perp.kind = ContractKind::Linear;
    EXPECT_TRUE(fee_joins_sums(perp, SettleSide::Quote, FeeAsset{}));
    // too long to hold is held as a code no sum matches
    EXPECT_EQ(to_fee_asset("FDUSDT").view(), "?");
    EXPECT_FALSE(fee_joins_sums(perp, SettleSide::Quote, to_fee_asset("FDUSDT")));
}

static_assert(sizeof(OrderEvent) == 192);
static_assert(offsetof(OrderEvent, exchange_order_id) == 56);
static_assert(sizeof(FillReport) == 168);

} // namespace
} // namespace raikiri::portfolio
