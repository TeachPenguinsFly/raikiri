#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/order.hpp>
#include <raikiri/oms/order_store.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace raikiri::oms {
namespace {

constexpr InstrumentId k_instrument =
    InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});

OrderCmd make_cmd(ClientOrderId cid, std::int64_t qty = 10, std::int64_t price = 100) {
    OrderCmd cmd{};
    cmd.kind = CmdKind::Create;
    cmd.side = Side::Buy;
    cmd.order_type = OrderType::Limit;
    cmd.tif = TimeInForce::Gtc;
    cmd.instrument = k_instrument;
    cmd.client_order_id = cid;
    cmd.price = PriceTicks::from_raw(price);
    cmd.qty = QtyLots::from_raw(qty);
    cmd.ts_created = UnixNanos{1'000};
    return cmd;
}

OrderEvent client_event(OrderEventType type, ClientOrderId cid) {
    OrderEvent ev{};
    ev.type = type;
    ev.client_order_id = cid;
    ev.ts_event = UnixNanos{2'000};
    return ev;
}

OrderEvent ecoid_event(OrderEventType type, const ExchangeClientOrderId& ecoid) {
    OrderEvent ev{};
    ev.type = type;
    ev.exchange_client_order_id = ecoid;
    ev.ts_event = UnixNanos{2'000};
    return ev;
}

OrderEvent exchange_event(OrderEventType type, std::string_view exchange_id) {
    OrderEvent ev{};
    ev.type = type;
    ev.exchange_order_id = *ExchangeOrderId::from(exchange_id);
    ev.ts_event = UnixNanos{2'000};
    return ev;
}

OrderEvent fill_event(ClientOrderId cid, std::int64_t fill_qty, std::int64_t leaves) {
    auto ev = client_event(OrderEventType::Filled, cid);
    ev.qty = QtyLots::from_raw(fill_qty);
    ev.leaves_qty = QtyLots::from_raw(leaves);
    return ev;
}

class OrderStoreTest : public ::testing::Test {
protected:
    Order* mk(std::int64_t qty = 10) {
        const auto cid = ids_.next_client_id();
        const auto res = store_.create(make_cmd(cid, qty), next_ecoid());
        return res.has_value() ? *res : nullptr;
    }

    ExchangeClientOrderId next_ecoid() {
        wall_.ns += 1'000'000;
        return ids_.next_exchange_client_id(wall_);
    }

    OrderStore store_{16};
    IdGen ids_{7, 42};
    UnixNanos wall_{1'700'000'000'000'000'000ULL};
};

TEST_F(OrderStoreTest, CreateInitializesFromCmd) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->status(), OrderStatus::Initialized);
    EXPECT_EQ(order->instrument(), k_instrument);
    EXPECT_EQ(order->qty(), QtyLots::from_raw(10));
    EXPECT_EQ(order->leaves(), QtyLots::from_raw(10));
    EXPECT_EQ(order->filled(), QtyLots::from_raw(0));
    EXPECT_TRUE(order->exchange_order_id().empty());
    EXPECT_EQ(order->ts_created(), UnixNanos{1'000});
    EXPECT_EQ(store_.open_count(), std::size_t{1});
    EXPECT_EQ(store_.find(order->client_order_id()), order);
    EXPECT_EQ(store_.find(order->exchange_client_order_id()), order);
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("nope")), nullptr);
}

TEST_F(OrderStoreTest, FullLifecycleRecyclesSlot) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    const auto ecoid = order->exchange_client_order_id();

    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::Submitted);

    auto ack = client_event(OrderEventType::Accepted, cid);
    ack.exchange_order_id = *ExchangeOrderId::from("EX-1");
    ASSERT_TRUE(store_.apply_event(ack).has_value());
    EXPECT_EQ(order->status(), OrderStatus::Accepted);
    EXPECT_EQ(order->exchange_order_id().view(), "EX-1");
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("EX-1")), order);

    ASSERT_TRUE(store_.apply_event(fill_event(cid, 4, 6)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(order->filled(), QtyLots::from_raw(4));
    EXPECT_EQ(order->leaves(), QtyLots::from_raw(6));

    const auto done = store_.apply_event(fill_event(cid, 6, 0));
    ASSERT_TRUE(done.has_value());
    // the slot is recycled but the pointer stays readable until the next create()
    EXPECT_EQ((*done)->status(), OrderStatus::Filled);
    EXPECT_EQ((*done)->filled(), QtyLots::from_raw(10));
    EXPECT_EQ((*done)->leaves(), QtyLots::from_raw(0));
    EXPECT_EQ(store_.open_count(), std::size_t{0});
    EXPECT_EQ(store_.find(cid), nullptr);
    EXPECT_EQ(store_.find(ecoid), nullptr);
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("EX-1")), nullptr);
}

