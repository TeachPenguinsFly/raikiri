#pragma once

// every event goes through transition(); prior_working_ is the status a no-effect modify or cancel
// resumes

#include <raikiri/common/fixed_point.hpp>
#include <raikiri/common/time.hpp>
#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>

namespace raikiri::oms {

class Order {
public:
    Order() noexcept = default;

    Order(const OrderCmd& cmd, const ExchangeClientOrderId& exchange_client_order_id) noexcept
        : instrument_{cmd.instrument}, side_{cmd.side}, order_type_{cmd.order_type}, tif_{cmd.tif}, flags_{cmd.flags},
          price_{cmd.price}, qty_{cmd.qty}, filled_{QtyLots::from_raw(0)}, leaves_{cmd.qty},
          client_order_id_{cmd.client_order_id}, exchange_client_order_id_{exchange_client_order_id},
          ts_created_{cmd.ts_created}, ts_updated_{cmd.ts_created} {}

    [[nodiscard]] std::expected<void, TransitionError> apply(const OrderEvent& event) noexcept {
        // leaves only fall: a resend or a fill that overtook another must not hand lots back, or they are booked twice
        auto remaining = leaves_;
        if (event.type == OrderEventType::Filled) {
            if (event.leaves_qty.is_undef()) {
                // a pulled fill has undef leaves, so its own size comes off what was left
                const auto size = event.qty.is_undef() ? std::int64_t{0} : event.qty.raw;
                remaining = QtyLots::from_raw(leaves_.raw > size ? leaves_.raw - size : std::int64_t{0});
            } else {
                remaining = QtyLots::from_raw(std::min(leaves_.raw, event.leaves_qty.raw));
            }
        }
        const bool late = absorbed(status_, event.type);
        if (!late) {
            const bool full_fill = event.type == OrderEventType::Filled && remaining.raw == 0;
            const auto next = transition(status_, event.type, full_fill, prior_working_);
            if (!next) {
                return std::unexpected{next.error()};
            }
            status_ = *next;
            if (status_ == OrderStatus::Accepted || status_ == OrderStatus::PartiallyFilled ||
                status_ == OrderStatus::Triggered) {
                prior_working_ = status_;
            } else if (event.type == OrderEventType::Filled && !full_fill) {
                prior_working_ = OrderStatus::PartiallyFilled;
            }
        }
        // a fill reported after a refusal moves nothing; one after a cancel still counts
        if (event.type == OrderEventType::Filled && status_ != OrderStatus::Rejected &&
            status_ != OrderStatus::Denied) {
            if (event.liquidity == Liquidity::Internal) {
                internal_filled_ = QtyLots::from_raw(internal_filled_.raw + (leaves_.raw - remaining.raw));
            }
            leaves_ = remaining;
            filled_ = qty_ - leaves_;
        }
        if (exchange_order_id_.empty() && !event.exchange_order_id.empty()) {
            exchange_order_id_ = event.exchange_order_id;
        }
        const auto at = event.ts_event.ns != 0 ? event.ts_event : event.ts_init;
        if (event.type == OrderEventType::Updated && !event.price.is_undef() && at.ns >= ts_priced_.ns) {
            price_ = event.price;
            ts_priced_ = at;
        }
        if (event.type == OrderEventType::PendingCancel) {
            ts_cancel_sent_ = event.ts_init;
        }
        if (event.type == OrderEventType::PendingUpdate && !late) {
            pending_price_ = event.price;
        }
        ts_updated_ = UnixNanos{std::max(ts_updated_.ns, at.ns)};
        ++version_;
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

    // the size the venue was told: lots crossed inside the desk never reached it
    [[nodiscard]] QtyLots venue_qty() const noexcept { return QtyLots::from_raw(qty_.raw - internal_filled_.raw); }

    [[nodiscard]] QtyLots leaves() const noexcept { return leaves_; }

    [[nodiscard]] OrderStatus status() const noexcept { return status_; }

    [[nodiscard]] OrderStatus prior_working() const noexcept { return prior_working_; }

    [[nodiscard]] ClientOrderId client_order_id() const noexcept { return client_order_id_; }

    [[nodiscard]] const ExchangeClientOrderId& exchange_client_order_id() const noexcept {
        return exchange_client_order_id_;
    }

    [[nodiscard]] const ExchangeOrderId& exchange_order_id() const noexcept { return exchange_order_id_; }

    [[nodiscard]] UnixNanos ts_created() const noexcept { return ts_created_; }

    [[nodiscard]] UnixNanos ts_updated() const noexcept { return ts_updated_; }

    [[nodiscard]] UnixNanos ts_cancel_sent() const noexcept { return ts_cancel_sent_; }

    [[nodiscard]] PriceTicks pending_price() const noexcept { return pending_price_; }

    // moves with every event this order takes
    [[nodiscard]] std::uint64_t version() const noexcept { return version_; }

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
    QtyLots internal_filled_{QtyLots::from_raw(0)};
    ClientOrderId client_order_id_{};
    ExchangeClientOrderId exchange_client_order_id_{};
    ExchangeOrderId exchange_order_id_{}; // empty until the venue's ack binds it
    UnixNanos ts_created_{};
    UnixNanos ts_updated_{};
    UnixNanos ts_priced_{};
    UnixNanos ts_cancel_sent_{};
    PriceTicks pending_price_{};
    std::uint64_t version_{0};
};

} // namespace raikiri::oms
