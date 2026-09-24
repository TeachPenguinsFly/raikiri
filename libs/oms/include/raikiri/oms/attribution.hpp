#pragma once

// restores our client order id on a venue event and reads leaves before the event is applied

#include <raikiri/domain/order_events.hpp>
#include <raikiri/oms/order_store.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace raikiri::oms {

// What the store knew about an order before this event moved it.
struct Attribution {
    // null when this process never held the order
    const Order* order{nullptr};
    // undef when the order is unknown; check `order` first
    QtyLots prev_leaves{};
};

// resolves the order by any of its three ids and writes ours onto the event
[[nodiscard]] Attribution attribute(OrderEvent& event, const OrderStore& store) noexcept;

// the drop in leaves, never negative; with undef leaves (a pulled fill) `qty` capped by what was
// left
[[nodiscard]] QtyLots filled_delta(const Attribution& who, const OrderEvent& event) noexcept;

// executions already taken from a pull; an empty exec id is always admitted
class SeenFills {
public:
    // true, and recorded, when unseen
    [[nodiscard]] bool admit(std::string_view exec_id, UnixNanos now);

    // forgets what no pull window can reach back over
    void prune(UnixNanos now, std::uint64_t window_ns);

    [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

private:
    std::unordered_map<std::string, std::uint64_t> seen_;
};

} // namespace raikiri::oms
