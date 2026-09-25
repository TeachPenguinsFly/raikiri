#pragma once

#include <raikiri/venues/binance/errors.hpp>
#include <raikiri/wire/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace raikiri::venues::binance {

// x decides which event is emitted; X is carried for reconciliation only
struct WireOrderUpdate {
    std::string_view symbol;
    std::string_view client_order_id;   // c: our ExchangeClientOrderId
    std::string_view exec_type;         // x: NEW, CANCELED, EXPIRED, TRADE, ...
    std::string_view order_status;      // X: NEW, PARTIALLY_FILLED, FILLED, ...
    std::string_view side;              // S
    std::string_view order_type;        // o
    std::string_view time_in_force;     // f
    std::string_view order_qty;         // q
    std::string_view order_price;       // p
    std::string_view last_filled_qty;   // l
    std::string_view cum_filled_qty;    // z
    std::string_view last_filled_price; // L
    std::string_view commission;        // n — absent unless this frame is a fill
    std::string_view commission_asset;  // N
    std::uint64_t order_id{0};          // i
    std::uint64_t trade_id{0};          // t
    std::uint64_t trade_ms{0};          // T inside o
    std::uint64_t event_ms{0};          // E at the top level
    bool is_maker{false};               // m
    bool reduce_only{false};            // R
};

[[nodiscard]] std::expected<WireOrderUpdate, WireError> parse_order_update(std::string_view data);

// shared by post and get /fapi/v1/order; openOrders is an array of it
struct WireOrder {
    std::string_view symbol;
    std::string_view client_order_id;
    std::string_view status;
    std::string_view side;
    std::string_view price;
    std::string_view orig_qty;
    std::string_view executed_qty;
    std::uint64_t order_id{0};
    std::uint64_t update_ms{0};
};

[[nodiscard]] std::expected<WireOrder, WireError> parse_order(std::string_view body);

inline constexpr std::size_t k_max_orders = 512;

struct OrderBuffer {
    std::array<WireOrder, k_max_orders> orders{};
};

[[nodiscard]] std::expected<std::span<const WireOrder>, WireError> parse_orders(std::string_view body,
                                                                                OrderBuffer& storage);

// the account's own fill from /fapi/v1/userTrades
struct WireTrade {
    std::string_view symbol;
    std::string_view price;
    std::string_view qty;
    std::string_view commission;
    std::string_view side;
    std::uint64_t trade_id{0};
    std::uint64_t order_id{0};
    std::uint64_t time_ms{0};
    bool maker{false};
};

inline constexpr std::size_t k_max_trades = 1024;

struct TradeBuffer {
    std::array<WireTrade, k_max_trades> trades{};
};

[[nodiscard]] std::expected<std::span<const WireTrade>, WireError> parse_user_trades(std::string_view body,
                                                                                     TradeBuffer& storage);

} // namespace raikiri::venues::binance
