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
    EXPECT_TRUE(gate.check(valid_limit(), 0).has_value());
    EXPECT_TRUE(gate.check(valid_market(), 0).has_value());
    EXPECT_TRUE(gate.check(valid_limit(), 63).has_value());
}

TEST(RiskGate, BadInstrument) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.instrument = InstrumentId{};
    const auto res = gate.check(cmd, 0);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadInstrument);
}

TEST(RiskGate, BadQty) {
    const RiskGate gate{};
    for (const auto qty : {QtyLots::from_raw(0), QtyLots::from_raw(-5), QtyLots::undef()}) {
        auto cmd = valid_limit();
        cmd.qty = qty;
        const auto res = gate.check(cmd, 0);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), RiskReject::BadQty);
    }
}

TEST(RiskGate, QtyTooLarge) {
    const RiskGate gate{RiskConfig{.max_open_orders = 64, .max_qty_lots = QtyLots::from_raw(10)}};
    EXPECT_TRUE(gate.check(valid_limit(10), 0).has_value()); // at the cap is fine
    const auto res = gate.check(valid_limit(11), 0);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::QtyTooLarge);
}

TEST(RiskGate, BadPriceOnLimitOrders) {
    const RiskGate gate{};
    for (const auto price :
         {PriceTicks::from_raw(0), PriceTicks::from_raw(-1), PriceTicks::undef()}) {
        auto cmd = valid_limit();
        cmd.price = price;
        const auto res = gate.check(cmd, 0);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), RiskReject::BadPrice);
    }
    // Market orders carry no price; undef must not trip the price rule.
    EXPECT_TRUE(gate.check(valid_market(), 0).has_value());
}

TEST(RiskGate, MarketPostOnlyIsBadFlags) {
    const RiskGate gate{};
    auto cmd = valid_market();
    cmd.flags = OrderFlags::PostOnly;
    const auto res = gate.check(cmd, 0);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadFlags);

    // The flag itself is fine where it means something.
    auto limit = valid_limit();
    limit.flags = OrderFlags::PostOnly;
    EXPECT_TRUE(gate.check(limit, 0).has_value());
    auto market = valid_market();
    market.flags = OrderFlags::ReduceOnly;
    EXPECT_TRUE(gate.check(market, 0).has_value());
}

TEST(RiskGate, TooManyOpen) {
    const RiskGate gate{};
    const auto res = gate.check(valid_limit(), 64);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::TooManyOpen);

    const RiskGate tight{RiskConfig{.max_open_orders = 2, .max_qty_lots = QtyLots::from_raw(
                                        std::numeric_limits<std::int64_t>::max())}};
    EXPECT_TRUE(tight.check(valid_limit(), 1).has_value());
    ASSERT_FALSE(tight.check(valid_limit(), 2).has_value());
    EXPECT_EQ(tight.check(valid_limit(), 2).error(), RiskReject::TooManyOpen);
}

TEST(RiskGate, MostFundamentalRejectWinsWhenSeveralApply) {
    const RiskGate gate{};
    auto cmd = valid_limit();
    cmd.instrument = InstrumentId{};
    cmd.qty = QtyLots::from_raw(0);
    cmd.price = PriceTicks::undef();
    const auto res = gate.check(cmd, 64);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), RiskReject::BadInstrument);
}

} // namespace
} // namespace raikiri::oms
