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
    EXPECT_EQ(gate.config().max_qty_lots, QtyLots::from_raw(std::numeric_limits<std::int64_t>::max()));
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
    EXPECT_EQ(res.error(), RiskReject::BadInstrument);
}

TEST(RiskGate, BadQty) {
    const RiskGate gate{};
    for (const auto qty : {QtyLots::from_raw(0), QtyLots::from_raw(-5), QtyLots::undef()}) {
        auto cmd = valid_limit();
        cmd.qty = qty;
        const auto res = gate.check(cmd, Exposure{.open_orders = 0});
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), RiskReject::BadQty);
    }
}

TEST(RiskGate, QtyTooLarge) {
    const RiskGate gate{RiskConfig{.max_open_orders = 64, .max_qty_lots = QtyLots::from_raw(10)}};
    EXPECT_TRUE(gate.check(valid_limit(10), Exposure{.open_orders = 0}).has_value()); // at the cap is fine
    const auto res = gate.check(valid_limit(11), Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::QtyTooLarge);
}

TEST(RiskGate, BadPriceOnLimitOrders) {
    const RiskGate gate{};
    for (const auto price : {PriceTicks::from_raw(0), PriceTicks::from_raw(-1), PriceTicks::undef()}) {
        auto cmd = valid_limit();
        cmd.price = price;
        const auto res = gate.check(cmd, Exposure{.open_orders = 0});
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), RiskReject::BadPrice);
    }
    EXPECT_TRUE(gate.check(valid_market(), Exposure{.open_orders = 0}).has_value());
}

TEST(RiskGate, MarketPostOnlyIsBadFlags) {
    const RiskGate gate{};
    auto cmd = valid_market();
    cmd.flags = OrderFlags::PostOnly;
    const auto res = gate.check(cmd, Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadFlags);

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
    EXPECT_EQ(res.error(), RiskReject::TooManyOpen);

    const RiskGate tight{
        RiskConfig{.max_open_orders = 2, .max_qty_lots = QtyLots::from_raw(std::numeric_limits<std::int64_t>::max())}};
    EXPECT_TRUE(tight.check(valid_limit(), Exposure{.open_orders = 1}).has_value());
    ASSERT_FALSE(tight.check(valid_limit(), Exposure{.open_orders = 2}).has_value());
    EXPECT_EQ(tight.check(valid_limit(), Exposure{.open_orders = 2}).error(), RiskReject::TooManyOpen);
}

TEST(RiskGate, MostFundamentalRejectWinsWhenSeveralApply) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.instrument = InstrumentId{};
    cmd.qty = QtyLots::from_raw(0);
    cmd.price = PriceTicks::undef();
    const auto res = gate.check(cmd, Exposure{.open_orders = 64});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadInstrument);
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
    EXPECT_EQ(pending.error(), RiskReject::LongLimit);
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
    EXPECT_EQ(res.error(), RiskReject::ReconBreak);

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
                           PriceBand}) {
        EXPECT_NE(to_string(why), "unknown") << "unnamed RiskReject";
        EXPECT_FALSE(to_string(why).empty());
    }
    EXPECT_EQ(to_string(static_cast<RiskReject>(200)), "unknown");
}

} // namespace
} // namespace raikiri::oms
