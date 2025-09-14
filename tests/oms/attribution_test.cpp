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
    // Created, sent and acknowledged. The FSM refuses a fill on an order that
    // never went out, and skipping the local Submitted transition is exactly
    // how orders once stayed open forever.
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
    // The bug this exists for: a venue knows an order only by the id we sent
    // it, so a fill off the private stream leaves client_order_id at zero. A
    // lookup by that id finds nothing, the delta comes out zero, and the whole
    // booking is skipped in silence.
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

TEST_F(AttributionTest, AResentFillMovesNoMoney) {
    // The reason the delta is the DROP in leaves and never the event's own
    // qty: a venue that repeats a notification states the same qty again, and
    // deriving the position from it would book the fill twice.
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
    // An amend that grows an order raises its leaves. The difference is
    // negative, and reading it as a fill would credit a position the venue
    // never gave us.
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
