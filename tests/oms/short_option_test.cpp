#include <raikiri/oms/risk.hpp>
#include <raikiri/oms/short_option.hpp>

#include <gtest/gtest.h>

#include <array>

namespace raikiri::oms {
namespace {

constexpr FixedE9 dec(std::int64_t whole, std::int64_t e9_part = 0) noexcept {
    return FixedE9::from_raw(whole * k_e9 + e9_part);
}

ShortMarginInput input(OptionUnit unit, PutCall pc, FixedE9 strike, FixedE9 index, FixedE9 mark, FixedE9 price) {
    return {.premium = unit,
            .put_call = pc,
            .strike = strike,
            .index = index,
            .forward = index,
            .mark = mark,
            .order_price = price};
}

// the venue's own worked example: underlying 115000, strike 116000, mark 200 gives 16450 per unit
TEST(ShortMargin, GateCallMatchesItsPublishedExample) {
    const auto rates = short_margin_rates(VenueId::Gate, "BTC");
    const auto m = short_margin(
        rates, input(OptionUnit::Quote, PutCall::Call, dec(116'000), dec(115'000), dec(200), dec(150)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->initial, dec(16'450));
    // 0.075 × 115000 + 200
    EXPECT_EQ(m->maintenance, dec(8'825));
    // the order's premium term is the lower of mark and price
    EXPECT_EQ(m->order, dec(16'400));
}

// put: max(0.1 × (100000 + 500), 0.15 × 100000 − 10000) + 500 = max(10050, 5000) + 500
TEST(ShortMargin, GatePutFloorCarriesTheMark) {
    const auto rates = short_margin_rates(VenueId::Gate, "ETH");
    const auto m = short_margin(rates,
                                input(OptionUnit::Quote, PutCall::Put, dec(90'000), dec(100'000), dec(500), dec(500)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->initial, dec(10'550));
    EXPECT_EQ(m->maintenance, dec(8'000));
}

// the venue's example: index 30000, mark 300, factor 3% gives a position mm of 1260, which implies a 0.2% fee
TEST(ShortMargin, BybitMaintenanceMatchesItsPublishedExample) {
    const auto rates = short_margin_rates(VenueId::Bybit, "BTC");
    const auto m = short_margin(rates,
                                input(OptionUnit::Quote, PutCall::Call, dec(33'000), dec(30'000), dec(300), dec(300)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->maintenance, dec(1'260));
    // max(0.10 × 30000 − 3000, 0.05 × 30000) + 300 = 1800, above the mm
    EXPECT_EQ(m->initial, dec(1'800));
    // 1800 − 300 premium
    EXPECT_EQ(m->order, dec(1'500));
}

// eth's min factor equals its mm factor, so the fee lifts the mm over the risk: 1500 against 1500 + 60
TEST(ShortMargin, BybitFloorsAtMaintenance) {
    const auto rates = short_margin_rates(VenueId::Bybit, "ETH");
    const auto m = short_margin(rates,
                                input(OptionUnit::Quote, PutCall::Put, dec(10'000), dec(30'000), dec(0), dec(100)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->maintenance, dec(1'560));
    EXPECT_EQ(m->initial, dec(1'560));
    EXPECT_EQ(m->order, dec(1'500));
}

// coin: max(0.15 − 3000/60000, 0.1) + 0.01 = 0.11 btc per btc
TEST(ShortMargin, DeribitInverseIsCoinPerCoin) {
    const auto rates = short_margin_rates(VenueId::Deribit, "BTC");
    const auto mark = FixedE9::from_raw(10'000'000);
    const auto call = short_margin(rates, input(OptionUnit::Base, PutCall::Call, dec(63'000), dec(60'000), mark, mark));
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->initial, FixedE9::from_raw(110'000'000));
    EXPECT_EQ(call->maintenance, FixedE9::from_raw(85'000'000));
    EXPECT_EQ(call->order, call->initial);
}

TEST(ShortMargin, DeribitLinearCallAndPut) {
    const auto rates = short_margin_rates(VenueId::Deribit, "BTC");
    // max(0.15 − 3000/60000, 0.1) × 60000 + 500
    const auto call = short_margin(
        rates, input(OptionUnit::Quote, PutCall::Call, dec(63'000), dec(60'000), dec(500), dec(500)));
    ASSERT_TRUE(call.has_value());
    EXPECT_EQ(call->initial, dec(6'500));
    EXPECT_EQ(call->maintenance, dec(5'000));
    // max((0.15 − 12000/60000) × 60000, 0.1 × 48000) + 500 = 4800 + 500; mm 0.075 × 48000 + 500
    const auto put = short_margin(rates,
                                  input(OptionUnit::Quote, PutCall::Put, dec(48'000), dec(60'000), dec(500), dec(500)));
    ASSERT_TRUE(put.has_value());
    EXPECT_EQ(put->initial, dec(5'300));
    EXPECT_EQ(put->maintenance, dec(4'100));
}

// coin: max(0.1, 0.15 − 0) + 0.02 = 0.17; opening at 0.03 reserves 0.14; mm 0.03 + 0.02
TEST(ShortMargin, OkxCoinMarginedCreditsThePremiumDownToTheMinimum) {
    const auto rates = short_margin_rates(VenueId::Okx, "BTC");
    const auto mark = FixedE9::from_raw(20'000'000);
    const auto m = short_margin(
        rates, input(OptionUnit::Base, PutCall::Call, dec(60'000), dec(60'000), mark, FixedE9::from_raw(30'000'000)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->initial, FixedE9::from_raw(170'000'000));
    EXPECT_EQ(m->order, FixedE9::from_raw(140'000'000));
    EXPECT_EQ(m->maintenance, FixedE9::from_raw(50'000'000));
    const auto rich = short_margin(
        rates, input(OptionUnit::Base, PutCall::Call, dec(60'000), dec(60'000), mark, FixedE9::from_raw(150'000'000)));
    ASSERT_TRUE(rich.has_value());
    EXPECT_EQ(rich->order, FixedE9::from_raw(100'000'000));
}

// usd: max(0.1, 0.15) × 60000 + 1000 = 10000; opening at 1000 reserves max(9000, 6000)
TEST(ShortMargin, OkxUsdMarginedScalesByTheIndex) {
    const auto rates = short_margin_rates(VenueId::Okx, "ETH");
    const auto m = short_margin(
        rates, input(OptionUnit::Quote, PutCall::Put, dec(60'000), dec(60'000), dec(1'000), dec(1'000)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->initial, dec(10'000));
    EXPECT_EQ(m->order, dec(9'000));
    EXPECT_EQ(m->maintenance, dec(4'000));
}

TEST(ShortMargin, NoPublishedFormulaOrNoInputOrTheWrongUnitIsAnError) {
    const auto none = short_margin_rates(VenueId::Binance, "BTC");
    EXPECT_EQ(none.formula, MarginFormula::Unpublished);
    const auto good = input(OptionUnit::Quote, PutCall::Call, dec(60'000), dec(60'000), dec(1), dec(1));
    EXPECT_EQ(short_margin(none, good).error(), ShortMarginError::NoFormula);
    auto missing = good;
    missing.mark = FixedE9::undef();
    EXPECT_EQ(short_margin(short_margin_rates(VenueId::Gate, "BTC"), missing).error(), ShortMarginError::NoInput);
    auto coin = good;
    coin.premium = OptionUnit::Base;
    EXPECT_EQ(short_margin(short_margin_rates(VenueId::Bybit, "BTC"), coin).error(), ShortMarginError::WrongUnit);
    EXPECT_EQ(short_margin_rates(VenueId::Okx, "SOL").formula, MarginFormula::Unpublished);
}

Instrument option(OptionUnit unit, PutCall pc, VenueId venue = VenueId::Gate) {
    OptionInstrument out{.put_call = pc, .premium = unit};
    out.base.id = InstrumentId::make(venue, MarketType::Option, SymbolId{3});
    out.base.kind = ContractKind::Linear;
    out.base.tick_size = FixedE9::from_raw(k_e9 / 10);
    out.base.lot_size = dec(1);
    out.base.contract_value = FixedE9::from_raw(k_e9 / 100);
    out.strike = PriceTicks::from_raw(600'000); // 60000
    return out;
}

// 100 contracts of 0.01 are one coin: call at the underlying, put at the strike, and the same on a coin contract
TEST(ShortWorstCase, CallAtTheUnderlyingPutAtTheStrikeOnEitherSettlement) {
    const auto call = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Call));
    EXPECT_EQ(short_worst_case(call, QtyLots::from_raw(100), dec(70'000)), dec(70'000));
    EXPECT_TRUE(short_worst_case(call, QtyLots::from_raw(100), FixedE9::undef()).is_undef());
    const auto put = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Put));
    EXPECT_EQ(short_worst_case(put, QtyLots::from_raw(100), FixedE9::undef()), dec(60'000));
    const auto coin = std::get<OptionInstrument>(option(OptionUnit::Base, PutCall::Call));
    EXPECT_EQ(short_worst_case(coin, QtyLots::from_raw(100), dec(70'000)), dec(70'000));
}

TEST(ShortFigures, WorkingSellsCountOnlyPastAnyLong) {
    const auto call = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Call));
    const auto rates = short_margin_rates(VenueId::Gate, "BTC");
    const auto mine = short_figures({.option = &call,
                                     .position = QtyLots::from_raw(-100),
                                     .working_sell = QtyLots::from_raw(100),
                                     .index = dec(60'000),
                                     .forward = dec(60'000),
                                     .mark = dec(500)},
                                    rates);
    EXPECT_EQ(mine.held.raw, 100);
    EXPECT_EQ(mine.working.raw, 100);
    EXPECT_EQ(mine.naked_usd, dec(120'000));
    // at the money: 0.15 × 60000 + 500 per unit, on one unit each
    EXPECT_EQ(mine.margin_held, dec(9'500));
    EXPECT_EQ(mine.margin_working, dec(9'500));
    const auto covered = short_figures(
        {.option = &call, .position = QtyLots::from_raw(100), .working_sell = QtyLots::from_raw(40)}, rates);
    EXPECT_EQ(covered.held.raw, 0);
    EXPECT_EQ(covered.working.raw, 0);
    EXPECT_EQ(covered.naked_usd, dec(0));
}

TEST(SettlementAsset, TheUnderlyingWhenCoinSettledElseWhatTheVenueSettlesIn) {
    auto coin = std::get<OptionInstrument>(option(OptionUnit::Base, PutCall::Call));
    coin.base.underlying = *UnderlyingText::from("BTC");
    EXPECT_EQ(settlement_asset(coin), "BTC");
    auto usd = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Call, VenueId::Deribit));
    usd.base.exchange_symbol = *SymbolText::from("BTC_USDC-27NOV26-60000-C");
    EXPECT_EQ(settlement_asset(usd), "USDC");
    usd = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Call, VenueId::Bybit));
    usd.base.exchange_symbol = *SymbolText::from("BTC-27NOV26-60000-C-USDT");
    EXPECT_EQ(settlement_asset(usd), "USDT");
    usd.base.exchange_symbol = *SymbolText::from("BTC-27NOV26-60000-C");
    EXPECT_EQ(settlement_asset(usd), "USDC");
    usd = std::get<OptionInstrument>(option(OptionUnit::Quote, PutCall::Call, VenueId::Gate));
    usd.base.exchange_symbol = *SymbolText::from("BTC_USDT-261127-60000-C");
    EXPECT_EQ(settlement_asset(usd), "USDT");
    usd.premium = OptionUnit::Unknown;
    EXPECT_EQ(settlement_asset(usd), "");
}

TEST(MarginAvailable, ReadsTheAssetAndRefusesAnOldReadOrAMissingRow) {
    std::array<BalanceReport, 2> rows{};
    rows[0].asset = *AssetCode::from("USDC");
    rows[0].available = dec(5);
    rows[1].asset = *AssetCode::from("USDT");
    rows[1].available = dec(7);
    const UnixNanos pulled{1'000};
    EXPECT_EQ(margin_available(rows, "USDT", pulled, UnixNanos{1'500}, 1'000), dec(7));
    EXPECT_TRUE(margin_available(rows, "USDT", pulled, UnixNanos{5'000}, 1'000).is_undef());
    EXPECT_TRUE(margin_available(rows, "BTC", pulled, UnixNanos{1'500}, 1'000).is_undef());
}

OrderCmd sell(const Instrument& listed, std::int64_t lots, std::int64_t ticks, Side side = Side::Sell) {
    OrderCmd cmd{};
    cmd.instrument = base_of(listed).id;
    cmd.side = side;
    cmd.order_type = OrderType::Limit;
    cmd.qty = QtyLots::from_raw(lots);
    cmd.price = PriceTicks::from_raw(ticks);
    return cmd;
}

// 100 contracts of 0.01 at index 60000: strike at the money, mark and price 500, so gate reserves 9500
Exposure funded(const Instrument& listed, FixedE9 available) {
    Exposure out{.instrument = &listed};
    out.short_option.rates = short_margin_rates(VenueId::Gate, "BTC");
    out.short_option.index = dec(60'000);
    out.short_option.forward = dec(60'000);
    out.short_option.mark = dec(500);
    out.short_option.margin_available = available;
    return out;
}

RiskConfig no_buffer() {
    RiskConfig config{};
    config.short_margin_buffer_bp = 0;
    return config;
}

TEST(ShortGate, AnOpeningNeedsAMarginFigureOfItsOwn) {
    const auto listed = option(OptionUnit::Quote, PutCall::Call);
    const RiskGate gate{no_buffer()};
    const auto cmd = sell(listed, 100, 5'000);
    EXPECT_TRUE(gate.check(cmd, funded(listed, dec(9'500))).has_value());
    const auto short_by_one = gate.check(cmd, funded(listed, dec(9'499)));
    EXPECT_EQ(short_by_one.error().why, RiskReject::ShortMargin);
    EXPECT_EQ(short_by_one.error().limit, dec(9'499).raw);
    EXPECT_EQ(short_by_one.error().value, dec(9'500).raw);
    EXPECT_EQ(gate.check(cmd, funded(listed, FixedE9::undef())).error().why, RiskReject::ShortMarginUnknown);
    auto no_formula = funded(listed, dec(9'500));
    no_formula.short_option.rates = {};
    EXPECT_EQ(gate.check(cmd, no_formula).error().why, RiskReject::ShortMarginUnknown);
    auto no_mark = funded(listed, dec(9'500));
    no_mark.short_option.mark = FixedE9::undef();
    EXPECT_EQ(gate.check(cmd, no_mark).error().why, RiskReject::ShortMarginUnknown);
}

TEST(ShortGate, TheBufferAndWorkingShortsTakeFromWhatIsAvailable) {
    const auto listed = option(OptionUnit::Quote, PutCall::Call);
    RiskConfig config{};
    config.short_margin_buffer_bp = 1'000;
    const RiskGate gate{config};
    const auto cmd = sell(listed, 100, 5'000);
    EXPECT_EQ(gate.check(cmd, funded(listed, dec(9'500))).error().why, RiskReject::ShortMargin);
    EXPECT_TRUE(gate.check(cmd, funded(listed, dec(10'450))).has_value());
    auto busy = funded(listed, dec(10'450));
    busy.short_option.margin_working = dec(1);
    EXPECT_EQ(gate.check(cmd, busy).error().why, RiskReject::ShortMargin);
}

TEST(ShortGate, ClosingAShortAndSellingALongAreAlwaysAllowed) {
    const auto listed = option(OptionUnit::Quote, PutCall::Call);
    RiskConfig config = no_buffer();
    config.max_naked_short = dec(1);
    const RiskGate gate{config};
    Exposure unfunded{.instrument = &listed};
    unfunded.position_lots = QtyLots::from_raw(-100);
    unfunded.short_option.greeks_breached = true;
    EXPECT_TRUE(gate.check(sell(listed, 100, 5'000, Side::Buy), unfunded).has_value());
    Exposure longer{.instrument = &listed};
    longer.position_lots = QtyLots::from_raw(100);
    longer.short_option.greeks_breached = true;
    EXPECT_TRUE(gate.check(sell(listed, 100, 5'000), longer).has_value());
    // past the long, only the part that opens needs margin: 10 contracts are 0.1 units, so 950
    auto past = funded(listed, dec(950));
    past.position_lots = QtyLots::from_raw(90);
    EXPECT_TRUE(RiskGate{no_buffer()}.check(sell(listed, 100, 5'000), past).has_value());
    past.short_option.margin_available = dec(949);
    EXPECT_EQ(RiskGate{no_buffer()}.check(sell(listed, 100, 5'000), past).error().why, RiskReject::ShortMargin);
}

TEST(ShortGate, NakedLimitValuesCallsAtTheUnderlyingAndPutsAtTheStrikeAcrossTheBook) {
    const auto call = option(OptionUnit::Quote, PutCall::Call);
    RiskConfig config = no_buffer();
    config.max_naked_short = dec(100'000);
    const RiskGate gate{config};
    auto within = funded(call, dec(1'000'000));
    within.short_option.naked_besides = dec(40'000);
    EXPECT_TRUE(gate.check(sell(call, 100, 5'000), within).has_value());
    within.short_option.naked_besides = dec(40'001);
    const auto refused = gate.check(sell(call, 100, 5'000), within);
    EXPECT_EQ(refused.error().why, RiskReject::NakedShort);
    EXPECT_EQ(refused.error().value, dec(100'001).raw);
    within.short_option.naked_besides = FixedE9::undef();
    EXPECT_EQ(gate.check(sell(call, 100, 5'000), within).error().why, RiskReject::UnpricedExposure);

    // the put is its strike whatever the underlying does
    const auto put = option(OptionUnit::Quote, PutCall::Put);
    auto crash = funded(put, dec(1'000'000));
    crash.short_option.index = dec(1'000);
    crash.short_option.forward = dec(1'000);
    crash.short_option.naked_besides = dec(40'001);
    EXPECT_EQ(gate.check(sell(put, 100, 5'000), crash).error().why, RiskReject::NakedShort);
}

// a coin option's margin is in coin against a coin balance: 0.15 + 0.01 per btc, on one btc
TEST(ShortGate, CoinSettledMarginIsCheckedInCoin) {
    OptionInstrument coin = std::get<OptionInstrument>(option(OptionUnit::Base, PutCall::Call, VenueId::Deribit));
    coin.base.tick_size = FixedE9::from_raw(100'000); // 0.0001
    coin.strike = PriceTicks::from_raw(600'000'000);  // 60000
    const Instrument listed = coin;
    Exposure exposure{.instrument = &listed};
    exposure.short_option.rates = short_margin_rates(VenueId::Deribit, "BTC");
    exposure.short_option.index = dec(60'000);
    exposure.short_option.forward = dec(60'000);
    exposure.short_option.mark = FixedE9::from_raw(10'000'000);
    exposure.short_option.margin_available = FixedE9::from_raw(160'000'000);
    const RiskGate gate{no_buffer()};
    EXPECT_TRUE(gate.check(sell(listed, 100, 100), exposure).has_value());
    exposure.short_option.margin_available = FixedE9::from_raw(159'999'999);
    EXPECT_EQ(gate.check(sell(listed, 100, 100), exposure).error().why, RiskReject::ShortMargin);
}

TEST(ShortGate, ABookPastItsGreeksMaySellNoMore) {
    const auto listed = option(OptionUnit::Quote, PutCall::Call);
    const RiskGate gate{no_buffer()};
    auto exposure = funded(listed, dec(1'000'000));
    exposure.short_option.greeks_breached = true;
    EXPECT_EQ(gate.check(sell(listed, 100, 5'000), exposure).error().why, RiskReject::ShortGreeks);
    EXPECT_TRUE(gate.check(sell(listed, 100, 5'000, Side::Buy), exposure).has_value());
}

TEST(ShortGate, TheSameInputsReachTheSameVerdict) {
    const auto listed = option(OptionUnit::Quote, PutCall::Call);
    const RiskGate gate{no_buffer()};
    const auto exposure = funded(listed, dec(9'499));
    const auto first = gate.check(sell(listed, 100, 5'000), exposure);
    const auto second = gate.check(sell(listed, 100, 5'000), exposure);
    EXPECT_EQ(first.error().why, second.error().why);
    EXPECT_EQ(first.error().value, second.error().value);
}

} // namespace
} // namespace raikiri::oms
