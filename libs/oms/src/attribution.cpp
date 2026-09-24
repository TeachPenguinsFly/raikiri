#include <raikiri/oms/attribution.hpp>

#include <algorithm>
#include <cstdint>

namespace raikiri::oms {

Attribution attribute(OrderEvent& event, const OrderStore& store) noexcept {
    Attribution out{};
    if (event.client_order_id.raw != 0) {
        out.order = store.find(event.client_order_id);
    }
    // must resolve the same order the store does
    if (out.order == nullptr && !event.exchange_client_order_id.empty()) {
        out.order = store.find(event.exchange_client_order_id);
    }
    // last, the venue's order id; a pulled fill (weex userTrades) may carry only that
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
    // a pulled fill has undef leaves, so its own size is the fill, capped by what the order had
    // left
    if (event.leaves_qty.is_undef()) {
        const auto left = who.prev_leaves.is_undef() ? std::int64_t{0} : who.prev_leaves.raw;
        const auto size = event.qty.is_undef() ? std::int64_t{0} : event.qty.raw;
        const auto taken = std::min(size, left);
        return QtyLots::from_raw(taken > 0 ? taken : 0);
    }
    if (who.prev_leaves.is_undef()) {
        return QtyLots::from_raw(0);
    }
    const auto delta = who.prev_leaves.raw - event.leaves_qty.raw;
    // never negative; leaves that rose mean an amend grew the order
    return QtyLots::from_raw(delta > 0 ? delta : 0);
}

bool SeenFills::admit(std::string_view exec_id, UnixNanos now) {
    if (exec_id.empty()) {
        return true; // a venue that mints no exec id must not collapse to one fill
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
