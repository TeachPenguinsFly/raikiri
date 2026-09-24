#pragma once

#include <raikiri/common/time.hpp>
#include <raikiri/venues/bybit/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bybit {

// PartiallyFilledCanceled is terminal
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Untriggered,
    Triggered,
    Deactivated,
    Rejected,
    Cancelled,
    PartiallyFilledCanceled,
    Filled,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

struct WireOrderUpdate {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view order_link_id; // our ULID comes back here
    std::string_view side;
    std::string_view order_type;
    std::string_view price;
    std::string_view qty;
    std::string_view leaves_qty;
    std::string_view cum_exec_qty;
    std::string_view avg_price;
    std::string_view reject_reason;
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t updated_ms{0};
};

inline constexpr std::size_t k_max_updates = 64;

struct OrderUpdateBuffer {
    WireOrderUpdate updates[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireOrderUpdate>, WireError>
parse_order_updates(std::string_view frame, OrderUpdateBuffer& storage);

struct WireExecution {
    std::string_view symbol;
    // "Trade" | "Funding" | "Settle" | "BustTrade" | "AdlTrade" | ...
    // funding settlements arrive on this topic with a real qty and price
    std::string_view exec_type;
    std::string_view exec_id;
    std::string_view order_id;
    std::string_view order_link_id;
    std::string_view side;
    std::string_view exec_price;
    std::string_view exec_qty;
    std::string_view exec_fee;
    std::string_view leaves_qty;
    bool is_maker{false};
    std::uint64_t exec_ms{0};
};

struct ExecutionBuffer {
    WireExecution executions[k_max_updates];
};

[[nodiscard]] std::expected<std::span<const WireExecution>, WireError>
parse_executions(std::string_view frame, ExecutionBuffer& storage);

// result of /v5/order/create and /v5/order/cancel
struct WireOrderAck {
    std::string_view order_id;
    std::string_view order_link_id;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view result);

// one entry of /v5/order/realtime
using WireOpenOrder = WireOrderUpdate;

[[nodiscard]] std::expected<std::span<const WireOpenOrder>, WireError>
parse_open_orders(std::string_view result, OrderUpdateBuffer& storage);

} // namespace raikiri::venues::bybit
