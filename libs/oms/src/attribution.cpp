#include <raikiri/oms/attribution.hpp>

namespace raikiri::oms {

Attribution attribute(OrderEvent& event, const OrderStore& store) noexcept {
    Attribution out{};
    // Our own id first when the event carries one — a local transition does.
    if (event.client_order_id.raw != 0) {
        out.order = store.find(event.client_order_id);
    }
    // Then the id the venue knows. The store resolves either; this has to
    // resolve the same one, or the book never learns what the store just did.
    if (out.order == nullptr && !event.exchange_client_order_id.empty()) {
        out.order = store.find(event.exchange_client_order_id);
    }
    if (out.order == nullptr) {
        return out;
    }
    out.prev_leaves = out.order->leaves();
    event.client_order_id = out.order->client_order_id();
    return out;
}

QtyLots filled_delta(const Attribution& who, const OrderEvent& event) noexcept {
    if (who.order == nullptr) {
        return QtyLots::from_raw(0);
    }
    const auto delta = who.prev_leaves.raw - event.leaves_qty.raw;
    // Never negative. Leaves that ROSE mean an amend grew the order, which is
    // not a fill; treating the difference as one would credit a position the
    // venue never gave us.
    return QtyLots::from_raw(delta > 0 ? delta : 0);
}

} // namespace raikiri::oms
