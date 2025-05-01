#pragma once

// The live order object: origin snapshot + lifecycle state.
//
// An Order never decides status itself — every event goes through the domain
// transition() table, so the OMS and a journal replay produce byte-identical
// state from the same event stream. What the Order adds on top of the FSM is
// the bookkeeping the FSM was designed to lean on:
//
// - prior_working_: the status to resume when a pending modify/cancel resolves
//   without effect. Kept synced to the last working status the order occupied,
//   INCLUDING a partial fill that lands while a cancel is in flight (status
//   stays PendingCancel, but a later CancelRejected must resume
//   PartiallyFilled, not the pre-cancel Accepted).
// - quantities: the venue's leaves_qty on a fill is authoritative; filled is
//   derived (qty - leaves) rather than summed, so venues that resend fill
//   notifications cannot double-count.

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>

#include <expected>

namespace raikiri::oms {

class Order {
public:
    Order() noexcept = default;

    Order(const OrderCmd& cmd, const ExchangeClientOrderId& exchange_client_order_id) noexcept
        : instrument_{cmd.instrument}, side_{cmd.side}, order_type_{cmd.order_type}, tif_{cmd.tif},
          flags_{cmd.flags}, price_{cmd.price}, qty_{cmd.qty}, filled_{QtyLots::from_raw(0)},
          leaves_{cmd.qty}, client_order_id_{cmd.client_order_id},
          exchange_client_order_id_{exchange_client_order_id}, ts_created_{cmd.ts_created},
          ts_updated_{cmd.ts_created} {}

    [[nodiscard]] std::expected<void, TransitionError> apply(const OrderEvent& event) noexcept {
        const bool full_fill = event.type == OrderEventType::Filled && event.leaves_qty.raw == 0;
        const auto next = transition(status_, event.type, full_fill, prior_working_);
        if (!next) {
            return std::unexpected{next.error()};
        }
        status_ = *next;

        if (event.type == OrderEventType::Filled) {
            leaves_ = event.leaves_qty;
            filled_ = qty_ - leaves_;
        }
        if (exchange_order_id_.empty() && !event.exchange_order_id.empty()) {
            exchange_order_id_ = event.exchange_order_id;
        }
        if (status_ == OrderStatus::Accepted || status_ == OrderStatus::PartiallyFilled ||
            status_ == OrderStatus::Triggered) {
            prior_working_ = status_;
        } else if (event.type == OrderEventType::Filled && !full_fill) {
            prior_working_ = OrderStatus::PartiallyFilled;
        }
        ts_updated_ = event.ts_event.ns != 0 ? event.ts_event : event.ts_init;
        return {};
    }

    [[nodiscard]] bool is_terminal() const noexcept { return raikiri::is_terminal(status_); }

    [[nodiscard]] InstrumentId instrument() const noexcept { return instrument_; }

    [[nodiscard]] Side side() const noexcept { return side_; }

    [[nodiscard]] OrderType order_type() const noexcept { return order_type_; }

    [[nodiscard]] TimeInForce tif() const noexcept { return tif_; }

    [[nodiscard]] OrderFlags flags() const noexcept { return flags_; }

    [[nodiscard]] PriceTicks price() const noexcept { return price_; }

    [[nodiscard]] QtyLots qty() const noexcept { return qty_; }

    [[nodiscard]] QtyLots filled() const noexcept { return filled_; }

    [[nodiscard]] QtyLots leaves() const noexcept { return leaves_; }

    [[nodiscard]] OrderStatus status() const noexcept { return status_; }

    [[nodiscard]] OrderStatus prior_working() const noexcept { return prior_working_; }

    [[nodiscard]] ClientOrderId client_order_id() const noexcept { return client_order_id_; }

    [[nodiscard]] const ExchangeClientOrderId& exchange_client_order_id() const noexcept {
        return exchange_client_order_id_;
    }

    [[nodiscard]] const ExchangeOrderId& exchange_order_id() const noexcept {
        return exchange_order_id_;
    }

    [[nodiscard]] UnixNanos ts_created() const noexcept { return ts_created_; }

    [[nodiscard]] UnixNanos ts_updated() const noexcept { return ts_updated_; }

private:
    InstrumentId instrument_{};
    Side side_{Side::Buy};
    OrderType order_type_{OrderType::Limit};
    TimeInForce tif_{TimeInForce::Gtc};
    OrderFlags flags_{OrderFlags::None};
    OrderStatus status_{OrderStatus::Initialized};
    OrderStatus prior_working_{OrderStatus::Accepted};
    PriceTicks price_{};
    QtyLots qty_{};
    QtyLots filled_{};
    QtyLots leaves_{};
    ClientOrderId client_order_id_{};
    ExchangeClientOrderId exchange_client_order_id_{};
    ExchangeOrderId exchange_order_id_{}; // empty until the venue's ack binds it
    UnixNanos ts_created_{};
    UnixNanos ts_updated_{};
};

} // namespace raikiri::oms
