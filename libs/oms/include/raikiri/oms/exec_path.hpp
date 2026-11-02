#pragma once

// one path for every venue event, pushed or pulled, in whatever order it arrives

#include <raikiri/domain/order_events.hpp>
#include <raikiri/oms/attribution.hpp>
#include <raikiri/oms/order_store.hpp>

#include <cstdint>
#include <vector>

namespace raikiri::oms {

// a pull window reaches back minutes, not hours
inline constexpr std::uint64_t k_closed_order_window_ns = 15ULL * 60 * 1'000'000'000ULL;

// an order the store has retired, so that what the venue still owes about it reaches its owner
struct ClosedOrder {
    ClientOrderId id{};
    ExchangeClientOrderId exchange_client_order_id{};
    ExchangeOrderId exchange_order_id{};
    Side side{Side::Buy};
    // lots a fill that raced the close may still book
    QtyLots left{QtyLots::from_raw(0)};
    std::uint64_t at_ns{0};
};

class ClosedOrders {
public:
    void note(const Order& order, QtyLots left, UnixNanos at);

    // by any id the event carries, the way the store routes
    [[nodiscard]] ClosedOrder* find(const OrderEvent& event) noexcept;
    [[nodiscard]] bool holds(ClientOrderId id) const noexcept;

    void prune(UnixNanos now, std::uint64_t window_ns);

    [[nodiscard]] std::size_t size() const noexcept { return closed_.size(); }

private:
    std::vector<ClosedOrder> closed_;
};

struct ExecOutcome {
    // the same execution again with nothing new in it; the event goes no further
    bool repeat{false};
    bool late{false};
    // a fill no order here owns, open or closed
    bool unmatched{false};
    // the state machine refused it; must stay zero
    bool refused{false};
    // null when late or unmatched; valid until the next create
    const Order* order{nullptr};
    QtyLots delta{QtyLots::from_raw(0)};
};

class ExecPath {
public:
    explicit ExecPath(OrderStore& store) noexcept : store_{&store} {}

    // writes our id onto the event; `pulled` fills were admitted by the caller before it built the event
    [[nodiscard]] ExecOutcome apply(OrderEvent& event, bool pulled, UnixNanos now);

    [[nodiscard]] SeenFills& seen() noexcept { return seen_; }

    [[nodiscard]] const SeenFills& seen() const noexcept { return seen_; }

    [[nodiscard]] ClosedOrders& closed() noexcept { return closed_; }

    [[nodiscard]] const ClosedOrders& closed() const noexcept { return closed_; }

private:
    OrderStore* store_;
    SeenFills seen_;
    ClosedOrders closed_;
};

} // namespace raikiri::oms
