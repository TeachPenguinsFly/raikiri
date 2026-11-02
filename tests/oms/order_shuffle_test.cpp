#include <raikiri/oms/order.hpp>
#include <raikiri/oms/order_store.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace raikiri::oms {
namespace {

using E = OrderEventType;
using S = OrderStatus;

constexpr InstrumentId k_instrument = InstrumentId::make(VenueId::Binance, MarketType::Perp, SymbolId{1});
constexpr ClientOrderId k_id = ClientOrderId::make(7, 1);
constexpr std::int64_t k_qty = 10;
constexpr std::int64_t k_old_price = 100;
constexpr std::int64_t k_new_price = 90;

OrderCmd create_cmd() {
    OrderCmd cmd{};
    cmd.instrument = k_instrument;
    cmd.client_order_id = k_id;
    cmd.price = PriceTicks::from_raw(k_old_price);
    cmd.qty = QtyLots::from_raw(k_qty);
    cmd.ts_created = UnixNanos{1};
    return cmd;
}

OrderEvent event(E type, std::uint64_t ts) {
    OrderEvent out{};
    out.type = type;
    out.client_order_id = k_id;
    out.instrument = k_instrument;
    out.ts_event = UnixNanos{ts};
    out.ts_init = UnixNanos{ts};
    out.qty = QtyLots::undef();
    out.leaves_qty = QtyLots::undef();
    out.price = PriceTicks::undef();
    return out;
}

OrderEvent ack(std::uint64_t ts) {
    auto out = event(E::Accepted, ts);
    out.exchange_order_id = *ExchangeOrderId::from("venue-1");
    return out;
}

OrderEvent fill(std::int64_t lots, std::int64_t leaves, std::uint64_t ts) {
    auto out = event(E::Filled, ts);
    out.qty = QtyLots::from_raw(lots);
    out.leaves_qty = QtyLots::from_raw(leaves);
    out.price = PriceTicks::from_raw(k_old_price);
    return out;
}

OrderEvent updated(std::uint64_t ts) {
    auto out = event(E::Updated, ts);
    out.price = PriceTicks::from_raw(k_new_price);
    return out;
}

struct Final {
    OrderStatus status{};
    std::int64_t filled{0};
    std::int64_t leaves{0};
    std::int64_t price{0};
    std::string venue_id;