TEST_F(OrderStoreTest, FastFillBeforeAckRoutesByEcoid) {
    Order* order = mk(5);
    ASSERT_NE(order, nullptr);
    const auto ecoid = order->exchange_client_order_id();
    ASSERT_TRUE(
        store_.apply_event(client_event(OrderEventType::Submitted, order->client_order_id()))
            .has_value());

    auto fill = ecoid_event(OrderEventType::Filled, ecoid);
    fill.qty = QtyLots::from_raw(5);
    fill.leaves_qty = QtyLots::from_raw(0);
    fill.exchange_order_id = *ExchangeOrderId::from("EX-FAST");
    const auto done = store_.apply_event(fill);
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ((*done)->status(), OrderStatus::Filled);
    EXPECT_EQ(store_.open_count(), std::size_t{0});
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("EX-FAST")), nullptr);
}

TEST_F(OrderStoreTest, CancelRejectedResumesPartiallyFilled) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Accepted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(fill_event(cid, 3, 7)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PartiallyFilled);

    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::PendingCancel, cid)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PendingCancel);

    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::CancelRejected, cid)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(store_.open_count(), std::size_t{1});
}

TEST_F(OrderStoreTest, FillDuringPendingCancelUpdatesResumeTarget) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Accepted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::PendingCancel, cid)).has_value());
    EXPECT_EQ(order->prior_working(), OrderStatus::Accepted);

    ASSERT_TRUE(store_.apply_event(fill_event(cid, 4, 6)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PendingCancel);
    EXPECT_EQ(order->prior_working(), OrderStatus::PartiallyFilled);

    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::CancelRejected, cid)).has_value());
    EXPECT_EQ(order->status(), OrderStatus::PartiallyFilled);
    EXPECT_EQ(order->filled(), QtyLots::from_raw(4));
}

TEST_F(OrderStoreTest, FullFillWinsPendingCancelRace) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Accepted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::PendingCancel, cid)).has_value());

    const auto done = store_.apply_event(fill_event(cid, 10, 0));
    ASSERT_TRUE(done.has_value());
    EXPECT_EQ((*done)->status(), OrderStatus::Filled);
    EXPECT_EQ(store_.open_count(), std::size_t{0});
}

TEST_F(OrderStoreTest, RoutesByExchangeIdAfterBind) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    auto ack = client_event(OrderEventType::Accepted, cid);
    ack.exchange_order_id = *ExchangeOrderId::from("EX-9");
    ASSERT_TRUE(store_.apply_event(ack).has_value());

    const auto res = store_.apply_event(exchange_event(OrderEventType::Canceled, "EX-9"));
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ((*res)->status(), OrderStatus::Canceled);
    EXPECT_EQ(store_.open_count(), std::size_t{0});
}

TEST_F(OrderStoreTest, UnknownIdsReportNotFound) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);

    const auto unknown_cid =
        store_.apply_event(client_event(OrderEventType::Accepted, ClientOrderId::make(9, 999)));
    ASSERT_FALSE(unknown_cid.has_value());
    EXPECT_EQ(unknown_cid.error(), ApplyError::NotFound);

    const auto unknown_ecoid = store_.apply_event(
        ecoid_event(OrderEventType::Accepted, *Ulid::from("0123456789ABCDEFGHJKMNPQRS")));
    ASSERT_FALSE(unknown_ecoid.has_value());
    EXPECT_EQ(unknown_ecoid.error(), ApplyError::NotFound);

    const auto unknown_exchange =
        store_.apply_event(exchange_event(OrderEventType::Canceled, "EX-UNKNOWN"));
    ASSERT_FALSE(unknown_exchange.has_value());
    EXPECT_EQ(unknown_exchange.error(), ApplyError::NotFound);

    OrderEvent no_ids{};
    no_ids.type = OrderEventType::Accepted;
    const auto anonymous = store_.apply_event(no_ids);
    ASSERT_FALSE(anonymous.has_value());
    EXPECT_EQ(anonymous.error(), ApplyError::NotFound);

    EXPECT_EQ(store_.open_count(), std::size_t{1});
}

