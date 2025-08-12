#include <raikiri/oms/risk.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_instrument =
    InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{2});

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
    EXPECT_EQ(gate.config().max_qty_lots,
              QtyLots::from_raw(std::numeric_limits<std::int64_t>::max()));
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
    for (const auto price :
         {PriceTicks::from_raw(0), PriceTicks::from_raw(-1), PriceTicks::undef()}) {
        auto cmd = valid_limit();
        cmd.price = price;
        const auto res = gate.check(cmd, Exposure{.open_orders = 0});
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), RiskReject::BadPrice);
    }
    // Market orders carry no price; undef must not trip the price rule.
    EXPECT_TRUE(gate.check(valid_market(), Exposure{.open_orders = 0}).has_value());
}

TEST(RiskGate, MarketPostOnlyIsBadFlags) {
    const RiskGate gate{};
    auto cmd = valid_market();
    cmd.flags = OrderFlags::PostOnly;
    const auto res = gate.check(cmd, Exposure{.open_orders = 0});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadFlags);

    // The flag itself is fine where it means something.
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

    const RiskGate tight{RiskConfig{.max_open_orders = 2, .max_qty_lots = QtyLots::from_raw(
                                        std::numeric_limits<std::int64_t>::max())}};
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
    // The failure this exists for: sixty-four buys of one lot each, every one
    // of them individually inside a ten-lot limit, all live at once.
    RiskConfig config{};
    config.max_long_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);

    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(9)}).has_value());
    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(10)}).has_value());

    // Flat, but nine lots are working. The gate must see them.
    const auto pending = gate.check(cmd, Exposure{.working_buy_lots = QtyLots::from_raw(10)});
    ASSERT_FALSE(pending.has_value());
    EXPECT_EQ(pending.error(), RiskReject::LongLimit);
}

TEST(RiskGate, OppositeSideWorkingOrdersAreNotNettedOff) {
    // Ten sells are working, so "net" exposure looks like zero. They can all
    // be cancelled, and a limit that assumes they will not be is not a limit.
    RiskConfig config{};
    config.max_long_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Buy;
    cmd.qty = QtyLots::from_raw(1);

    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(10),
                                          .working_sell_lots = QtyLots::from_raw(10)})
                     .has_value());
}

TEST(RiskGate, SellingIntoAShortIsWhatTheShortLimitBounds) {
    RiskConfig config{};
    config.max_short_lots = QtyLots::from_raw(10);
    const RiskGate gate{config};

    auto cmd = valid_limit();
    cmd.side = Side::Sell;
    cmd.qty = QtyLots::from_raw(1);

    // Short nine: one more is the last one allowed.
    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(-9)}).has_value());
    EXPECT_FALSE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(-10)}).has_value());

    // Selling while LONG reduces exposure and is never refused by this limit.
    EXPECT_TRUE(gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(1000)}).has_value());
}

TEST(RiskGate, ReduceOnlyIsCountedBecauseNothingHereEnforcesIt) {
    // Conservative on purpose: the gate cannot tell whether the venue honours
    // the flag, and trusting an unenforced one is how the other side opens.
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
    EXPECT_TRUE(
        gate.check(cmd, Exposure{.position_lots = QtyLots::from_raw(1'000'000'000)}).has_value());
}

// A break refuses before any other field is consulted: the position both
// sides disagree about is the one every other limit is measured against.
TEST(Risk, AReconciliationBreakRefusesEverythingItGates) {
    const RiskGate gate;
    const auto res = gate.check(valid_limit(), Exposure{.reconciliation_break = true});
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::ReconBreak);

    // Same command, same gate, break cleared — so the refusal above was the
    // break and not something else about the order.
    EXPECT_TRUE(gate.check(valid_limit(), Exposure{}).has_value());
}

// The band is a RATIO, so it has to mean the same thing on an instrument
// quoted in 790'000 ticks and one quoted in 13'865. It did not: the first
// draft divided the reference by 10'000 before multiplying, and on a small
// tick count that division truncated to 1 — turning a 500 bp band into
// whatever 500 ticks happened to be worth. A live XRP order 4.0 % from the mid
// was refused against a 5 % limit because of it, and the same code let a BTC
// order through at 4.94 %. The error scaled with the tick, which is precisely
// what a basis-point limit exists not to do.
TEST(RiskGate, ThePriceBandMeansTheSameAtAnyTickScale) {
    RiskGate gate{RiskConfig{.max_price_band_bp = 500}};

    const auto admits = [&gate](std::int64_t reference, std::int64_t price) {
        auto cmd = valid_limit(10, price);
        Exposure exposure{};
        exposure.reference_price = PriceTicks::from_raw(reference);
        return gate.check(cmd, exposure).has_value();
    };

    // A cheap instrument: 13'865 ticks, so 500 bp admits 693 ticks either way.
    EXPECT_TRUE(admits(13'865, 13'865 - 550));  // 3.97 %
    EXPECT_TRUE(admits(13'865, 13'865 + 550));
    EXPECT_FALSE(admits(13'865, 13'865 - 800)); // 5.77 %

    // An expensive one, in the same ratio terms.
    EXPECT_TRUE(admits(790'000, 790'000 - 39'000));  // 4.94 %
    EXPECT_FALSE(admits(790'000, 790'000 - 40'000)); // 5.06 %
}

// UNDEF IS NOT ZERO. A caller with no quote leaves the reference undefined,
// and banding against a price nobody published would refuse orders on the
// strength of a number that does not exist.
TEST(RiskGate, NoReferencePriceMeansNoBandRatherThanARefusal) {
    RiskGate gate{RiskConfig{.max_price_band_bp = 500}};
    EXPECT_TRUE(gate.check(valid_limit(10, 1), Exposure{}).has_value());
}

// A refusal that reaches a log as "unknown" is a refusal nobody can act on,
// and the only way that happens is an enumerator added without a name. The
// list is spelled out rather than iterated so adding one breaks this file.
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
