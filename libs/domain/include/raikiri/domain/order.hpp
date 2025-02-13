#pragma once

// Order vocabulary shared by the OMS, gateways, and journal.

#include <cstdint>
#include <string_view>

namespace raikiri {

enum class OrderType : std::uint8_t { Limit = 0, Market };

enum class TimeInForce : std::uint8_t { Gtc = 0, Ioc, Fok };

// Bit flags; venues express these as separate booleans on the wire.
enum class OrderFlags : std::uint8_t {
    None = 0,
    PostOnly = 1U << 0,
    ReduceOnly = 1U << 1,
};

[[nodiscard]] constexpr OrderFlags operator|(OrderFlags a, OrderFlags b) noexcept {
    return static_cast<OrderFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr bool has_flag(OrderFlags flags, OrderFlags f) noexcept {
    return (static_cast<std::uint8_t>(flags) & static_cast<std::uint8_t>(f)) != 0;
}

enum class OrderStatus : std::uint8_t {
    Initialized = 0, // exists locally, nothing sent
    Denied,          // risk gate refused; never left the process   [terminal]
    Submitted,       // handed to the gateway / wire
    Accepted,        // venue acknowledged
    Rejected,        // venue refused (or REST error synthesized)   [terminal]
    Triggered,       // stop/conditional became active
    PendingUpdate,   // modify in flight
    PendingCancel,   // cancel in flight
    PartiallyFilled,
    Canceled, // [terminal]
    Expired,  // [terminal]
    Filled,   // [terminal]
};

[[nodiscard]] constexpr bool is_terminal(OrderStatus s) noexcept {
    return s == OrderStatus::Denied || s == OrderStatus::Rejected || s == OrderStatus::Canceled ||
           s == OrderStatus::Expired || s == OrderStatus::Filled;
}

[[nodiscard]] std::string_view to_string(OrderStatus status) noexcept;

} // namespace raikiri
