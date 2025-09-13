#pragma once

// An order's life on the wire, across both trees: the REST rows, the answer
// to a create or a cancel, and the user-stream push.

#include <raikiri/venues/bingx/errors.hpp>
#include <raikiri/wire/json.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::bingx {

// BingX spells a resting order PENDING on both trees and a cancelled one
// CANCELED on one and CANCELLED on the other (VERIFIED in BingX's own worked
// examples), and its coin-M push has been seen answering "Unknown". Reading
// any of those as a status we recognise is how an order that is still working
// gets adopted as finished.
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

// One row type for both trees. The fields they share are the ones an order
// report is made of; the ones only the perpetual tree states (avgPrice,
// positionSide) are optional here rather than a second struct.
//
// `order_id` is a raw token because BingX mints ids past 2^53 and emits them
// as JSON numbers on one endpoint and quoted strings on another.
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

// The answer to a create or a cancel. Spot puts the order in `data` and the
// perpetual tree wraps it one level deeper in `data.order`; this reads either,
// because the difference is a routing detail and not a schema this adapter
// wants two parsers for.
[[nodiscard]] std::expected<WireOrder, WireError> parse_order_ack(std::string_view body);

// The order push, letter-keyed on both trees. The PERPETUAL tree quotes every
// number and SPOT sends them bare, so each field is read as a raw scalar
// rather than as a string.
//
// `last_qty`/`last_price` are what a fill states; the coin-M copy of this push
// omits both and states only the cumulative, which is why a TRADE without them
// is dropped rather than reported at the average price. This adapter serves
// linear contracts, where BingX documents both.
struct WireOrderUpdate {
    std::string_view symbol;                          // s
    std::string_view client_order_id;                 // c — absent on the spot push
    std::string_view order_id;                        // i
    std::string_view side;                            // S
    std::string_view type;                            // o
    std::string_view exec_type;                       // x: NEW/TRADE/CANCELED/EXPIRED/CALCULATED
    std::string_view price;                           // p
    std::string_view orig_qty;                        // q
    std::string_view last_qty;                        // l
    std::string_view cum_qty;                         // z
    std::string_view last_price;                      // L
    std::string_view fee;                             // n — negative when charged
    std::string_view trade_id;                        // t — spot only; the perpetual push has none
    WireOrderStatus status{WireOrderStatus::Unknown}; // X
    bool is_maker{false};                             // m — spot only
    bool maker_known{false};
    std::uint64_t ts_ms{0}; // T
};

[[nodiscard]] std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view frame);

} // namespace raikiri::venues::bingx
