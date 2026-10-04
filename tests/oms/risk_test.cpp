#include <raikiri/oms/notional.hpp>
#include <raikiri/oms/risk.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});

OrderCmd valid_limit(std::int64_t qty = 10, std::int64_t price = 100) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = k_instrument;
    cmd.client_order_id = ClientOrderId::make(1, 1);
    cmd.price = PriceTicks::from_raw(price);
    cmd.qty = QtyLots::from_raw(qty);
    return cmd;
}

OrderCmd valid_market(std::int64_t qty = 10) {
    auto cmd = valid_limit(qty);
    cmd.order_type = OrderType::Market;
    cmd.tif = TimeInForce::Ioc;
    cmd.price = PriceTicks::undef(); // market orders carry no price
    return cmd;
}

TEST(RiskGate, DefaultConfigPassesValidOrders) {
    const RiskGate gate{};
    EXPECT_EQ(gate.config().max_open_orders, std::size_t{64});
    EXPECT_TRUE(gate.config().max_lots.perp.is_undef());
    EXPECT_FALSE(gate.config().prices_orders());
    EXPECT_TRUE(gate.check(valid_limit(), Exposure{.open_orders = 0}).has_value());
    EXPECT_TRUE(gate.check(valid_market(), Exposure{.open_orders = 0}).has_value());
    EXPECT_TRUE(gate.check(valid_limit(), Exposure{.open_orders = 63}).has_value());
}

TEST(RiskGate, BadInstrument) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.instrument = InstrumentId{};
    const auto res = gate.check(cmd, Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::BadInstrument);
}

TEST(RiskGate, BadQty) {
    const RiskGate gate{};
    for (const auto qty : {QtyLots::from_raw(0), QtyLots::from_raw(-5), QtyLots::undef()}) {
        auto cmd = valid_limit();
        cmd.qty = qty;
        const auto res = gate.check(cmd, Exposure{.open_orders = 0});
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().why, RiskReject::BadQty);
    }
}

TEST(RiskGate, TheLotCeilingIsChosenByInstrumentKind) {
    const RiskGate gate{RiskConfig{.max_lots = {.spot = QtyLots::from_raw(5), .perp = QtyLots::from_raw(10)}}};
    EXPECT_TRUE(gate.check(valid_limit(10), Exposure{.open_orders = 0}).has_value()); // at the cap is fine
    const auto res = gate.check(valid_limit(11), Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::QtyTooLarge);
    EXPECT_EQ(res.error().limit, 10);
    EXPECT_EQ(res.error().value, 11);

    auto spot = valid_limit(6);
    spot.instrument = InstrumentId::make(VenueId::Bybit, MarketType::Spot, SymbolId{2});
    EXPECT_EQ(gate.check(spot, Exposure{.open_orders = 0}).error().why, RiskReject::QtyTooLarge);
    auto future = valid_limit(1'000'000);
    future.instrument = InstrumentId::make(VenueId::Bybit, MarketType::Future, SymbolId{2});
    EXPECT_TRUE(gate.check(future, Exposure{.open_orders = 0}).has_value()) << "no ceiling set for futures";
}

TEST(RiskGate, BadPriceOnLimitOrders) {
    const RiskGate gate{};
    for (const auto price : {PriceTicks::from_raw(0), PriceTicks::from_raw(-1), PriceTicks::undef()}) {
        auto cmd = valid_limit();
        cmd.price = price;
        const auto res = gate.check(cmd, Exposure{.open_orders = 0});
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error().why, RiskReject::BadPrice);
    }
    EXPECT_TRUE(gate.check(valid_market(), Exposure{.open_orders = 0}).has_value());
}

TEST(RiskGate, MarketPostOnlyIsBadFlags) {
    const RiskGate gate{};
    auto cmd = valid_market();
    cmd.flags = OrderFlags::PostOnly;
    const auto res = gate.check(cmd, Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::BadFlags);

    auto limit = valid_limit();
    limit.flags = OrderFlags::PostOnly;
    EXPECT_TRUE(gate.check(limit, Exposure{.open_orders = 0}).has_value());
    auto market = valid_market();
    market.flags = OrderFlags::ReduceOnly;
    EXPECT_TRUE(gate.check(market, Exposure{.open_orders = 0}).has_value());
}

