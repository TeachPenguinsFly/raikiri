#include <raikiri/domain/instrument.hpp>

#include <gtest/gtest.h>

namespace raikiri {
namespace {

InstrumentBase btc_perp_base() {
    InstrumentBase base{};
    base.id = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
    base.local_symbol = *SymbolText::from("BTC/USDT");
    base.exchange_symbol = *SymbolText::from("BTCUSDT");
    base.tick_size = *parse_decimal_e9("0.1");
    base.lot_size = *parse_decimal_e9("0.001");
    base.min_notional = *parse_decimal_e9("5");
    base.contract_value = *parse_decimal_e9("1");
    base.kind = ContractKind::Linear;
    return base;
}

TEST(Instrument, PriceConversionExact) {
    const auto base = btc_perp_base();
    const auto ticks = base.price_to_ticks(*parse_decimal_e9("50000.5"));
    ASSERT_TRUE(ticks.has_value());
    EXPECT_EQ(ticks->raw, 500'005);
    EXPECT_EQ(base.ticks_to_price(*ticks).raw, parse_decimal_e9("50000.5")->raw);
}

TEST(Instrument, RejectsNonMultiplePrice) {
    const auto base = btc_perp_base();
    const auto ticks = base.price_to_ticks(*parse_decimal_e9("50000.55"));
    ASSERT_FALSE(ticks.has_value());
    EXPECT_EQ(ticks.error(), ConvError::NotAMultiple);
}

TEST(Instrument, QtyConversionExact) {
    const auto base = btc_perp_base();
    const auto lots = base.qty_to_lots(*parse_decimal_e9("0.25"));
    ASSERT_TRUE(lots.has_value());
    EXPECT_EQ(lots->raw, 250);
    EXPECT_EQ(base.lots_to_qty(*lots).raw, parse_decimal_e9("0.25")->raw);
}

TEST(Instrument, UndefRejected) {
    const auto base = btc_perp_base();
    EXPECT_EQ(base.price_to_ticks(FixedE9::undef()).error(), ConvError::UndefValue);
    EXPECT_TRUE(base.ticks_to_price(PriceTicks::undef()).is_undef());
}

TEST(Instrument, ValidateCatchesBadLoads) {
    EXPECT_TRUE(validate(btc_perp_base()));

    auto no_tick = btc_perp_base();
    no_tick.tick_size = FixedE9::from_raw(0);
    EXPECT_FALSE(validate(no_tick));

    auto no_id = btc_perp_base();
    no_id.id = {};
    EXPECT_FALSE(validate(no_id));

    // A zero-initialised kind is the shape of an adapter that forgot to set it,
    // and it must not load as a plausible-looking linear contract.
    auto no_kind = btc_perp_base();
    no_kind.kind = ContractKind::Unknown;
    EXPECT_FALSE(validate(no_kind));
}

TEST(Instrument, VariantForcesOptionHandling) {
    // Compile-time property: the Option alternative exists in the variant, so
    // every std::visit over Instrument must already handle it. This is the
    // options extension seam.
    const Instrument instrument = PerpInstrument{btc_perp_base(), 28'800};
    EXPECT_EQ(market_of(instrument), MarketType::Perp);
    EXPECT_EQ(base_of(instrument).local_symbol.view(), "BTC/USDT");

    const Instrument option = OptionInstrument{btc_perp_base(),
                                               PriceTicks::from_raw(500'000),
                                               UnixNanos{1'800'000'000'000'000'000ULL},
                                               PutCall::Call};
    EXPECT_EQ(std::get<OptionInstrument>(option).put_call, PutCall::Call);
}

} // namespace
} // namespace raikiri
