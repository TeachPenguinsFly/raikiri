#include <raikiri/domain/order_events.hpp>

namespace raikiri {

std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
    case OrderStatus::Initialized:
        return "initialized";
    case OrderStatus::Denied:
        return "denied";
    case OrderStatus::Submitted:
        return "submitted";
    case OrderStatus::Accepted:
        return "accepted";
    case OrderStatus::Rejected:
        return "rejected";
    case OrderStatus::Triggered:
        return "triggered";
    case OrderStatus::PendingUpdate:
        return "pending_update";
    case OrderStatus::PendingCancel:
        return "pending_cancel";
    case OrderStatus::PartiallyFilled:
        return "partially_filled";
    case OrderStatus::Canceled:
        return "canceled";
    case OrderStatus::Expired:
        return "expired";
    case OrderStatus::Filled:
        return "filled";
    }
    return "unknown";
}

std::string_view to_string(OrderEventType type) noexcept {
    switch (type) {
    case OrderEventType::Initialized:
        return "initialized";
    case OrderEventType::Denied:
        return "denied";
    case OrderEventType::Submitted:
        return "submitted";
    case OrderEventType::Accepted:
        return "accepted";
    case OrderEventType::Rejected:
        return "rejected";
    case OrderEventType::Triggered:
        return "triggered";
    case OrderEventType::PendingUpdate:
        return "pending_update";
    case OrderEventType::Updated:
        return "updated";
    case OrderEventType::ModifyRejected:
        return "modify_rejected";
    case OrderEventType::PendingCancel:
        return "pending_cancel";
    case OrderEventType::Canceled:
        return "canceled";
    case OrderEventType::CancelRejected:
        return "cancel_rejected";
    case OrderEventType::Expired:
        return "expired";
    case OrderEventType::Filled:
        return "filled";
    case OrderEventType::ReconGap:
        return "recon_gap";
    case OrderEventType::ReconClosed:
        return "recon_closed";
    }
    return "unknown";
}

// The single arbiter of order status. Every arm that exists encodes something
// a venue actually does; every pair that is absent is a protocol violation we
// refuse to absorb silently.
std::expected<OrderStatus, TransitionError>
transition(OrderStatus status, OrderEventType event, bool full_fill, OrderStatus resume) noexcept {
    using S = OrderStatus;
    using E = OrderEventType;

    const auto invalid = [&]() -> std::expected<OrderStatus, TransitionError> {
        return std::unexpected{TransitionError{status, event}};
    };

    // Audit event: reconciliation noticed missed history. Valid from any
    // status (a gap can be discovered after the order went terminal); the
    // status itself does not move.
    if (event == E::ReconGap) {
        return status;
    }

    switch (status) {
    case S::Initialized:
        switch (event) {
        case E::Denied:
            return S::Denied; // risk gate refused; never reached the wire
        case E::Submitted:
            return S::Submitted;
        case E::Canceled:
            return S::Canceled; // strategy withdrew intent before submit
        case E::ReconClosed:
            return S::Canceled; // journal replay found intent with no submit
        default:
            return invalid();
        }

    case S::Submitted:
        switch (event) {
        case E::Accepted:
            return S::Accepted;
        case E::Rejected:
            return S::Rejected; // includes locally-synthesized REST-error rejects
        case E::Filled:
            // User-stream fills regularly beat the REST ack on bybit/binance.
            return full_fill ? S::Filled : S::PartiallyFilled;
        case E::Canceled:
            return S::Canceled; // cancel raced the ack and won
        case E::Expired:
            return S::Expired; // IOC resolved before the ack arrived
        case E::PendingCancel:
            return S::PendingCancel; // strategy cancels an in-flight order
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::Accepted:
        switch (event) {
        case E::Accepted:
            return S::Accepted; // duplicate ack: REST and WS both report — idempotent
        case E::Rejected:
            return S::Rejected; // late reject: ack first, margin check second (okx)
        case E::Filled:
            return full_fill ? S::Filled : S::PartiallyFilled;
        case E::Canceled:
            return S::Canceled; // venue-initiated (post-only cross, liquidation)
        case E::Expired:
            return S::Expired;
        case E::Triggered:
            return S::Triggered;
        case E::Updated:
            return S::Accepted; // venue-side amend without our pending (price protection)
        case E::PendingUpdate:
            return S::PendingUpdate;
        case E::PendingCancel:
            return S::PendingCancel;
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::Triggered:
        switch (event) {
        case E::Filled:
            return full_fill ? S::Filled : S::PartiallyFilled;
        case E::Canceled:
            return S::Canceled;
        case E::Expired:
            return S::Expired;
        case E::Rejected:
            return S::Rejected; // trigger fired into insufficient margin
        case E::Updated:
            return S::Triggered;
        case E::PendingUpdate:
            return S::PendingUpdate;
        case E::PendingCancel:
            return S::PendingCancel;
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::PendingUpdate:
        switch (event) {
        case E::Updated:
            return resume; // amend confirmed; resume prior working status
        case E::ModifyRejected:
            return resume; // amend refused; order unchanged on the venue
        case E::Accepted:
            return resume; // some venues answer an amend with a fresh ack
        case E::Filled:
            // The old price filled while the amend was in flight.
            return full_fill ? S::Filled : S::PartiallyFilled;
        case E::Canceled:
            return S::Canceled; // amend raced a venue-side cancel
        case E::Expired:
            return S::Expired;
        case E::Rejected:
            return S::Rejected; // venues that implement amend as cancel+replace
        case E::PendingCancel:
            return S::PendingCancel; // cancel overrides the pending modify
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::PendingCancel:
        switch (event) {
        case E::Canceled:
            return S::Canceled;
        case E::CancelRejected:
            return resume; // "too late to cancel" — usually means a fill is coming
        case E::Filled:
            // The classic race: the fill matched before the cancel landed.
            // Partial: stay pending — the cancel is still in flight for the rest.
            return full_fill ? S::Filled : S::PendingCancel;
        case E::Accepted:
            return S::PendingCancel; // late ack of the original order; cancel still out
        case E::Expired:
            return S::Expired; // venue expired it before the cancel landed
        case E::Rejected:
            return S::Rejected; // cancel resolved an order that was never accepted
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::PartiallyFilled:
        switch (event) {
        case E::Filled:
            return full_fill ? S::Filled : S::PartiallyFilled;
        case E::Canceled:
            return S::Canceled; // remainder canceled
        case E::Expired:
            return S::Expired; // remainder expired
        case E::Updated:
            return S::PartiallyFilled;
        case E::PendingUpdate:
            return S::PendingUpdate;
        case E::PendingCancel:
            return S::PendingCancel;
        case E::ReconClosed:
            return S::Canceled;
        default:
            return invalid();
        }

    case S::Canceled:
        switch (event) {
        case E::Filled:
            // Real venue behavior: cancel confirmed, then a fill that matched
            // first arrives. Fills outrank cancels — money moved.
            return full_fill ? S::Filled : S::Canceled;
        default:
            return invalid();
        }

    case S::Expired:
        switch (event) {
        case E::Filled:
            return full_fill ? S::Filled : S::Expired; // same race as Canceled
        default:
            return invalid();
        }

    case S::Denied:
    case S::Rejected:
    case S::Filled:
        return invalid(); // hard-terminal: no event moves these

    }
    return invalid();
}

} // namespace raikiri