TEST(RiskGate, TooManyOpen) {
    const RiskGate gate{};
    const auto res = gate.check(valid_limit(), Exposure{.open_orders = 64});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::TooManyOpen);

    const RiskGate tight{RiskConfig{.max_open_orders = 2}};
    EXPECT_TRUE(tight.check(valid_limit(), Exposure{.open_orders = 1}).has_value());
    ASSERT_FALSE(tight.check(valid_limit(), Exposure{.open_orders = 2}).has_value());
    EXPECT_EQ(tight.check(valid_limit(), Exposure{.open_orders = 2}).error().why, RiskReject::TooManyOpen);
}

TEST(RiskGate, MostFundamentalRejectWinsWhenSeveralApply) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.instrument = InstrumentId{};
    cmd.qty = QtyLots::from_raw(0);
    cmd.price = PriceTicks::undef();
    const auto res = gate.check(cmd, Exposure{.open_orders = 64});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::BadInstrument);
}

TEST(RiskGate, PositionLimitCountsOrdersAlreadyOnTheWire) {
    RiskConfig config{};
    config.max_long_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);

    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(9)}).has_value());
    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(10)}).has_value());

    const auto pending = gate.check(cmd, Exposure{.working_buy_lots = QtyLots::from_raw(10)});
    ASSERT_FALSE(pending.has_value());
    EXPECT_EQ(pending.error().why, RiskReject::LongLimit);
}

TEST(RiskGate, OppositeSideWorkingOrdersAreNotNettedOff) {
    RiskConfig config{};
    config.max_long_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);

    EXPECT_FALSE(
        gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(10), .working_sell_lots = QtyLots::from_raw(10)})
            .has_value());
}

TEST(RiskGate, SellingIntoAShortIsWhatTheShortLimitBounds) {
    RiskConfig config{};
    config.max_short_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Sell;
    cmd.qty = QtyLots::from_raw(1);

    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(-9)}).has_value());
    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(-10)}).has_value());

    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(1000)}).has_value());
}

TEST(RiskGate, ReduceOnlyIsCountedBecauseNothingHereEnforcesIt) {
    RiskConfig config{};
    config.max_long_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);
    cmd.flags = OrderFlags::ReduceOnly;

    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(10)}).has_value());
}

TEST(RiskGate, NoLimitConfiguredMeansNoLimitApplied) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.qty = QtyLots::from_raw(1'000'000);
    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(1'000'000'000)}).has_value());
}

TEST(Risk, AReconciliationBreakRefusesEverythingItGates) {
    const RiskGate gate;
    const auto res = gate.check(valid_limit(), Exposure{.reconciliation_break = true});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::ReconBreak);

    EXPECT_TRUE(gate.check(valid_limit(), Exposure{}).has_value());
}

