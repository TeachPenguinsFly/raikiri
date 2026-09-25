#pragma once

// terminal orders are retired at once; a pointer from apply_event() stays valid until the next
// create() reuses the slot

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/domain/order_cmd.hpp>
#include <raikiri/domain/order_events.hpp>
#include <raikiri/oms/order.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <unordered_map>
#include <vector>

namespace raikiri::oms {

enum class StoreError : std::uint8_t {
    Full,      // capacity exhausted; retire something or size the store right
    Duplicate, // ClientOrderId or ExchangeClientOrderId already live
};

enum class ApplyError : std::uint8_t {
    NotFound,      // no live order under any id the event carries
    BadTransition, // the FSM refused the (status, event) pair; order untouched
};

class OrderStore {
public:
    static constexpr std::size_t k_default_capacity = 65536;

    explicit OrderStore(std::size_t capacity = k_default_capacity);

    OrderStore(const OrderStore&) = delete;
    OrderStore& operator=(const OrderStore&) = delete;

    [[nodiscard]] std::expected<Order*, StoreError>
    create(const OrderCmd& cmd, const ExchangeClientOrderId& exchange_client_order_id) noexcept;

    [[nodiscard]] Order* find(ClientOrderId id) noexcept;
    [[nodiscard]] const Order* find(ClientOrderId id) const noexcept;
    [[nodiscard]] Order* find(const ExchangeClientOrderId& id) noexcept;
    [[nodiscard]] const Order* find(const ExchangeClientOrderId& id) const noexcept;
    [[nodiscard]] Order* find_by_exchange_id(const ExchangeOrderId& id) noexcept;
    [[nodiscard]] const Order* find_by_exchange_id(const ExchangeOrderId& id) const noexcept;

    [[nodiscard]] std::expected<const Order*, ApplyError> apply_event(const OrderEvent& event) noexcept;

    [[nodiscard]] std::size_t open_count() const noexcept { return slots_.size() - free_.size(); }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    // Iteration order is unspecified.
    template <typename F> void for_each_open(F&& fn) const {
        for (const auto& [id, slot] : by_client_) {
            fn(slots_[slot]);
        }
    }

private:
    [[nodiscard]] std::size_t route(const OrderEvent& event) const noexcept;
    void retire(std::size_t slot) noexcept;

    std::vector<Order> slots_;
    std::vector<std::size_t> free_; // stack of free slot indexes
    std::unordered_map<ClientOrderId, std::size_t> by_client_;
    std::unordered_map<ExchangeClientOrderId, std::size_t> by_exchange_client_;
    std::unordered_map<ExchangeOrderId, std::size_t> by_exchange_;
};

// includes submitted orders the venue has not acknowledged, since those can still fill
struct WorkingLots {
    QtyLots buy{QtyLots::from_raw(0)};
    QtyLots sell{QtyLots::from_raw(0)};
};

[[nodiscard]] WorkingLots working_lots(const OrderStore& store, InstrumentId instrument) noexcept;

} // namespace raikiri::oms
