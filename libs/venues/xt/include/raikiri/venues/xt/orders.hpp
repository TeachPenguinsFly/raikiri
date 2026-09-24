#pragma once

// spot sockets use two-letter keys (s, oq, sd, st) where rest uses long names; decimals arrive quoted or bare

#include <raikiri/venues/xt/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::xt {

// PARTIALLY_CANCELED maps to a cancel; its fill was already reported
enum class WireOrderStatus : std::uint8_t {
    New,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
    Expired,
    Unknown,
};

[[nodiscard]] WireOrderStatus order_status(std::string_view text) noexcept;

// xt caps size at 100 per read
inline constexpr std::size_t k_max_orders = 512;

// orderSide is the direction; positionSide stays LONG on a sell that closes a long
struct WireFuturesOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view orig_qty; // contracts
    std::string_view executed_qty; // contracts
    std::string_view avg_price;
    std::string_view order_side;    // BUY | SELL
    std::string_view position_side; // LONG | SHORT
    std::string_view order_type;    // LIMIT | MARKET
    std::string_view time_in_force; // GTC | IOC | FOK | GTX
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t created_ms{0};
    std::uint64_t updated_ms{0};
};

struct FuturesOrderBuffer {
    std::array<WireFuturesOrder, k_max_orders> orders{};
};

// trade-list sends execId and takerMaker text, the socket an isMaker bool; liquidity_known says if either came
struct WireFuturesDeal {
    std::string_view symbol;
    std::string_view exec_id; // empty on the socket push
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view price;
    std::string_view quantity; // contracts
    std::string_view fee;
    std::string_view order_side; // empty on /order/trade-list
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct FuturesDealBuffer {
    std::array<WireFuturesDeal, k_max_orders> deals{};
};

// result as an array or result.items
[[nodiscard]] std::expected<std::span<const WireFuturesOrder>, WireError>
parse_futures_orders(std::string_view body, FuturesOrderBuffer& storage);

// `data` on the socket push, `result` on the single-order REST read.
[[nodiscard]] std::expected<WireFuturesOrder, WireError> parse_futures_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireFuturesDeal>, WireError>
parse_futures_deals(std::string_view body, FuturesDealBuffer& storage);

[[nodiscard]] std::expected<WireFuturesDeal, WireError> parse_futures_deal(std::string_view frame);

// result is a bare id string on futures create/cancel and an object with orderId elsewhere
[[nodiscard]] std::expected<std::string_view, WireError> parse_order_ack(std::string_view body);

struct WireSpotOrder {
    std::string_view symbol;
    std::string_view order_id;
    std::string_view client_order_id;
    std::string_view side; // BUY | SELL
    std::string_view type; // LIMIT | MARKET
    std::string_view time_in_force;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::string_view leaving_qty; // stated outright by xt
    std::string_view avg_price;
    std::string_view fee;
    WireOrderStatus state{WireOrderStatus::Unknown};
    std::uint64_t ts_ms{0};
};

struct SpotOrderBuffer {
    std::array<WireSpotOrder, k_max_orders> orders{};
};

// takerMaker is the account's side; on the socket it is tm, 1 taker and 2 maker
struct WireSpotTrade {
    std::string_view symbol;
    std::string_view trade_id;
    std::string_view order_id;
    std::string_view side; // BUY | SELL; empty on the socket push
    std::string_view price;
    std::string_view quantity;
    std::string_view fee;
    bool maker{false};
    bool liquidity_known{false};
    std::uint64_t ts_ms{0};
};

struct SpotTradeBuffer {
    std::array<WireSpotTrade, k_max_orders> trades{};
};

// result as an array (open-order) or result.items (history/trade)
[[nodiscard]] std::expected<std::span<const WireSpotOrder>, WireError>
parse_spot_orders(std::string_view body, SpotOrderBuffer& storage);

[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order(std::string_view body);

[[nodiscard]] std::expected<std::span<const WireSpotTrade>, WireError>
parse_spot_trades(std::string_view body, SpotTradeBuffer& storage);

// socket pushes use the two-letter keys
[[nodiscard]] std::expected<WireSpotOrder, WireError> parse_spot_order_push(std::string_view frame);

[[nodiscard]] std::expected<WireSpotTrade, WireError> parse_spot_trade_push(std::string_view frame);

} // namespace raikiri::venues::xt
