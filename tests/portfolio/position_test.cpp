#include <raikiri/portfolio/position.hpp>

#include <gtest/gtest.h>

#include <vector>

namespace raikiri::portfolio {
namespace {

struct Fill {
    Side side{Side::Buy};
    std::int64_t lots{0};
    std::string_view price;
    std::string_view fee{"0"};
};

InstrumentBase make(ContractKind kind,
                    std::string_view tick,
                    std::string_view lot,
                    std::string_view contract_value) {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from("X");
    base.exchange_symbol = *SymbolText::from("X");
    base.tick_size = *parse_decimal_e9(tick);
    base.lot_size = *parse_decimal_e9(lot);
    base.contract_value = *parse_decimal_e9(contract_value);
    base.kind = kind;
    return base;
}

InstrumentBase binance_btcusdt() {
    return make(ContractKind::Linear, "0.1", "0.001", "1");
}

InstrumentBase bitmex_xbtusd() {
    return make(ContractKind::Inverse, "0.5", "1", "1");
}

Position fold(const InstrumentBase& base, const std::vector<Fill>& fills) {
    Position position{PositionKey{7, base.id}, UnixNanos{1}};
    std::uint64_t ts = 1;
    for (const auto& f : fills) {
        position.apply_fill(base,
                            f.side,
                            QtyLots::from_raw(f.lots),
                            *base.price_to_ticks(*parse_decimal_e9(f.price)),
                            *parse_decimal_e9(f.fee),
                            UnixNanos{++ts});
    }
    return position;
}

// A position is only its numbers; comparing them all is the determinism claim.
::testing::AssertionResult same(const Position& a, const Position& b) {
    if (a.qty().raw == b.qty().raw && a.cost().raw == b.cost().raw &&
        a.realized().raw == b.realized().raw && a.fees().raw == b.fees().raw) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
           << "qty " << a.qty().raw << "/" << b.qty().raw << " cost " << a.cost().raw << "/"
           << b.cost().raw << " realized " << a.realized().raw << "/" << b.realized().raw
           << " fees " << a.fees().raw << "/" << b.fees().raw;
}

TEST(Position, LongOpensAndClosesForWhatTheMoveWasWorth) {
    const auto base = binance_btcusdt();
    // 1 BTC = 1000 lots at a 0.001 lot size.
    const auto p = fold(base, {{Side::Buy, 1000, "50000"}, {Side::Sell, 1000, "51000"}});

    EXPECT_TRUE(p.flat());
    EXPECT_EQ(p.realized().raw, 1000 * k_e9);
    EXPECT_EQ(p.cost().raw, 0);
}

TEST(Position, ShortIsTheSameCodeWithTheSignsReversed) {
    const auto base = binance_btcusdt();
    const auto win = fold(base, {{Side::Sell, 1000, "51000"}, {Side::Buy, 1000, "50000"}});
    EXPECT_EQ(win.realized().raw, 1000 * k_e9);

    const auto loss = fold(base, {{Side::Sell, 1000, "50000"}, {Side::Buy, 1000, "51000"}});
    EXPECT_EQ(loss.realized().raw, -1000 * k_e9);
}

TEST(Position, FullCloseLeavesNoBasisResidue) {
    // Three opens at different prices, then one exact close. A pro-rata
    // release would truncate on each leg and strand a few nanounits in a
    // position that is flat.
    const auto base = binance_btcusdt();
    const auto p = fold(base,
                        {{Side::Buy, 333, "50000.1"},
                         {Side::Buy, 333, "50000.3"},
                         {Side::Buy, 334, "50000.7"},
                         {Side::Sell, 1000, "51000"}});
    EXPECT_TRUE(p.flat());
    EXPECT_EQ(p.cost().raw, 0);
}

TEST(Position, PartialCloseDoesNotMoveTheAverageEntry) {
    // The classic bug: releasing basis by a formula that is not exactly
    // pro-rata walks the average with every partial close.
    const auto base = binance_btcusdt();
    Position p{PositionKey{7, base.id}, UnixNanos{1}};
    const auto at = [&](std::string_view px) {
        return *base.price_to_ticks(*parse_decimal_e9(px));
    };

    p.apply_fill(
        base, Side::Buy, QtyLots::from_raw(1000), at("50000"), FixedE9::from_raw(0), UnixNanos{2});
    const auto before = p.avg_entry_price(base);
    EXPECT_EQ(before.raw, 50'000 * k_e9);

    p.apply_fill(
        base, Side::Sell, QtyLots::from_raw(400), at("55000"), FixedE9::from_raw(0), UnixNanos{3});
    EXPECT_EQ(p.qty().raw, 600);
    EXPECT_EQ(p.avg_entry_price(base).raw, before.raw);
    EXPECT_EQ(p.realized().raw, 400 * 5 * k_e9); // 0.4 BTC through a 5000 move
}

TEST(Position, FlipSplitsIntoACloseAndAnOpen) {
    // The property: one crossing fill and the two fills it is equivalent to
    // must produce the same position, to the unit.
    const auto base = binance_btcusdt();
    const auto flipped = fold(base, {{Side::Buy, 1000, "50000"}, {Side::Sell, 2500, "51000"}});
    const auto split = fold(
        base,
        {{Side::Buy, 1000, "50000"}, {Side::Sell, 1000, "51000"}, {Side::Sell, 1500, "51000"}});
    EXPECT_TRUE(same(flipped, split));

    EXPECT_EQ(flipped.qty().raw, -1500);
    EXPECT_EQ(flipped.realized().raw, 1000 * k_e9); // only the closing 1 BTC realized
    EXPECT_EQ(flipped.avg_entry_price(base).raw, 51'000 * k_e9);
}

TEST(Position, FlipProRatesTheFeeWithoutLosingAUnit) {
    // 7 nanounits across a 1000/2500 split does not divide. Rounding both
    // halves independently would make them sum to something other than 7.
    const auto base = binance_btcusdt();
    Position p{PositionKey{7, base.id}, UnixNanos{1}};
    const auto at = [&](std::string_view px) {
        return *base.price_to_ticks(*parse_decimal_e9(px));
    };

    p.apply_fill(
        base, Side::Buy, QtyLots::from_raw(1000), at("50000"), FixedE9::from_raw(0), UnixNanos{2});
    const auto applied = p.apply_fill(
        base, Side::Sell, QtyLots::from_raw(2500), at("51000"), FixedE9::from_raw(7), UnixNanos{3});

    EXPECT_TRUE(applied.flipped);
    EXPECT_EQ(applied.close_fee.raw + applied.open_fee.raw, 7);
    EXPECT_EQ(p.fees().raw, 7);
}

TEST(Position, ResentFillIsANoOp) {
    // The caller derives the delta from the drop in leaves_qty, so a venue
    // resending a fill notification produces zero and must change nothing.
    const auto base = binance_btcusdt();
    Position p{PositionKey{7, base.id}, UnixNanos{1}};
    const auto at = *base.price_to_ticks(*parse_decimal_e9("50000"));

    p.apply_fill(base, Side::Buy, QtyLots::from_raw(1000), at, FixedE9::from_raw(3), UnixNanos{2});
    const auto before = p;
    const auto applied =
        p.apply_fill(base, Side::Buy, QtyLots::from_raw(0), at, FixedE9::from_raw(3), UnixNanos{4});

    EXPECT_TRUE(same(before, p));
    EXPECT_EQ(applied.realized.raw, 0);
    EXPECT_EQ(p.fill_count(), 1U);
}

TEST(Position, FoldIsDeterministic) {
    const auto base = binance_btcusdt();
    const std::vector<Fill> fills{{Side::Buy, 1000, "50000", "1"},
                                  {Side::Sell, 400, "50500", "2"},
                                  {Side::Sell, 1200, "51000", "3"},
                                  {Side::Buy, 600, "49000", "4"}};
    EXPECT_TRUE(same(fold(base, fills), fold(base, fills)));
}

TEST(Position, InverseLongProfitsWhenItsBaseValueShrinks) {
    // 50 000 contracts of 1 USD: 1.0 BTC at entry, 0.8333 at exit. The long
    // gains 0.16666667 BTC — and it gains it because the number went DOWN.
    const auto base = bitmex_xbtusd();
    const auto p = fold(base, {{Side::Buy, 50'000, "50000"}, {Side::Sell, 50'000, "60000"}});

    EXPECT_TRUE(p.flat());
    EXPECT_NEAR(p.realized().raw, 166'666'667, 1);
    EXPECT_EQ(p.cost().raw, 0);
}

TEST(Position, InverseShortLosesWhatTheLongGained) {
    const auto base = bitmex_xbtusd();
    const auto p = fold(base, {{Side::Sell, 50'000, "50000"}, {Side::Buy, 50'000, "60000"}});
    EXPECT_NEAR(p.realized().raw, -166'666'667, 1);
}

TEST(Position, UnrealizedIsAFunctionOfTheMarkArgument) {
    const auto base = binance_btcusdt();
    const auto p = fold(base, {{Side::Buy, 1000, "50000"}});
    const auto at = [&](std::string_view px) {
        return *base.price_to_ticks(*parse_decimal_e9(px));
    };

    EXPECT_EQ(unrealized_pnl(base, p, at("51000")).raw, 1000 * k_e9);
    EXPECT_EQ(unrealized_pnl(base, p, at("49000")).raw, -1000 * k_e9);
    EXPECT_EQ(unrealized_pnl(base, p, at("50000")).raw, 0);
    // Asking twice at different prices left the position alone.
    EXPECT_EQ(p.cost().raw, 50'000 * k_e9);
}

TEST(Position, UnrealizedOnInverseAlsoInverts) {
    const auto base = bitmex_xbtusd();
    const auto p = fold(base, {{Side::Buy, 50'000, "50000"}});
    const auto at = *base.price_to_ticks(*parse_decimal_e9("60000"));
    EXPECT_NEAR(unrealized_pnl(base, p, at).raw, 166'666'667, 1);
}

TEST(Position, RealizedIsGrossAndFeesAreSeparate) {
    // Netting fees into the basis would make the average entry a number no
    // venue report can ever be compared against.
    const auto base = binance_btcusdt();
    const auto p =
        fold(base, {{Side::Buy, 1000, "50000", "10"}, {Side::Sell, 1000, "51000", "10.2"}});
    EXPECT_EQ(p.realized().raw, 1000 * k_e9);
    EXPECT_EQ(p.fees().raw, parse_decimal_e9("20.2")->raw);
    EXPECT_EQ(p.net_realized().raw, 1000 * k_e9 - parse_decimal_e9("20.2")->raw);
}

} // namespace
} // namespace raikiri::portfolio
