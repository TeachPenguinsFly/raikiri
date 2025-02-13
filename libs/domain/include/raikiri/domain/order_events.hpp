#pragma once

// Order lifecycle events and the status transition table.
//
// Every change to an order is an event; an order's state is reconstructable
// by replaying its events from Initialized (the journal makes that replay
// real, not theoretical). Adapters never invent status changes — they emit
// events, and `transition()` is the single arbiter of what an event does to
// a status. Any (status, event) pair not listed is a venue-protocol violation
// we want to hear about loudly, not paper over.
//
// The table encodes venue reality, not an idealized lifecycle — see the
// per-arm comments (fills before acks, cancel/fill races, cancels that
// resolve after expiry). Sourced from years of the Go system trading on
// these venues plus nautilus_trader's equivalent table.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>

#include <cstdint>
#include <expected>
#include <string_view>

namespace raikiri {

enum class OrderEventType : std::uint8_t {
    Initialized = 0,
    Denied,      // risk gate refusal (never reached the wire)
    Submitted,   // OMS handed the order to a gateway
    Accepted,    // venue ack (WS or REST)
    Rejected,    // venue reject; ALSO synthesized locally on REST error —
                 // venues do not push WS notifications for REST-level rejects
    Triggered,   // conditional order armed
    PendingUpdate,
    Updated,        // modify confirmed
    ModifyRejected, // modify refused; order continues in its prior state
    PendingCancel,
    Canceled,
    CancelRejected, // cancel refused; order continues in its prior state
    Expired,        // IOC/FOK remainder or venue-side expiry
    Filled,         // partial or full; fullness decided by leaves_qty
    ReconGap,       // reconciliation found evidence of missed events
    ReconClosed,    // reconciliation: order no longer exists on the venue
};

[[nodiscard]] std::string_view to_string(OrderEventType type) noexcept;

struct TransitionError {
    OrderStatus from{};
    OrderEventType event{};
};

// Pure transition function; the only writer of OrderStatus anywhere.
//
// `full_fill` matters only for Filled events: true when leaves_qty reaches
// zero. `resume` is the working status to return to when a pending
// modify/cancel resolves without effect (Updated, ModifyRejected,
// CancelRejected): the order object passes the status it remembered before
// entering the pending state (Accepted, PartiallyFilled or Triggered).
// ReconGap never changes status (audit only); ReconClosed force-closes.
[[nodiscard]] std::expected<OrderStatus, TransitionError>
transition(OrderStatus status, OrderEventType event, bool full_fill = false,
           OrderStatus resume = OrderStatus::Accepted) noexcept;

// The wire/journal-facing event record. POD: embedded in shm exec_events and
// SBE journal frames. 192 bytes, layout asserted where it enters shm.
struct OrderEvent {
    OrderEventType type{OrderEventType::Initialized};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    OrderFlags flags{OrderFlags::None};
    std::uint8_t liquidity{0}; // 0 unknown, 1 maker, 2 taker
    std::uint8_t pad_[2]{};

    InstrumentId instrument{};
    ClientOrderId client_order_id{};
    ExchangeClientOrderId exchange_client_order_id{}; // 27 B
    std::uint8_t pad2_[5]{};
    ExchangeOrderId exchange_order_id{}; // 41 B
    ExecId exec_id{};                    // 41 B
    std::uint8_t pad3_[6]{};

    PriceTicks price{};        // limit price or fill price
    QtyLots qty{};             // order qty or fill qty
    QtyLots leaves_qty{};      // remaining after this event
    FixedE9 fee{};             // venue fee for this fill (quote units, e9)
    UnixNanos ts_event{};      // venue timestamp
    UnixNanos ts_init{};       // local ingest timestamp
};

static_assert(sizeof(OrderEvent) <= 192, "OrderEvent is the shm exec_events payload");

} // namespace raikiri
