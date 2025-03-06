#include <raikiri/oms/order_store.hpp>

namespace raikiri::oms {

namespace {

constexpr std::size_t k_npos = static_cast<std::size_t>(-1);

} // namespace

OrderStore::OrderStore(std::size_t capacity) : slots_(capacity) {
    free_.reserve(capacity);
    // Descending so the free-list stack hands out slot 0 first — makes reuse
    // deterministic and test-visible.
    for (std::size_t i = capacity; i > 0; --i) {
        free_.push_back(i - 1);
    }
    by_client_.reserve(capacity);
    by_exchange_client_.reserve(capacity);
    by_exchange_.reserve(capacity);
}

std::expected<Order*, StoreError>
OrderStore::create(const OrderCmd& cmd,
                   const ExchangeClientOrderId& exchange_client_order_id) noexcept {
    if (by_client_.contains(cmd.client_order_id) ||
        by_exchange_client_.contains(exchange_client_order_id)) {
        return std::unexpected{StoreError::Duplicate};
    }
    if (free_.empty()) {
        return std::unexpected{StoreError::Full};
    }
    const std::size_t slot = free_.back();
    free_.pop_back();
    slots_[slot] = Order{cmd, exchange_client_order_id};
    by_client_.emplace(cmd.client_order_id, slot);
    by_exchange_client_.emplace(exchange_client_order_id, slot);
    return &slots_[slot];
}

Order* OrderStore::find(ClientOrderId id) noexcept {
    const auto it = by_client_.find(id);
    return it == by_client_.end() ? nullptr : &slots_[it->second];
}

const Order* OrderStore::find(ClientOrderId id) const noexcept {
    const auto it = by_client_.find(id);
    return it == by_client_.end() ? nullptr : &slots_[it->second];
}

Order* OrderStore::find(const ExchangeClientOrderId& id) noexcept {
    const auto it = by_exchange_client_.find(id);
    return it == by_exchange_client_.end() ? nullptr : &slots_[it->second];
}

const Order* OrderStore::find(const ExchangeClientOrderId& id) const noexcept {
    const auto it = by_exchange_client_.find(id);
    return it == by_exchange_client_.end() ? nullptr : &slots_[it->second];
}

Order* OrderStore::find_by_exchange_id(const ExchangeOrderId& id) noexcept {
    const auto it = by_exchange_.find(id);
    return it == by_exchange_.end() ? nullptr : &slots_[it->second];
}

const Order* OrderStore::find_by_exchange_id(const ExchangeOrderId& id) const noexcept {
    const auto it = by_exchange_.find(id);
    return it == by_exchange_.end() ? nullptr : &slots_[it->second];
}

// First id present wins; there is no fallback to the next id on a lookup
// miss. An event whose leading id we do not know is an event we cannot trust
// to describe an order we do know.
std::size_t OrderStore::route(const OrderEvent& event) const noexcept {
    if (event.client_order_id.valid()) {
        const auto it = by_client_.find(event.client_order_id);
        return it == by_client_.end() ? k_npos : it->second;
    }
    if (!event.exchange_client_order_id.empty()) {
        const auto it = by_exchange_client_.find(event.exchange_client_order_id);
        return it == by_exchange_client_.end() ? k_npos : it->second;
    }
    if (!event.exchange_order_id.empty()) {
        const auto it = by_exchange_.find(event.exchange_order_id);
        return it == by_exchange_.end() ? k_npos : it->second;
    }
    return k_npos;
}

std::expected<const Order*, ApplyError> OrderStore::apply_event(const OrderEvent& event) noexcept {
    const std::size_t slot = route(event);
    if (slot == k_npos) {
        return std::unexpected{ApplyError::NotFound};
    }
    Order& order = slots_[slot];
    const bool was_bound = !order.exchange_order_id().empty();
    if (const auto applied = order.apply(event); !applied) {
        return std::unexpected{ApplyError::BadTransition};
    }
    // Bind before the terminal check so retire() always sees a consistent
    // index (a fill-before-ack can both introduce the venue id and close the
    // order in one event).
    if (!was_bound && !order.exchange_order_id().empty()) {
        by_exchange_.emplace(order.exchange_order_id(), slot);
    }
    if (order.is_terminal()) {
        retire(slot);
    }
    return &order;
}

void OrderStore::retire(std::size_t slot) noexcept {
    Order& order = slots_[slot];
    by_client_.erase(order.client_order_id());
    by_exchange_client_.erase(order.exchange_client_order_id());
    if (!order.exchange_order_id().empty()) {
        by_exchange_.erase(order.exchange_order_id());
    }
    // Slot contents stay intact until create() reuses it — that is the
    // documented grace period for the pointer apply_event() just returned.
    free_.push_back(slot);
}

} // namespace raikiri::oms
