#include <raikiri/oms/exec_path.hpp>
#include <raikiri/oms/id_gen.hpp>
#include <raikiri/oms/kernel_gate.hpp>
#include <raikiri/oms/order_commands.hpp>
#include <raikiri/oms/repeat_guard.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace raikiri::oms {
namespace {

using E = OrderEventType;

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
constexpr ClientOrderId k_id = ClientOrderId::make(7, 1);
constexpr UnixNanos k_now{5'000'000'000'000ULL};

ExchangeClientOrderId ecoid(std::uint64_t seed) {
    IdGen ids{1, seed};
    return ids.next_exchange_client_id(UnixNanos{1'000'000'000'000'000'000ULL});
}

OrderCmd create_cmd(ClientOrderId id = k_id, std::uint64_t seed = 11) {
    OrderCmd cmd{};
    cmd.instrument = k_instrument;
    cmd.client_order_id = id;
    cmd.exchange_client_order_id = ecoid(seed);
    cmd.price = PriceTicks::from_raw(100);
    cmd.qty = QtyLots::from_raw(10);
    cmd.ts_created = UnixNanos{1'000};
    return cmd;
}

// what a venue says: its own ids only, so attribution has work to do
OrderEvent venue_event(E type) {
    OrderEvent out{};
    out.type = type;
    out.instrument = k_instrument;
    out.exchange_client_order_id = ecoid(11);
    out.exchange_order_id = *ExchangeOrderId::from("V1");
    out.ts_event = UnixNanos{2'000};
    out.ts_init = UnixNanos{2'000};
    return out;
}

OrderEvent pushed_fill(std::string_view exec, std::int64_t lots, std::int64_t leaves) {
    auto out = venue_event(E::Filled);
    out.exec_id = *ExecId::from(exec);
    out.qty = QtyLots::from_raw(lots);
    out.leaves_qty = QtyLots::from_raw(leaves);
    out.price = PriceTicks::from_raw(100);
    return out;
}

// a pull knows the execution and the venue's order id, nothing about leaves
OrderEvent pulled_fill(std::string_view exec, std::int64_t lots) {
    OrderEvent out{};
    out.type = E::Filled;
    out.instrument = k_instrument;
    out.exchange_order_id = *ExchangeOrderId::from("V1");
    out.exec_id = *ExecId::from(exec);
    out.qty = QtyLots::from_raw(lots);
    out.leaves_qty = QtyLots::undef();
    out.price = PriceTicks::from_raw(100);
    out.ts_event = UnixNanos{2'000};
    out.ts_init = UnixNanos{2'000};
    return out;
}

struct Arrival {
    OrderEvent event{};
    bool pulled{false};
};

struct Rig {
    OrderStore store{8};
    ExecPath path{store};
    std::int64_t booked{0};

    Rig() {
        EXPECT_TRUE(store.create(create_cmd(), create_cmd().exchange_client_order_id).has_value());
        OrderEvent submitted{};
        submitted.type = E::Submitted;
        submitted.client_order_id = k_id;
        EXPECT_TRUE(store.apply_event(submitted).has_value());
    }

    void take(Arrival arrival) {
        if (arrival.pulled && !path.seen().admit(arrival.event.exec_id.view(), k_now)) {
            return;
        }
        const auto done = path.apply(arrival.event, arrival.pulled, k_now);
        EXPECT_FALSE(done.refused) << to_string(arrival.event.type);
        booked += done.delta.raw;
    }
};

void expect_books(std::vector<Arrival> script, std::int64_t lots, std::size_t open_after) {
    std::vector<std::size_t> index(script.size());
    std::iota(index.begin(), index.end(), std::size_t{0});
    do {
        Rig rig;
        std::string order;
        for (const auto i : index) {
            rig.take(script[i]);
            order += std::to_string(i) + " ";
        }
        EXPECT_EQ(rig.booked, lots) << "arrival order " << order;
        EXPECT_EQ(rig.store.open_count(), open_after) << "arrival order " << order;
        if (testing::Test::HasFailure()) {
            return;
        }
    } while (std::ranges::next_permutation(index).found);
}

TEST(ExecPath, PushedFillsInAnyOrderBookOnce) {
    expect_books({{pushed_fill("a", 5, 5)}, {pushed_fill("b", 3, 2)}, {pushed_fill("c", 2, 0)}}, 10, 0);
}

TEST(ExecPath, PullOfAFillRacingItsPushBooksOnce) {
    expect_books({{pushed_fill("a", 5, 5)},
                  {pushed_fill("b", 3, 2)},
                  {pushed_fill("c", 2, 0)},
                  {pulled_fill("c", 2), true},
                  {pulled_fill("a", 5), true}},
                 10,
                 0);
}

TEST(ExecPath, AFillAndACancelCrossingOnTheWireBookTheFill) {
    expect_books({{venue_event(E::Accepted)}, {pushed_fill("a", 3, 7)}, {venue_event(E::Canceled)}}, 3, 0);
}

TEST(ExecPath, AFillPulledAfterTheCloseFindsItsOwnerByTheVenueId) {
    Rig rig;
    rig.take({venue_event(E::Accepted)});
    rig.take({venue_event(E::Canceled)});
    auto late = pulled_fill("a", 4);
    late.exchange_client_order_id = {};
    ASSERT_TRUE(rig.path.seen().admit("a", k_now));
    const auto done = rig.path.apply(late, true, k_now);
    EXPECT_TRUE(done.late);
    EXPECT_EQ(done.delta.raw, 4);
    EXPECT_EQ(late.client_order_id, k_id);
}

TEST(ExecPath, AFillPastWhatTheCloseLeftBooksOnlyTheRest) {
    Rig rig;
    rig.take({venue_event(E::Canceled)});
    rig.take({pushed_fill("a", 7, 3)});
    rig.take({pushed_fill("b", 7, 0)});
    EXPECT_EQ(rig.booked, 10);
}

TEST(ExecPath, AnExactRepeatGoesNoFurther) {
    Rig rig;
    rig.take({pushed_fill("a", 5, 5)});
    auto again = pushed_fill("a", 5, 5);
    EXPECT_TRUE(rig.path.apply(again, false, k_now).repeat);
}

TEST(ExecPath, AFillNobodyHoldsIsUnmatched) {
    OrderStore store{2};
    ExecPath path{store};
    auto stray = pushed_fill("a", 5, 5);
    const auto done = path.apply(stray, false, k_now);
    EXPECT_TRUE(done.unmatched);
    EXPECT_EQ(done.delta.raw, 0);
}

OrderShape shape(std::int64_t price = 100, std::uint64_t key = 0, std::uint16_t book = 7) {
    return {.book = book,
            .instrument = k_instrument,
            .side = Side::Buy,
            .price = PriceTicks::from_raw(price),
            .qty = QtyLots::from_raw(10),
            .repeat_key = key};
}

constexpr std::uint64_t k_window = 2'000'000'000ULL;

TEST(RepeatGuard, RefusesTheSameOrderInsideTheWindowOnly) {
    RepeatGuard guard{8};
    guard.sent(shape(), k_id, k_now);
    EXPECT_EQ(guard.held(shape(), UnixNanos{k_now.ns + 1}, k_window), k_id);
    EXPECT_FALSE(guard.held(shape(), UnixNanos{k_now.ns + k_window}, k_window).has_value());
}

TEST(RepeatGuard, ADifferenceInAnyFieldIsAnotherOrder) {
    RepeatGuard guard{8};
    guard.sent(shape(), k_id, k_now);
    EXPECT_FALSE(guard.held(shape(101), k_now, k_window).has_value());
    EXPECT_FALSE(guard.held(shape(100, 0, 8), k_now, k_window).has_value());
    auto sell = shape();
    sell.side = Side::Sell;
    EXPECT_FALSE(guard.held(sell, k_now, k_window).has_value());
    auto bigger = shape();
    bigger.qty = QtyLots::from_raw(11);
    EXPECT_FALSE(guard.held(bigger, k_now, k_window).has_value());
}

TEST(RepeatGuard, ABookThatRepeatsOnPurposeHasNoWindow) {
    RepeatGuard guard{8};
    guard.sent(shape(), k_id, k_now);
    EXPECT_FALSE(guard.held(shape(), k_now, 0).has_value());
}

TEST(RepeatGuard, ASlicingAlgosChildrenDifferByTheirOrdinalButAResendDoesNot) {
    RepeatGuard guard{8};
    guard.sent(shape(100, (std::uint64_t{3} << 32) | 1), k_id, k_now);
    EXPECT_FALSE(guard.held(shape(100, (std::uint64_t{3} << 32) | 2), k_now, k_window).has_value());
    EXPECT_FALSE(guard.held(shape(100, (std::uint64_t{4} << 32) | 1), k_now, k_window).has_value());
    EXPECT_EQ(guard.held(shape(100, (std::uint64_t{3} << 32) | 1), k_now, k_window), k_id);
}

TEST(RepeatGuard, AWithdrawnOrderDoesNotHoldItsShape) {
    RepeatGuard guard{8};
    guard.sent(shape(), k_id, k_now);
    guard.withdrawn(k_id);
    EXPECT_FALSE(guard.held(shape(), k_now, k_window).has_value());
}

TEST(RepeatGuard, ForgetsTheOldestPastItsSlots) {
    RepeatGuard guard{2};
    guard.sent(shape(100), k_id, k_now);
    guard.sent(shape(101), k_id, k_now);
    guard.sent(shape(102), k_id, k_now);
    EXPECT_FALSE(guard.held(shape(100), k_now, k_window).has_value());
    EXPECT_TRUE(guard.held(shape(102), k_now, k_window).has_value());
}

OrderEvent local(E type, UnixNanos at, std::int64_t price = 100) {
    OrderEvent out{};
    out.type = type;
    out.client_order_id = k_id;
    out.price = PriceTicks::from_raw(price);
    out.ts_event = at;
    out.ts_init = at;
    return out;
}

TEST(OrderCommands, ASecondCancelIsANoOpUntilTheFirstLooksLost) {
    Order order{create_cmd(), create_cmd().exchange_client_order_id};
    ASSERT_TRUE(order.apply(local(E::Submitted, k_now)).has_value());
    EXPECT_EQ(cancel_effect(order, k_now), CommandEffect::Send);
    ASSERT_TRUE(order.apply(local(E::PendingCancel, k_now)).has_value());
    EXPECT_EQ(cancel_effect(order, UnixNanos{k_now.ns + 1}), CommandEffect::NoOp);
    EXPECT_EQ(cancel_effect(order, UnixNanos{k_now.ns + k_cancel_resend_ns}), CommandEffect::Send);
}

TEST(OrderCommands, AnAmendOfACancellingOrderOrToThePriceAlreadyAskedIsANoOp) {
    Order order{create_cmd(), create_cmd().exchange_client_order_id};
    ASSERT_TRUE(order.apply(local(E::Submitted, k_now)).has_value());
    ASSERT_TRUE(order.apply(local(E::Accepted, k_now)).has_value());
    EXPECT_EQ(amend_effect(order, PriceTicks::from_raw(100)), CommandEffect::NoOp);
    EXPECT_EQ(amend_effect(order, PriceTicks::from_raw(90)), CommandEffect::Send);
    ASSERT_TRUE(order.apply(local(E::PendingUpdate, k_now, 90)).has_value());
    EXPECT_EQ(amend_effect(order, PriceTicks::from_raw(90)), CommandEffect::NoOp);
    EXPECT_EQ(amend_effect(order, PriceTicks::from_raw(80)), CommandEffect::Send);
    ASSERT_TRUE(order.apply(local(E::PendingCancel, k_now)).has_value());
    EXPECT_EQ(amend_effect(order, PriceTicks::from_raw(80)), CommandEffect::NoOp);
}

TEST(OrdersGone, AnotherMarketsOrderIsNotJudgedByThisTable) {
    OrderStore store{4};
    ASSERT_TRUE(store.create(create_cmd(), create_cmd().exchange_client_order_id).has_value());
    const AccountId spot{.venue = VenueId::Binance, .market = MarketType::Spot};
    const AccountId perp{.venue = VenueId::Binance, .market = MarketType::Perp};
    EXPECT_TRUE(orders_gone(store, spot, {}, k_now).empty());
    EXPECT_EQ(orders_gone(store, perp, {}, k_now).size(), 1U);
}

TEST(OrdersGone, AnOrderStillOnItsWayIsNotJudged) {
    OrderStore store{4};
    auto cmd = create_cmd();
    cmd.ts_created = UnixNanos{k_now.ns - 1'000};
    ASSERT_TRUE(store.create(cmd, cmd.exchange_client_order_id).has_value());
    const AccountId perp{.venue = VenueId::Binance, .market = MarketType::Perp};
    EXPECT_TRUE(orders_gone(store, perp, {}, k_now).empty());
    EXPECT_EQ(orders_gone(store, perp, {}, UnixNanos{k_now.ns + k_recon_order_grace_ns}).size(), 1U);
}

TEST(OrdersGone, AnOrderTheVenueStillHoldsStays) {
    OrderStore store{4};
    const auto cmd = create_cmd();
    ASSERT_TRUE(store.create(cmd, cmd.exchange_client_order_id).has_value());
    const AccountId perp{.venue = VenueId::Binance, .market = MarketType::Perp};
    const std::vector held{cmd.exchange_client_order_id};
    EXPECT_TRUE(orders_gone(store, perp, held, k_now).empty());
}

TEST(KernelGateDoubt, ASnapshotFromBeforeTheDoubtCannotClearIt) {
    KernelGate gate;
    const UnixNanos restarted{1'000};
    gate.on(KernelEvent::Restored, restarted);
    EXPECT_FALSE(gate.may_open());
    EXPECT_FALSE(gate.answers_doubt(UnixNanos{999}));
    EXPECT_TRUE(gate.answers_doubt(UnixNanos{1'000}));
    gate.on(KernelEvent::ReconAgreed, UnixNanos{2'000});
    EXPECT_TRUE(gate.may_open());
}

TEST(KernelGateDoubt, ASecondDoubtMovesTheBarThoughTheStateStays) {
    KernelGate gate;
    gate.on(KernelEvent::ExecOverrun, UnixNanos{1'000});
    gate.on(KernelEvent::ExecOverrun, UnixNanos{5'000});
    EXPECT_FALSE(gate.answers_doubt(UnixNanos{4'000}));
}

TEST(IdGenResume, ARestartDoesNotMintAnIdItAlreadyUsed) {
    IdGen ids{7, 1};
    ids.resume_after(41);
    EXPECT_EQ(ids.next_client_id().seq(), 42U);
    ids.resume_after(5);
    EXPECT_EQ(ids.next_client_id().seq(), 43U);
}

} // namespace
} // namespace raikiri::oms