TEST_F(OrderStoreTest, EventAfterTerminalReportsNotFound) {
    Order* order = mk(5);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    ASSERT_TRUE(store_.apply_event(fill_event(cid, 5, 0)).has_value());

    const auto late = store_.apply_event(fill_event(cid, 5, 0));
    ASSERT_FALSE(late.has_value());
    EXPECT_EQ(late.error(), ApplyError::NotFound);
}

TEST_F(OrderStoreTest, BadTransitionLeavesOrderUntouched) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto res =
        store_.apply_event(client_event(OrderEventType::Accepted, order->client_order_id()));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), ApplyError::BadTransition);
    EXPECT_EQ(order->status(), OrderStatus::Initialized);
    EXPECT_EQ(store_.open_count(), std::size_t{1});
}

TEST_F(OrderStoreTest, DuplicateClientOrderIdRejected) {
    const auto cid = ids_.next_client_id();
    ASSERT_TRUE(store_.create(make_cmd(cid), next_ecoid()).has_value());
    const auto dup = store_.create(make_cmd(cid), next_ecoid());
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error(), StoreError::Duplicate);
    EXPECT_EQ(store_.open_count(), std::size_t{1});
}

TEST_F(OrderStoreTest, DuplicateEcoidRejected) {
    const auto ecoid = next_ecoid();
    ASSERT_TRUE(store_.create(make_cmd(ids_.next_client_id()), ecoid).has_value());
    const auto dup = store_.create(make_cmd(ids_.next_client_id()), ecoid);
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error(), StoreError::Duplicate);
}

TEST_F(OrderStoreTest, DuplicateAckIsIdempotent) {
    Order* order = mk(10);
    ASSERT_NE(order, nullptr);
    const auto cid = order->client_order_id();
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Submitted, cid)).has_value());
    auto ack = client_event(OrderEventType::Accepted, cid);
    ack.exchange_order_id = *ExchangeOrderId::from("EX-2");
    ASSERT_TRUE(store_.apply_event(ack).has_value());
    ASSERT_TRUE(store_.apply_event(ack).has_value());
    EXPECT_EQ(order->status(), OrderStatus::Accepted);
    EXPECT_EQ(store_.find_by_exchange_id(*ExchangeOrderId::from("EX-2")), order);
    EXPECT_EQ(store_.open_count(), std::size_t{1});
}

TEST_F(OrderStoreTest, ForEachOpenVisitsExactlyTheLiveSet) {
    Order* a = mk(1);
    Order* b = mk(2);
    Order* c = mk(3);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(store_.apply_event(client_event(OrderEventType::Canceled, b->client_order_id()))
                    .has_value());

    std::vector<ClientOrderId> seen;
    store_.for_each_open([&seen](const Order& order) { seen.push_back(order.client_order_id()); });
    EXPECT_EQ(seen.size(), std::size_t{2});
    EXPECT_EQ(store_.open_count(), std::size_t{2});
    for (const auto cid : seen) {
        EXPECT_NE(cid, b->client_order_id());
    }
}

