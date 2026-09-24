#pragma once

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// pending means new; canceled and cancelled both occur; coin-m may send "Unknown"
enum class WireOrderStatus : std::uint8_t {
    New, // NEW and PENDING: accepted, nothing filled
    PartiallyFilled,
    Filled,
    Canceled,
    Expired,
    Rejected, // spot's FAILED
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_of(std::string_view text) noexcept;

// one row type for both trees; order_id is a raw token since ids exceed 2^53
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side; // BUY/SELL
    std::string_view type; // LIMIT/MARKET/...
    std::string_view price;
    std::string_view avg_price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

inline constexpr std::size_t k_max_orders = 1024;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// data.orders[] on both trees.
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_orders(std::string_view body, OrderBuffer& storage);

// spot puts the order in data and perp in data.order; both are read
[[nodiscard]] std::expected<WireOrder, WireError> parse_order_ack(std::string_view body);

// perp quotes numbers and spot sends them bare; a trade without l/L is dropped
struct WireOrderUpdate {
    std::string_view symbol;                          // s
    std::string_view client_order_id; // c, absent on spot
    std::string_view order_id;                        // i
    std::string_view side;                            // S
    std::string_view type;                            // o
    std::string_view exec_type;                       // x: NEW/TRADE/CANCELED/EXPIRED/CALCULATED
    std::string_view price;                           // p
    std::string_view orig_qty;                        // q
    std::string_view last_qty;                        // l
    std::string_view cum_qty;                         // z
    std::string_view last_price;                      // L
    std::string_view fee; // n, negative when charged
    std::string_view trade_id; // t, spot only
    WireOrderStatus status{WireOrderStatus::Unknown}; // X
    bool is_maker{false}; // m, spot only
    bool maker_known{false};
    std::uint64_t ts_ms{0}; // T
};

[[nodiscard]] std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view frame);

} // namespace raikiri::venues::bingx