    friend bool operator==(const Final&, const Final&) = default;
};

Final final_of(const Order& order) {
    return {.status = order.status(),
            .filled = order.filled().raw,
            .leaves = order.leaves().raw,
            .price = order.price().raw,
            .venue_id = std::string{order.exchange_order_id().view()}};
}

// a request is ours and precedes its answers, so only orders that keep that are possible
bool possible(const std::vector<OrderEvent>& events) {
    const auto first = [&](E type) {
        const auto it = std::ranges::find(events, type, &OrderEvent::type);
        return it - events.begin();
    };
    const auto has = [&](E type) { return std::ranges::find(events, type, &OrderEvent::type) != events.end(); };
    if (has(E::PendingUpdate) && has(E::Updated) && first(E::Updated) < first(E::PendingUpdate)) {
        return false;
    }
    if (has(E::PendingUpdate) && has(E::ModifyRejected) && first(E::ModifyRejected) < first(E::PendingUpdate)) {
        return false;
    }
    if (has(E::PendingCancel) && has(E::Canceled) && first(E::Canceled) < first(E::PendingCancel)) {
        return false;
    }
    if (has(E::PendingCancel) && has(E::CancelRejected) && first(E::CancelRejected) < first(E::PendingCancel)) {
        return false;
    }
    return true;
}

std::string describe(const std::vector<OrderEvent>& events) {
    std::string out;
    for (const auto& one : events) {
        out += std::string{to_string(one.type)} + " ";
    }
    return out;
}

// every possible arrival order ends where the sent order ends, and none is refused
void expect_converges(std::vector<OrderEvent> script, const Final& expected) {
    std::vector<std::size_t> index(script.size());
    std::iota(index.begin(), index.end(), std::size_t{0});
    std::size_t tried = 0;
    do {
        std::vector<OrderEvent> shuffled;
        shuffled.reserve(index.size());
        for (const auto i : index) {
            shuffled.push_back(script[i]);
        }
        if (!possible(shuffled)) {
            continue;
        }
        ++tried;
        Order order{create_cmd(), ExchangeClientOrderId{}};
        ASSERT_TRUE(order.apply(event(E::Submitted, 1)).has_value());
        for (const auto& one : shuffled) {
            EXPECT_TRUE(order.apply(one).has_value())
                << to_string(one.type) << " refused in " << to_string(order.status()) << " on " << describe(shuffled);
        }
        const auto got = final_of(order);
        EXPECT_EQ(got, expected) << describe(shuffled) << " ended " << to_string(got.status) << " filled " << got.filled
                                 << " leaves " << got.leaves << " price " << got.price << " id '" << got.venue_id
                                 << "'";
        if (testing::Test::HasFailure()) {
            return;
        }
    } while (std::ranges::next_permutation(index).found);
    EXPECT_GT(tried, 0U);
}

TEST(OrderShuffle, TwoFillsAndAnAck) {
    expect_converges({ack(2), fill(3, 7, 3), fill(7, 0, 4)},
                     {.status = S::Filled, .filled = 10, .leaves = 0, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, ThreeFillsLeavesNeverRise) {
    expect_converges(
        {ack(2), fill(2, 8, 3), fill(3, 5, 4), fill(1, 4, 5)},
        {.status = S::PartiallyFilled, .filled = 6, .leaves = 4, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, PartialThenCancel) {
    expect_converges({ack(2), fill(3, 7, 3), event(E::PendingCancel, 4), event(E::Canceled, 5)},
                     {.status = S::Canceled, .filled = 3, .leaves = 7, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, CancelAckAfterTheFullFill) {
    expect_converges({ack(2), fill(10, 0, 3), event(E::PendingCancel, 4), event(E::Canceled, 5)},
                     {.status = S::Filled, .filled = 10, .leaves = 0, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, CancelRefusedThenFilled) {
    expect_converges({ack(2), event(E::PendingCancel, 3), event(E::CancelRejected, 4), fill(10, 0, 5)},
                     {.status = S::Filled, .filled = 10, .leaves = 0, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, AmendAckAfterTheCancel) {
    expect_converges(
        {ack(2), event(E::PendingUpdate, 3), updated(4), event(E::PendingCancel, 5), event(E::Canceled, 6)},
        {.status = S::Canceled, .filled = 0, .leaves = 10, .price = k_new_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, AmendRefusedAroundAFillAndACancel) {
    expect_converges({ack(2),
                      event(E::PendingUpdate, 3),
                      event(E::ModifyRejected, 4),
                      fill(5, 5, 5),
                      event(E::PendingCancel, 6),
                      event(E::Canceled, 7)},
                     {.status = S::Canceled, .filled = 5, .leaves = 5, .price = k_old_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, TheNewestAmendAckSetsThePrice) {
    auto older = updated(4);
    older.price = PriceTicks::from_raw(95);
    expect_converges({ack(2), older, updated(5)},
                     {.status = S::Accepted, .filled = 0, .leaves = 10, .price = k_new_price, .venue_id = "venue-1"});
}

TEST(OrderShuffle, AFillBeforeItsAckStillBindsTheVenueId) {
    OrderStore store{4};
    ASSERT_TRUE(store.create(create_cmd(), ExchangeClientOrderId{}).has_value());
    ASSERT_TRUE(store.apply_event(event(E::Submitted, 1)).has_value());
    ASSERT_TRUE(store.apply_event(fill(3, 7, 2)).has_value());
    ASSERT_TRUE(store.apply_event(ack(3)).has_value());
    EXPECT_NE(store.find_by_exchange_id(*ExchangeOrderId::from("venue-1")), nullptr);
}

TEST(OrderShuffle, AnOlderFillCannotRaiseLeaves) {
    Order order{create_cmd(), ExchangeClientOrderId{}};
    ASSERT_TRUE(order.apply(event(E::Submitted, 1)).has_value());
    ASSERT_TRUE(order.apply(fill(5, 3, 3)).has_value());
    ASSERT_TRUE(order.apply(fill(2, 8, 2)).has_value());
    EXPECT_EQ(order.leaves().raw, 3);
    EXPECT_EQ(order.filled().raw, 7);
}

} // namespace
} // namespace raikiri::oms
