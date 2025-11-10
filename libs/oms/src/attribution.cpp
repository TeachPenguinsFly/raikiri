#include <raikiri/oms/attribution.hpp>

#include <algorithm>
#include <cstdint>

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
    // AND LAST, THE VENUE'S OWN ORDER ID. A private socket echoes the id we
    // sent, so the two lookups above settle every pushed event — but a fill
    // read out of a venue's TRADE HISTORY need not carry it at all. WEEX is the
    // case that forces this: its userTrades names `orderId` and nothing of
    // ours, so a pulled fill matched no order, the book never took it, and
    // reconciliation reported a position we had placed ourselves as one the
    // venue held and we did not.
    if (out.order == nullptr && !event.exchange_order_id.empty()) {
        out.order = store.find_by_exchange_id(event.exchange_order_id);
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
    // A FILL THAT STATES NO LEAVES STATES ITS OWN SIZE INSTEAD.
    //
    // A private socket reports what the ORDER has left, and the drop in that is
    // the fill. A fill PULLED from the venue's trade history reports the
    // EXECUTION and says nothing about the order, so there is no drop to take —
    // the execution's own size is the fill, capped by what the order still had.
    //
    // The cap is not cosmetic: it is what stops a pull that arrives twice from
    // taking the same lots twice once the order is finished. It does NOT make
    // the pull idempotent on its own — a partial re-offered while the order
    // still has room would pass — which is why a caller feeding pulled fills
    // admits them through SeenFills first.
    //
    // Without this branch the subtraction below runs on INT64_MIN and overflows.
    if (event.leaves_qty.is_undef()) {
        const auto left = who.prev_leaves.is_undef() ? std::int64_t{0} : who.prev_leaves.raw;
        const auto size = event.qty.is_undef() ? std::int64_t{0} : event.qty.raw;
        const auto taken = std::min(size, left);
        return QtyLots::from_raw(taken > 0 ? taken : 0);
    }
    // The order is known, so its leaves are too — but a book that somehow held
    // an undefined one must not have it read as a number.
    if (who.prev_leaves.is_undef()) {
        return QtyLots::from_raw(0);
    }
    const auto delta = who.prev_leaves.raw - event.leaves_qty.raw;
    // Never negative. Leaves that ROSE mean an amend grew the order, which is
    // not a fill; treating the difference as one would credit a position the
    // venue never gave us.
    return QtyLots::from_raw(delta > 0 ? delta : 0);
}

bool SeenFills::admit(std::string_view exec_id, UnixNanos now) {
    if (exec_id.empty()) {
        return true; // see the header: a venue that mints none must not collapse
    }
    const auto [it, fresh] = seen_.try_emplace(std::string{exec_id}, now.ns);
    if (!fresh) {
        return false;
    }
    return true;
}

void SeenFills::prune(UnixNanos now, std::uint64_t window_ns) {
    if (now.ns <= window_ns) {
        return; // a clock inside the window can drop nothing
    }
    const auto floor = now.ns - window_ns;
    std::erase_if(seen_, [floor](const auto& entry) { return entry.second < floor; });
}

} // namespace raikiri::oms
