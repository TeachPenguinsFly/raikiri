#pragma once

// transition() is the only arbiter of what an event does to a status

#include <raikiri/domain/error_class.hpp>
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
    Denied,    // risk gate refusal (never reached the wire)
    Submitted, // OMS handed the order to a gateway
    Accepted,  // venue ack (WS or REST)
    Rejected, // venue reject, also synthesized on a rest error
    Triggered, // conditional order armed
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

// which side of the trade we were on
enum class Liquidity : std::uint8_t {
    Unknown = 0, // the venue did not say
    Maker,
    Taker,
};

[[nodiscard]] std::string_view to_string(Liquidity liquidity) noexcept;

struct TransitionError {
    OrderStatus from{};
    OrderEventType event{};
};

// the only writer of OrderStatus; `resume` is the working status a no-effect modify or cancel
// returns to
[[nodiscard]] std::expected<OrderStatus, TransitionError>
transition(OrderStatus status,
           OrderEventType event,
           bool full_fill = false,
           OrderStatus resume = OrderStatus::Accepted) noexcept;

// pod embedded in shm exec_events and journal frames; 192 bytes
struct OrderEvent {
    OrderEventType type{OrderEventType::Initialized};
    Side side{Side::Buy};
    OrderType order_type{OrderType::Limit};
    TimeInForce tif{TimeInForce::Gtc};
    OrderFlags flags{OrderFlags::None};
    Liquidity liquidity{Liquidity::Unknown};
    // the venue's own reject class and code, set only on rejected kinds
    ErrorClass reject_class{ErrorClass::Terminal};
    std::uint8_t pad_[1]{};

    InstrumentId instrument{};
    ClientOrderId client_order_id{};
    ExchangeClientOrderId exchange_client_order_id{}; // 27 B
    std::uint8_t pad2_[5]{};
    ExchangeOrderId exchange_order_id{}; // 41 B
    ExecId exec_id{};                    // 41 B
    std::uint8_t pad3_[2]{};
    std::int32_t reject_code{0};

    PriceTicks price{};   // limit price or fill price
    QtyLots qty{};        // order qty or fill qty
    QtyLots leaves_qty{}; // remaining after this event
    FixedE9 fee{};        // venue fee for this fill (quote units, e9)
    UnixNanos ts_event{}; // venue timestamp
    UnixNanos ts_init{};  // local ingest timestamp
};

static_assert(sizeof(OrderEvent) <= 192, "OrderEvent is the shm exec_events payload");

// the venue's exec id, else one derived from the fill and prefixed '~', so a resend maps to the
// same id
[[nodiscard]] ExecId
fill_identity(ClientOrderId order, QtyLots leaves, std::string_view venue_exec_id) noexcept;

} // namespace raikiri