TEST(RiskGate, ThePriceBandMeansTheSameAtAnyTickScale) {
    RiskGate gate{RiskConfig{.max_price_band_bp = 500}};

    const auto admits = [&gate](std::int64_t reference, std::int64_t price) {
        auto cmd = valid_limit(10, price);
        Exposure exposure{};
        exposure.reference_price = PriceTicks::from_raw(reference);
        return gate.check(cmd, exposure).has_value();
    };

    // 13'865 ticks, so 500 bp admits 693 ticks either way
    EXPECT_TRUE(admits(13'865, 13'865 - 550)); // 3.97 %
    EXPECT_TRUE(admits(13'865, 13'865 + 550));
    EXPECT_FALSE(admits(13'865, 13'865 - 800)); // 5.77 %

    EXPECT_TRUE(admits(790'000, 790'000 - 39'000));  // 4.94 %
    EXPECT_FALSE(admits(790'000, 790'000 - 40'000)); // 5.06 %
}

TEST(RiskGate, NoReferencePriceMeansNoBandRatherThanARefusal) {
    RiskGate gate{RiskConfig{.max_price_band_bp = 500}};
    EXPECT_TRUE(gate.check(valid_limit(10, 1), Exposure{}).has_value());
}

// 0.001 base per lot, tick 0.1: price 1000 ticks is 100 quote, so one lot is worth 0.1
Instrument linear_perp() {
    PerpInstrument out{};
    out.base.id = k_instrument;
    out.base.kind = ContractKind::Linear;
    out.base.tick_size = FixedE9::from_raw(100'000'000);
    out.base.lot_size = FixedE9::from_raw(1'000'000);
    out.base.contract_value = FixedE9::from_raw(k_e9);
    return out;
}

// one lot is one contract of 100 quote, whatever the price
Instrument inverse_perp() {
    PerpInstrument out{};
    out.base.id = k_instrument;
    out.base.kind = ContractKind::Inverse;
    out.base.tick_size = FixedE9::from_raw(500'000'000);
    out.base.lot_size = FixedE9::from_raw(k_e9);
    out.base.contract_value = FixedE9::from_raw(100 * k_e9);
    return out;
}

constexpr FixedE9 quote(std::int64_t whole) {
    return FixedE9::from_raw(whole * k_e9);
}

TEST(Notional, LinearIsLotsTimesLotSizeTimesPrice) {
    const auto value = notional(linear_perp(), PriceTicks::from_raw(1'000), QtyLots::from_raw(50));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(to_e9(*value), quote(5));
    EXPECT_EQ(notional(linear_perp(), PriceTicks::undef(), QtyLots::from_raw(1)).error(), NotionalError::NoPrice);
}

TEST(Notional, InverseIsTheContractsFaceAtAnyPrice) {
    for (const auto price : {PriceTicks::from_raw(1), PriceTicks::from_raw(1'000'000), PriceTicks::undef()}) {
        const auto value = notional(inverse_perp(), price, QtyLots::from_raw(3));
        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(to_e9(*value), quote(300));
    }
}

TEST(Notional, AnOptionIsValuedOnlyWhenItsPremiumIsInQuote) {
    OptionInstrument option{.base = base_of(linear_perp()), .premium = OptionUnit::Quote};
    const auto premium = notional(option, PriceTicks::from_raw(1'000), QtyLots::from_raw(10));
    ASSERT_TRUE(premium.has_value());
    EXPECT_EQ(to_e9(*premium), quote(1));
    option.premium = OptionUnit::Base;
    EXPECT_EQ(notional(option, PriceTicks::from_raw(1'000), QtyLots::from_raw(10)).error(), NotionalError::Unpriceable);
    PerpInstrument unknown{.base = base_of(linear_perp())};
    unknown.base.kind = ContractKind::Unknown;
    EXPECT_EQ(notional(unknown, PriceTicks::from_raw(1'000), QtyLots::from_raw(1)).error(), NotionalError::Unpriceable);
}

TEST(Notional, SaturatesInsteadOfWrapping) {
    const auto value = notional(linear_perp(),
                                PriceTicks::from_raw(std::numeric_limits<std::int64_t>::max()),
                                QtyLots::from_raw(std::numeric_limits<std::int64_t>::max()));
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(to_e9(*value).raw, std::numeric_limits<std::int64_t>::max());
}

TEST(RiskGate, PerOrderNotionalRefusesOnlyAboveTheLimit) {
    const auto perp = linear_perp();
    const RiskGate gate{RiskConfig{.max_order_notional = quote(100)}};
    // 1000 lots at 100 quote is exactly 100
    EXPECT_TRUE(gate.check(valid_limit(1'000, 1'000), Exposure{.instrument = &perp}).has_value());
    const auto res = gate.check(valid_limit(1'001, 1'000), Exposure{.instrument = &perp});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::OrderNotional);
    EXPECT_EQ(res.error().limit, quote(100).raw);
    EXPECT_EQ(res.error().order_notional, FixedE9::from_raw(100'100'000'000));
    EXPECT_EQ(describe(res.error()), "per-order notional limit 100, would reach 100.1, order notional 100.1");
}

TEST(RiskGate, InverseOrdersAreCountedInContractsNotPrice) {
    const auto inverse = inverse_perp();
    const RiskGate gate{RiskConfig{.max_order_notional = quote(1'000)}};
    EXPECT_TRUE(gate.check(valid_limit(10, 1), Exposure{.instrument = &inverse}).has_value());
    EXPECT_TRUE(gate.check(valid_limit(10, 1'000'000), Exposure{.instrument = &inverse}).has_value());
    EXPECT_EQ(gate.check(valid_limit(11, 1), Exposure{.instrument = &inverse}).error().why, RiskReject::OrderNotional);
    // a market order on an inverse contract needs no touch
    EXPECT_TRUE(gate.check(valid_market(10), Exposure{.instrument = &inverse}).has_value());
}

TEST(RiskGate, AMarketOrderIsValuedAtTheTouchOrRefused) {
    const auto perp = linear_perp();
    const RiskGate gate{RiskConfig{.max_order_notional = quote(100)}};
    const auto res = gate.check(valid_market(10), Exposure{.instrument = &perp});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::NoPrice);
    // a buy pays the ask
    const Exposure quoted{.instrument = &perp, .bid = PriceTicks::from_raw(1'000), .ask = PriceTicks::from_raw(2'000)};
    EXPECT_TRUE(gate.check(valid_market(500), quoted).has_value());
    EXPECT_EQ(gate.check(valid_market(501), quoted).error().why, RiskReject::OrderNotional);
    auto sell = valid_market(1'000);
    sell.side = Side::Sell;
    EXPECT_TRUE(gate.check(sell, quoted).has_value());
}

TEST(RiskGate, ANotionalLimitWithNoInstrumentRefuses) {
    const RiskGate gate{RiskConfig{.max_order_notional = quote(100)}};
    EXPECT_EQ(gate.check(valid_limit(), Exposure{}).error().why, RiskReject::Unpriceable);
}

TEST(RiskGate, OpenNotionalCountsWorkingOrdersAndPosition) {
    const auto perp = linear_perp();
    const RiskGate gate{RiskConfig{.max_instrument_notional = quote(100)}};
    // 400 held and 400 working, so 200 more reaches 1000 lots, 100 quote at 1000 ticks
    Exposure held{
        .position_lots = QtyLots::from_raw(400), .working_buy_lots = QtyLots::from_raw(400), .instrument = &perp};
    EXPECT_TRUE(gate.check(valid_limit(200, 1'000), held).has_value());
    const auto res = gate.check(valid_limit(201, 1'000), held);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::InstrumentNotional);
    EXPECT_EQ(res.error().value, FixedE9::from_raw(100'100'000'000).raw);
    EXPECT_EQ(res.error().order_notional, FixedE9::from_raw(20'100'000'000));

    // a sell that only reduces the long passes even over the limit
    held.position_lots = QtyLots::from_raw(2'000);
    auto reduce = valid_limit(1'500, 1'000);
    reduce.side = Side::Sell;
    EXPECT_TRUE(gate.check(reduce, held).has_value());
}

TEST(RiskGate, StrategyNotionalAddsTheOtherInstruments) {
    const auto perp = linear_perp();
    const RiskGate gate{RiskConfig{.max_strategy_notional = quote(100)}};
    Exposure exposure{.instrument = &perp, .strategy_open_notional = quote(60)};
    EXPECT_TRUE(gate.check(valid_limit(400, 1'000), exposure).has_value());
    const auto res = gate.check(valid_limit(401, 1'000), exposure);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().why, RiskReject::StrategyNotional);
    EXPECT_EQ(res.error().value, FixedE9::from_raw(100'100'000'000).raw);

    exposure.strategy_open_notional = FixedE9::undef();
    EXPECT_EQ(gate.check(valid_limit(1, 1'000), exposure).error().why, RiskReject::UnpricedExposure);
}

// the 1000-lot constant refused 0.001-base lots worth 1 quote, which a lot count cannot tell from a whale
TEST(RiskGate, ThereIsNoThousandLotCap) {
    const auto perp = linear_perp();
    const RiskGate unlimited{};
    EXPECT_TRUE(unlimited.check(valid_limit(1'000'000), Exposure{}).has_value());
    const RiskGate notional{RiskConfig{.max_order_notional = quote(5'000)}};
    EXPECT_TRUE(notional.check(valid_limit(50'000, 1'000), Exposure{.instrument = &perp}).has_value());
}

// listed explicitly so a new enumerator breaks this file
TEST(RiskGate, EveryRefusalCanNameItself) {
    using enum RiskReject;
    for (const auto why : {ReconBreak,
                           TooManyOpen,
                           BadQty,
                           QtyTooLarge,
                           BadPrice,
                           BadInstrument,
                           BadFlags,
                           LongLimit,
                           ShortLimit,
                           PriceBand,
                           NoPrice,
                           Unpriceable,
                           UnpricedExposure,
                           OrderNotional,
                           InstrumentNotional,
                           StrategyNotional}) {
        EXPECT_NE(to_string(why), "unknown") << "unnamed RiskReject";
        EXPECT_FALSE(to_string(why).empty());
    }
    EXPECT_EQ(to_string(static_cast<RiskReject>(200)), "unknown");
}

} // namespace
} // namespace raikiri::oms