TEST(OrderStoreCapacity, FullThenFreeListReuse) {
    OrderStore store{4};
    IdGen ids{1, 99};
    const UnixNanos wall{1};
    std::array<ClientOrderId, 4> cids{};
    for (auto& cid : cids) {
        cid = ids.next_client_id();
        ASSERT_TRUE(store.create(make_cmd(cid), ids.next_exchange_client_id(wall)).has_value());
    }
    EXPECT_EQ(store.open_count(), std::size_t{4});

    const auto full =
        store.create(make_cmd(ids.next_client_id()), ids.next_exchange_client_id(wall));
    ASSERT_FALSE(full.has_value());
    EXPECT_EQ(full.error(), StoreError::Full);

    ASSERT_TRUE(store.apply_event(client_event(OrderEventType::Canceled, cids[0])).has_value());
    EXPECT_EQ(store.open_count(), std::size_t{3});
    const auto reused =
        store.create(make_cmd(ids.next_client_id()), ids.next_exchange_client_id(wall));
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(store.open_count(), std::size_t{4});
}

TEST(OmsOrder, CanceledThenLateFillRace) {
    IdGen ids{3, 7};
    const auto cmd = make_cmd(ids.next_client_id());
    Order order{cmd, ids.next_exchange_client_id(UnixNanos{1})};
    const auto cid = cmd.client_order_id;
    ASSERT_TRUE(order.apply(client_event(OrderEventType::Submitted, cid)).has_value());
    ASSERT_TRUE(order.apply(client_event(OrderEventType::Accepted, cid)).has_value());
    ASSERT_TRUE(order.apply(client_event(OrderEventType::Canceled, cid)).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Canceled);
    EXPECT_TRUE(order.is_terminal());

    ASSERT_TRUE(order.apply(fill_event(cid, 4, 6)).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Canceled);
    EXPECT_EQ(order.filled(), QtyLots::from_raw(4));

    ASSERT_TRUE(order.apply(fill_event(cid, 6, 0)).has_value());
    EXPECT_EQ(order.status(), OrderStatus::Filled);
    EXPECT_EQ(order.filled(), QtyLots::from_raw(10));
    EXPECT_EQ(order.leaves(), QtyLots::from_raw(0));
}

TEST(OmsOrder, InvalidTransitionReportsExactPair) {
    IdGen ids{3, 7};
    const auto cmd = make_cmd(ids.next_client_id());
    Order order{cmd, ids.next_exchange_client_id(UnixNanos{1})};
    const auto res = order.apply(client_event(OrderEventType::CancelRejected, cmd.client_order_id));
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().from, OrderStatus::Initialized);
    EXPECT_EQ(res.error().event, OrderEventType::CancelRejected);
    EXPECT_EQ(order.status(), OrderStatus::Initialized);
}

