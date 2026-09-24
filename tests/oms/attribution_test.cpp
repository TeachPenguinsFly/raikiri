#include <raikiri/oms/attribution.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order_store.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_instrument =
    InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});

OrderCmd make_cmd(ClientOrderId cid, std::int64_t qty) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = k_instrument;
    cmd.client_order_id = cid;
    cmd.price = PriceTicks::from_raw(100);
    cmd.qty = QtyLots::from_raw(qty);
    cmd.ts_created = UnixNanos{1'000};
    return cmd;
}

class AttributionTest : public ::testing::Test {
protected:
    Order* place(std::int64_t qty) {
        const auto cid = ids_.next_client_id();
        wall_.ns += 1'000'000;
        const auto res = store_.create(make_cmd(cid, qty), ids_.next_exchange_client_id(wall_));
        if (!res.has_value()) {
            return nullptr;
        }
        for (const auto type : {OrderEventType::Submitted, OrderEventType::Accepted}) {
            OrderEvent ev{};
            ev.type = type;
            ev.client_order_id = cid;
            ev.ts_event = UnixNanos{2'000};
            if (!store_.apply_event(ev).has_value()) {
                return nullptr;
            }
        }
        return *res;
    }

    OrderStore store_{16};
    IdGen ids_{7, 42};
    UnixNanos wall_{1'700'000'000'000'000'000ULL};
};

TEST_F(AttributionTest, AVenueFillNamesTheOrderByOurExchangeIdAndCarriesNoIdOfOurs) {
    Order* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.exchange_client_order_id = order->exchange_client_order_id();
    event.leaves_qty = QtyLots::from_raw(4);
    ASSERT_EQ(event.client_order_id.raw, 0U) << "this is what a venue actually sends";

    const auto who = attribute(event, store_);
    ASSERT_NE(who.order, nullptr);
    EXPECT_EQ(event.client_order_id, order->client_order_id())
        << "restored onto the event before anything downstream reads it";
    EXPECT_EQ(who.prev_leaves, QtyLots::from_raw(10));
    EXPECT_EQ(filled_delta(who, event), QtyLots::from_raw(6));
}

TEST_F(AttributionTest, OurOwnIdStillResolves) {
    Order* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.client_order_id = order->client_order_id();
    event.leaves_qty = QtyLots::from_raw(0);

    const auto who = attribute(event, store_);
    ASSERT_NE(who.order, nullptr);
    EXPECT_EQ(filled_delta(who, event), QtyLots::from_raw(10));
}

TEST_F(AttributionTest, AnOrderThisProcessNeverHeldIsSaidSoRatherThanBookedAsZero) {
    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.exchange_client_order_id = *ExchangeClientOrderId::from("someone-elses-order");
    event.leaves_qty = QtyLots::from_raw(0);

    const auto who = attribute(event, store_);
    EXPECT_EQ(who.order, nullptr) << "the caller must report this, not skip it";
    EXPECT_TRUE(who.prev_leaves.is_undef()) << "unknown leaves, not zero leaves";
    EXPECT_EQ(filled_delta(who, event), QtyLots::from_raw(0));
    EXPECT_EQ(event.client_order_id.raw, 0U) << "nothing to restore, and nothing invented";
}

TEST_F(AttributionTest, AFillNamingOnlyTheVenuesOwnOrderIdStillFindsTheOrder) {
    // weex user trades name only the venue's orderId
    auto* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent acked{};
    acked.type = OrderEventType::Filled;
    acked.client_order_id = order->client_order_id();
    acked.exchange_order_id = *ExchangeOrderId::from("794687307704697193");
    acked.qty = QtyLots::from_raw(0);
    acked.leaves_qty = QtyLots::from_raw(10); // the venue naming its id, nothing filled
    ASSERT_TRUE(store_.apply_event(acked).has_value());

    OrderEvent pulled{};
    pulled.type = OrderEventType::Filled;
    pulled.exchange_order_id = *ExchangeOrderId::from("794687307704697193");
    pulled.qty = QtyLots::from_raw(10);
    pulled.leaves_qty = QtyLots::undef();
    pulled.ts_event = UnixNanos{4'000};
    ASSERT_TRUE(pulled.exchange_client_order_id.empty()) << "the venue names none";

    const auto who = attribute(pulled, store_);
    ASSERT_NE(who.order, nullptr) << "found by the venue's own order id";
    EXPECT_EQ(pulled.client_order_id, order->client_order_id()) << "and ours restored onto it";
    EXPECT_EQ(filled_delta(who, pulled), QtyLots::from_raw(10));
}

TEST_F(AttributionTest, APulledFillStatesItsOwnSizeBecauseItKnowsNothingOfTheOrder) {
    auto* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent pulled{};
    pulled.type = OrderEventType::Filled;
    pulled.exchange_client_order_id = order->exchange_client_order_id();
    pulled.qty = QtyLots::from_raw(4);
    pulled.leaves_qty = QtyLots::undef(); // the pull states none
    pulled.ts_event = UnixNanos{3'000};

    const auto who = attribute(pulled, store_);
    ASSERT_NE(who.order, nullptr);
    EXPECT_EQ(filled_delta(who, pulled), QtyLots::from_raw(4)) << "the execution's own size";
}

TEST_F(AttributionTest, APulledFillCannotTakeMoreThanTheOrderHadLeft) {
    auto* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent pulled{};
    pulled.type = OrderEventType::Filled;
    pulled.exchange_client_order_id = order->exchange_client_order_id();
    pulled.qty = QtyLots::from_raw(25); // more than the order ever was
    pulled.leaves_qty = QtyLots::undef();
    pulled.ts_event = UnixNanos{3'000};

    const auto who = attribute(pulled, store_);
    EXPECT_EQ(filled_delta(who, pulled), QtyLots::from_raw(10)) << "capped at what was open";
}

TEST(SeenFills, TheSameExecutionIsTakenOnceAndThenRefused) {
    SeenFills seen;
    const UnixNanos now{1'000};
    EXPECT_TRUE(seen.admit("220574436", now));
    EXPECT_FALSE(seen.admit("220574436", now)) << "the second offer of the same fill";
    EXPECT_TRUE(seen.admit("220574437", now)) << "a different execution still passes";
    EXPECT_EQ(seen.size(), 2U);
}

TEST(SeenFills, AnExecutionWithNoIdIsAlwaysAdmitted) {
    SeenFills seen;
    const UnixNanos now{1'000};
    EXPECT_TRUE(seen.admit("", now));
    EXPECT_TRUE(seen.admit("", now));
    EXPECT_TRUE(seen.admit("", now));
    EXPECT_EQ(seen.size(), 0U) << "and none of them is remembered";
}

TEST(SeenFills, WhatNoPullWindowCanReachBackOverIsForgotten) {
    SeenFills seen;
    EXPECT_TRUE(seen.admit("old", UnixNanos{1'000}));
    EXPECT_TRUE(seen.admit("new", UnixNanos{9'000}));
    seen.prune(UnixNanos{10'000}, /*window_ns=*/5'000);
    EXPECT_EQ(seen.size(), 1U);
    EXPECT_TRUE(seen.admit("old", UnixNanos{10'000})) << "forgotten, so admitted again";
    EXPECT_FALSE(seen.admit("new", UnixNanos{10'000})) << "still inside the window";
}

TEST(SeenFills, AClockInsideTheWindowDropsNothing) {
    SeenFills seen;
    EXPECT_TRUE(seen.admit("a", UnixNanos{10}));
    seen.prune(UnixNanos{20}, /*window_ns=*/1'000'000);
    EXPECT_EQ(seen.size(), 1U);
}

TEST_F(AttributionTest, AResentFillMovesNoMoney) {
    Order* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent first{};
    first.type = OrderEventType::Filled;
    first.exchange_client_order_id = order->exchange_client_order_id();
    first.qty = QtyLots::from_raw(6);
    first.leaves_qty = QtyLots::from_raw(4);
    const auto once = attribute(first, store_);
    EXPECT_EQ(filled_delta(once, first), QtyLots::from_raw(6));
    ASSERT_TRUE(store_.apply_event(first).has_value());

    OrderEvent again = first;
    again.client_order_id = ClientOrderId{};
    const auto twice = attribute(again, store_);
    ASSERT_NE(twice.order, nullptr);
    EXPECT_EQ(filled_delta(twice, again), QtyLots::from_raw(0))
        << "the leaves already fell; the second telling moves nothing";
}

TEST_F(AttributionTest, LeavesThatROSEAreAnAmendAndNotAFill) {
    Order* order = place(10);
    ASSERT_NE(order, nullptr);

    OrderEvent event{};
    event.type = OrderEventType::Filled;
    event.exchange_client_order_id = order->exchange_client_order_id();
    event.leaves_qty = QtyLots::from_raw(15);

    const auto who = attribute(event, store_);
    ASSERT_NE(who.order, nullptr);
    EXPECT_EQ(filled_delta(who, event), QtyLots::from_raw(0));
}

} // namespace
} // namespace raikiri::oms
