#pragma once

// every order's whole life in the audit trail; on the order path, so stack buffers and the sink's preallocated queue

#include <raikiri/audit/log.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/oms/order.hpp>

#include <string_view>

namespace raikiri::oms {

[[nodiscard]] std::string_view trail_action(OrderEventType type) noexcept;

class OrderTrail {
public:
    explicit OrderTrail(audit::Sink& sink) noexcept : sink_{&sink} {}

    // what this process did: "order.created" and "order.sent"
    void command(std::string_view action, const OrderCmd& cmd, UnixNanos at) noexcept;

    // `after` is the order once the event moved it; null for an event on an order no longer held
    void event(const OrderEvent& event, const Order* after, UnixNanos at) noexcept;

private:
    audit::Sink* sink_;
};

} // namespace raikiri::oms
