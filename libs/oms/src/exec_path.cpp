#include <raikiri/oms/exec_path.hpp>

#include <algorithm>
#include <ranges>

namespace raikiri::oms {

void ClosedOrders::note(const Order& order, QtyLots left, UnixNanos at) {
    closed_.push_back({.id = order.client_order_id(),
                       .exchange_client_order_id = order.exchange_client_order_id(),
                       .exchange_order_id = order.exchange_order_id(),
                       .side = order.side(),
                       .left = left.is_undef() ? QtyLots::from_raw(0) : left,
                       .at_ns = at.ns});
}

ClosedOrder* ClosedOrders::find(const OrderEvent& event) noexcept {
    for (auto& closed : closed_ | std::views::reverse) {
        if ((event.client_order_id.valid() && closed.id == event.client_order_id) ||
            (!event.exchange_client_order_id.empty() &&
             closed.exchange_client_order_id == event.exchange_client_order_id) ||
            (!event.exchange_order_id.empty() && closed.exchange_order_id == event.exchange_order_id)) {
            return &closed;
        }
    }
    return nullptr;
}

bool ClosedOrders::holds(ClientOrderId id) const noexcept {
    return std::ranges::any_of(closed_, [id](const ClosedOrder& closed) { return closed.id == id; });
}

void ClosedOrders::prune(UnixNanos now, std::uint64_t window_ns) {
    if (now.ns <= window_ns) {
        return;
    }
    const auto floor = now.ns - window_ns;
    std::erase_if(closed_, [floor](const ClosedOrder& closed) { return closed.at_ns < floor; });
}

ExecOutcome ExecPath::apply(OrderEvent& event, bool pulled, UnixNanos now) {
    ExecOutcome out{};
    auto who = attribute(event, *store_);
    if (!pulled && event.type == OrderEventType::Filled && !seen_.admit(event.exec_id.view(), now)) {
        // a pull booked this execution from its size alone; its lower leaves still say what other fills we are owed
        const bool balance = who.order != nullptr && !event.leaves_qty.is_undef() && !who.prev_leaves.is_undef() &&
                             event.leaves_qty.raw < who.prev_leaves.raw;
        if (!balance) {
            out.repeat = true;
            return out;
        }
    }
    auto* gone = who.order == nullptr ? closed_.find(event) : nullptr;
    if (gone != nullptr) {
        event.client_order_id = gone->id;
        event.side = gone->side;
        out.late = true;
    }
    const auto stored = store_->apply_event(event);
    out.refused = !stored.has_value() && stored.error() == ApplyError::BadTransition;
    if (stored.has_value()) {
        out.order = *stored;
        if ((*stored)->is_terminal()) {
            closed_.note(**stored, event.type == OrderEventType::Filled ? QtyLots::from_raw(0) : who.prev_leaves, now);
        }
    }
    if (event.type != OrderEventType::Filled) {
        return out;
    }
    if (gone != nullptr) {
        const auto size = event.qty.is_undef() ? std::int64_t{0} : event.qty.raw;
        out.delta = QtyLots::from_raw(std::max<std::int64_t>(0, std::min(size, gone->left.raw)));
        gone->left = QtyLots::from_raw(gone->left.raw - out.delta.raw);
    } else if (who.order != nullptr) {
        out.delta = filled_delta(who, event);
    } else {
        out.unmatched = true;
    }
    return out;
}

} // namespace raikiri::oms
