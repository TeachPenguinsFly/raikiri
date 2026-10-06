#pragma once

// a half-filled contract order is NEW; spot says PARTIALLY_FILLED, and either way leaves decides
// order ids arrive as a string or an 18-digit number, so they are read as raw text

#include <raikiri/venues/weex/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::weex {

enum class WireOrderStatus : std::uint8_t {
    New, // working, partly filled or not
    Pending,
    Untriggered, // a conditional order that has not armed
    Unactivated,
    Filled,
    Canceled,
    Canceling, // the venue has accepted a cancel and not finished it
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status_from(std::string_view text) noexcept;

// one row of GET /capi/v3/openOrders
struct WireOrder {
    std::string_view symbol;
    std::string_view order_id;        // orderId, read as text; see the note above
    std::string_view client_order_id; // clientOrderId: our ULID comes back here
    std::string_view side;            // BUY | SELL
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty; // cumulative, so leaves is a subtraction
    WireOrderStatus status{WireOrderStatus::Unknown};
    std::uint64_t update_ms{0};
};

// the venue's limit caps a page at 100
inline constexpr std::size_t k_max_orders = 128;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

// bare array, no envelope
[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

// one row alone, as GET /api/v3/order answers
[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

// a refused order still returns 200 with success false and a string errorCode
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_order_id; // clientOrderId on a place, origClientOrderId on a cancel
    std::string_view error_code;
    std::string_view error_message;
    bool success{false};
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body);

// batch cancel answers a bare array, one entry per order; mixed success is normal
inline constexpr std::size_t k_max_acks = 16;

struct OrderAckBuffer {
    std::array<WireOrderAck, k_max_acks> acks{};
};

[[nodiscard]] std::expected<std::span<const WireOrderAck>, WireError> parse_order_acks(std::string_view body,
                                                                                       OrderAckBuffer& storage);

// the string errorCode weex returns for an order it no longer has
[[nodiscard]] bool is_benign_cancel_error(std::string_view error_code) noexcept;

// fills carry only the venue's orderId
struct WireFill {
    std::string_view symbol;
    std::string_view fill_id;  // id
    std::string_view order_id; // orderId
    std::string_view side;     // BUY | SELL
    std::string_view price;
    std::string_view qty;        // base asset
    std::string_view commission; // positive is charged
    bool maker{false};
    std::uint64_t fill_ms{0};
};

// the venue's limit caps a page at 100
inline constexpr std::size_t k_max_fills = 128;

struct FillBuffer {
    std::array<WireFill, k_max_fills> fills{};
};

[[nodiscard]] std::expected<std::span<const WireFill>, WireError> parse_fills(std::string_view body,
                                                                              FillBuffer& storage);

} // namespace raikiri::venues::weex
