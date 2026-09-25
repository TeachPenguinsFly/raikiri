#pragma once

#include <raikiri/domain/identifiers.hpp>
#include <raikiri/venues/htx/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::htx {

enum class WireOrderState : std::uint8_t {
    Accepted, // reached the book; nothing filled yet
    PartiallyFilled,
    Filled,
    Canceled, // includes a partial fill that was then cancelled
    Rejected,
    Unknown,
};

// fee is negative on swap and positive on spot; the mapper takes the magnitude
struct WireFill {
    std::string_view exec_id; // swap trade `id`, spot `tradeId`
    std::string_view price;
    std::string_view qty;
    std::string_view fee; // empty when the frame carries none
    bool maker{false};
    std::uint64_t ts_ms{0};
};

// sizes are contracts on swap and base units on spot; spot states left_qty, swap states filled_qty
struct WireOrder {
    std::string_view contract_code;
    std::string_view order_id;
    std::string_view client_order_id; // swap: digits; spot: our ULID
    std::string_view price;
    std::string_view qty;
    std::string_view filled_qty;       // swap trade_volume, spot execAmt
    std::string_view left_qty;         // spot remainAmt; empty on the swap
    std::string_view order_price_type; // swap order_price_type, spot `type`
    bool sell{false};
    WireOrderState state{WireOrderState::Unknown};
    std::uint64_t ts_ms{0};
    // swap socket only; spot fills arrive on trade.clearing
    std::span<const WireFill> fills;
};

struct WireUserTrade {
    std::string_view contract_code;
    std::string_view order_id;
    std::string_view client_order_id;
    // set only by spot trade.clearing; otherwise the caller uses the last order frame
    std::string_view left_qty;
    WireFill fill;
    bool sell{false};
};

// swap pages at 50 and spot caps at 500
inline constexpr std::size_t k_max_orders = 512;

inline constexpr std::size_t k_max_fills_per_frame = 64;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
    std::array<WireFill, k_max_fills_per_frame> fills{};
};

// spot's data is a bare order id string; swap echoes client_order_id
struct WireOrderAck {
    std::string_view order_id;
    std::string_view client_order_id;
};

[[nodiscard]] std::expected<WireOrderAck, WireError> parse_order_ack(std::string_view body, MarketType market);

// swap answers ok even when a cancel failed (see data.errors); spot answers state 0 for an unknown id
struct WireCancelOutcome {
    bool accepted{false};
    std::string_view code; // the venue's own refusal code, when it named one
};

[[nodiscard]] std::expected<WireCancelOutcome, WireError> parse_cancel_outcome(std::string_view body,
                                                                               MarketType market);

// one order per frame with its trades inline
[[nodiscard]] std::expected<WireOrder, WireError> parse_swap_order_push(std::string_view frame, OrderBuffer& storage);

// one order per frame and never a fee
[[nodiscard]] std::expected<WireOrder, WireError> parse_spot_order_push(std::string_view frame);

// the only spot frame that carries transactFee
[[nodiscard]] std::expected<WireUserTrade, WireError> parse_spot_trade_push(std::string_view frame);

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError>
parse_rest_orders(std::string_view body, OrderBuffer& storage, MarketType market);

} // namespace raikiri::venues::htx