TEST(OmsOrder, FirstExchangeIdWinsAndTimestampsTrack) {
    IdGen ids{3, 7};
    const auto cmd = make_cmd(ids.next_client_id());
    Order order{cmd, ids.next_exchange_client_id(UnixNanos{1})};
    const auto cid = cmd.client_order_id;
    ASSERT_TRUE(order.apply(client_event(OrderEventType::Submitted, cid)).has_value());

    auto ack = client_event(OrderEventType::Accepted, cid);
    ack.exchange_order_id = *ExchangeOrderId::from("FIRST");
    ack.ts_event = UnixNanos{5'000};
    ASSERT_TRUE(order.apply(ack).has_value());
    EXPECT_EQ(order.exchange_order_id().view(), "FIRST");
    EXPECT_EQ(order.ts_updated(), UnixNanos{5'000});

    auto fill = fill_event(cid, 2, 8);
    fill.exchange_order_id = *ExchangeOrderId::from("SECOND");
    fill.ts_event = UnixNanos{0};
    fill.ts_init = UnixNanos{6'000};
    ASSERT_TRUE(order.apply(fill).has_value());
    EXPECT_EQ(order.exchange_order_id().view(), "FIRST");
    EXPECT_EQ(order.ts_updated(), UnixNanos{6'000});
}

TEST(IdGen, ClientIdsMonotoneFromOne) {
    IdGen gen{7, 1};
    const auto a = gen.next_client_id();
    const auto b = gen.next_client_id();
    const auto c = gen.next_client_id();
    EXPECT_EQ(a.strategy_id(), 7);
    EXPECT_EQ(a.seq(), std::uint64_t{1});
    EXPECT_EQ(b.seq(), std::uint64_t{2});
    EXPECT_EQ(c.seq(), std::uint64_t{3});
    EXPECT_TRUE(a.valid());
    EXPECT_LT(a.raw, b.raw);
    EXPECT_LT(b.raw, c.raw);
}

TEST(IdGen, UlidsDistinctAndOrderedWithinSameMillisecond) {
    IdGen gen{7, 1};
    const UnixNanos wall{1'700'000'000'000'000'000ULL};
    Ulid prev = gen.next_exchange_client_id(wall);
    EXPECT_EQ(prev.len, 26);
    for (int i = 0; i < 100; ++i) {
        const Ulid next = gen.next_exchange_client_id(wall);
        EXPECT_LT(prev, next); // strict order implies distinctness
        prev = next;
    }
}

TEST(IdGen, EntropySeededCtorProducesValidIds) {
    IdGen gen{5};
    const auto a = gen.next_exchange_client_id(UnixNanos{1});
    const auto b = gen.next_exchange_client_id(UnixNanos{1});
    EXPECT_EQ(a.len, 26);
    EXPECT_NE(a, b);
    EXPECT_EQ(gen.next_client_id().strategy_id(), 5);
}

TEST(WorkingLots, SplitsBySideAndSkipsOtherInstruments) {
    constexpr auto other = InstrumentId::make(VenueId::Bybit, MarketType::Perp, SymbolId{1});
    OrderStore store{16};
    IdGen gen{1};

    auto buy = make_cmd(gen.next_client_id(), 10);
    ASSERT_TRUE(store.create(buy, gen.next_exchange_client_id(UnixNanos{1})).has_value());

    auto sell = make_cmd(gen.next_client_id(), 4);
    sell.side = Side::Sell;
    ASSERT_TRUE(store.create(sell, gen.next_exchange_client_id(UnixNanos{1})).has_value());

    auto elsewhere = make_cmd(gen.next_client_id(), 99);
    elsewhere.instrument = other;
    ASSERT_TRUE(store.create(elsewhere, gen.next_exchange_client_id(UnixNanos{1})).has_value());

    const auto lots = working_lots(store, k_instrument);
    EXPECT_EQ(lots.buy.raw, 10);
    EXPECT_EQ(lots.sell.raw, 4);
    EXPECT_EQ(working_lots(store, other).buy.raw, 99);
}

TEST(WorkingLots, CountsWhatIsLeftAfterAPartialFill) {
    OrderStore store{16};
    IdGen gen{1};
    const auto cid = gen.next_client_id();
    const auto ecoid = gen.next_exchange_client_id(UnixNanos{1});
    ASSERT_TRUE(store.create(make_cmd(cid, 10), ecoid).has_value());

    // Initialized has no Accepted arm; Submitted comes first
    for (const auto type : {OrderEventType::Submitted, OrderEventType::Accepted}) {
        OrderEvent event{};
        event.type = type;
        event.client_order_id = cid;
        event.exchange_client_order_id = ecoid;
        event.leaves_qty = QtyLots::from_raw(10);
        ASSERT_TRUE(store.apply_event(event).has_value());
    }

    OrderEvent partial{};
    partial.type = OrderEventType::Filled;
    partial.client_order_id = cid;
    partial.exchange_client_order_id = ecoid;
    partial.qty = QtyLots::from_raw(4);
    partial.leaves_qty = QtyLots::from_raw(6);
    ASSERT_TRUE(store.apply_event(partial).has_value());

    EXPECT_EQ(working_lots(store, k_instrument).buy.raw, 6);
}

TEST(WorkingLots, TerminalOrdersAreGoneFromTheCount) {
    OrderStore store{16};
    IdGen gen{1};
    const auto cid = gen.next_client_id();
    const auto ecoid = gen.next_exchange_client_id(UnixNanos{1});
    ASSERT_TRUE(store.create(make_cmd(cid, 10), ecoid).has_value());

    OrderEvent canceled{};
    canceled.type = OrderEventType::Canceled;
    canceled.client_order_id = cid;
    canceled.exchange_client_order_id = ecoid;
    ASSERT_TRUE(store.apply_event(canceled).has_value());

    EXPECT_EQ(working_lots(store, k_instrument).buy.raw, 0);
}

} // namespace
} // namespace raikiri::oms
