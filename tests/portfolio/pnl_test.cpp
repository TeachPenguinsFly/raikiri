#include <raikiri/portfolio/pnl.hpp>

#include <gtest/gtest.h>

#include <cstdlib>

namespace raikiri::portfolio {
namespace {

InstrumentBase make(ContractKind kind, std::string_view tick, std::string_view lot, std::string_view contract_value) {
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

// binance BTCUSDT: quantity is base units, contract_value 1
InstrumentBase binance_btcusdt() {
    return make(ContractKind::Linear, "0.1", "0.001", "1");
}

// gate BTC_USDT: contracts of 0.0001 BTC
InstrumentBase gate_btc_usdt() {
    return make(ContractKind::Linear, "0.1", "1", "0.0001");
}

// deribit BTC-PERPETUAL: contracts of 10 USD, settled in BTC
InstrumentBase deribit_btc_perpetual() {
    return make(ContractKind::Inverse, "0.5", "1", "10");
}

// bitmex XBTUSD: one contract is exactly 1 USD.
InstrumentBase bitmex_xbtusd() {
    return make(ContractKind::Inverse, "0.5", "1", "1");
}

PriceTicks at(const InstrumentBase& base, std::string_view price) {
    return *base.price_to_ticks(*parse_decimal_e9(price));
}

TEST(Pnl, LinearBaseUnits) {
    const auto base = binance_btcusdt();
    const QtyLots half_btc = *base.qty_to_lots(*parse_decimal_e9("0.5"));

    EXPECT_EQ(settlement_value(base, at(base, "50000"), half_btc).raw, 25'000 * k_e9);
    EXPECT_EQ(settlement_value(base, at(base, "51000"), half_btc).raw, 25'500 * k_e9);
    // 0.5 BTC through a 1000 USDT move.
    EXPECT_EQ(settlement_value(base, at(base, "51000"), half_btc).raw -
                  settlement_value(base, at(base, "50000"), half_btc).raw,
              500 * k_e9);
}

TEST(Pnl, LinearSubUnitContractValueDoesNotVanish) {
    // 100 contracts x 0.0001 BTC = 0.01 BTC
    const auto base = gate_btc_usdt();
    const QtyLots hundred = *base.qty_to_lots(*parse_decimal_e9("100"));

    EXPECT_EQ(base_quantity(base, hundred).raw, 10'000'000); // 0.01 BTC
    EXPECT_EQ(settlement_value(base, at(base, "50000"), hundred).raw, 500 * k_e9);
    EXPECT_EQ(settlement_value(base, at(base, "51000"), hundred).raw, 510 * k_e9);
}

TEST(Pnl, InverseSettlesInBaseNotQuote) {
    // 100 contracts x 10 USD = 1000 USD held in BTC; a long gains as the base value shrinks
    const auto base = deribit_btc_perpetual();
    const QtyLots hundred = *base.qty_to_lots(*parse_decimal_e9("100"));

    const auto entry = settlement_value(base, at(base, "50000"), hundred);
    const auto exited = settlement_value(base, at(base, "60000"), hundred);
    EXPECT_EQ(entry.raw, 20'000'000); // 0.02 BTC

    const int128_t closed_form = (static_cast<int128_t>(1000) * k_e9 * k_e9) / 50'000 / k_e9 -
                                 (static_cast<int128_t>(1000) * k_e9 * k_e9) / 60'000 / k_e9;
    const auto realized_long = entry.raw - exited.raw;
    EXPECT_LE(std::abs(realized_long - static_cast<std::int64_t>(closed_form)), 1)
        << "realized " << realized_long << " vs closed form " << static_cast<std::int64_t>(closed_form);
    EXPECT_LE(std::abs(realized_long - 3'333'333), 1); // 0.003333... BTC
}

TEST(Pnl, InverseSignIsOppositeToLinearForTheSameMove) {
    const auto linear = binance_btcusdt();
    const auto inverse = bitmex_xbtusd();
    const QtyLots one_btc = *linear.qty_to_lots(*parse_decimal_e9("1"));
    const QtyLots contracts = *inverse.qty_to_lots(*parse_decimal_e9("50000"));

    const auto linear_delta = settlement_value(linear, at(linear, "60000"), one_btc).raw -
                              settlement_value(linear, at(linear, "50000"), one_btc).raw;
    const auto inverse_delta = settlement_value(inverse, at(inverse, "60000"), contracts).raw -
                               settlement_value(inverse, at(inverse, "50000"), contracts).raw;

    EXPECT_GT(linear_delta, 0);
    EXPECT_LT(inverse_delta, 0);
    // 50 000 contracts of 1 USD: 1.0 BTC at entry, 0.8333... at exit.
    EXPECT_EQ(settlement_value(inverse, at(inverse, "50000"), contracts).raw, k_e9);
    EXPECT_LE(std::abs(-inverse_delta - 166'666'667), 1); // +0.16666667 BTC to a long
}

TEST(Pnl, SettleSideFollowsTheContractKind) {
    EXPECT_EQ(settle_side(ContractKind::Linear), SettleSide::Quote);
    EXPECT_EQ(settle_side(ContractKind::Inverse), SettleSide::Base);
}

TEST(Pnl, BaseQuantityRefusesInverse) {
    const auto base = deribit_btc_perpetual();
    const QtyLots hundred = *base.qty_to_lots(*parse_decimal_e9("100"));
    EXPECT_TRUE(base_quantity(base, hundred).is_undef());
}

TEST(Pnl, InverseRefusesAZeroPriceRatherThanDividing) {
    const auto base = bitmex_xbtusd();
    const QtyLots one = *base.qty_to_lots(*parse_decimal_e9("1"));
    EXPECT_TRUE(settlement_value(base, PriceTicks::from_raw(0), one).is_undef());
}

} // namespace
} // namespace raikiri::portfolio
